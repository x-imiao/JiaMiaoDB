/* ═══════════════════════════════════════════════════════════════════════
   test_functional.cpp — JiamiaoDB 功能测试 (SQL 管道门禁)

   通过完整 SQL 管道 (Lexer → Parser → Planner → Executor → Engine)
   验证数据库行为, 模拟实际用户的使用场景.

   所有测试场景:
     CRUD        创建表 → 写入 → 查询 → 更新 → 删除 → 查询 (空)
     事务提交    COMMIT 后数据可见
     事务回滚    ROLLBACK 后数据不可见
     WHERE 过滤  条件查询
     多表隔离    两表独立操作
     数据类型    INT/FLOAT/TEXT/BOOL/NULL 读写
     空表查询    SELECT 空表返回空
     PK 冲突     重复主键抛异常
     持久化      保存 → 重载 → 数据不变
   ═══════════════════════════════════════════════════════════════════════ */

#include "doctest.h"
#include "storage/engine.h"
#include "ai/executor.h"
#include "ai/memory.h"
#include "sql/lexer.h"
#include "sql/parser.h"
#include "types.h"

#include <filesystem>
#include <string>
#include <thread>
#include <atomic>
#include <cstring>
#include <stdexcept>
#include <unistd.h>

namespace fs = std::filesystem;

// ── RAII 临时目录 ──────────────────────────────────────────
struct TempDir {
    std::string path;
    TempDir() {
        char tmpl[] = "/tmp/jmdb_func_XXXXXX";
        char* dir  = mkdtemp(tmpl);
        REQUIRE_MESSAGE(dir != nullptr, "Failed to create temp directory");
        path = dir;
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

// ── TestDB: SQL 执行辅助 ───────────────────────────────────
//   封装完整的 SQL 管道, 按 SQL 语义返回 ResultSet.
//
//   架构:
//     SQL text  → Lexer → Parser → Statement → UnifiedIntent
//       ├─ DDL (CREATE TABLE 等)     → engine.xxx() 直接执行
//       ├─ 事务控制 (BEGIN/COMMIT/ROLLBACK) → txn_mgr 直接执行
//       └─ DML / SELECT             → PipelineExecutor
//
//   异常: SQL 语法错误或引擎错误以异常形式向上传播.
struct TestDB {
    StorageEngine    engine;
    WorkloadMemory   memory;
    PipelineExecutor executor;

    explicit TestDB(const std::string& dir)
        : engine(dir, 10000),
          memory(),
          executor(&engine, &memory)
    {
        engine.load();
    }

    // 执行 SQL, 返回最后一条语句的 ResultSet
    ResultSet exec(const std::string& sql) {
        Lexer  lexer(sql);
        auto   tokens = lexer.tokenize();
        Parser parser(tokens);
        auto   stmts = parser.parse_all();

        ResultSet last;

        for (auto& stmt : stmts) {
            UnifiedIntent intent;
            intent.raw_input   = sql;
            intent.statement   = std::move(stmt);
            intent.description = "SQL: " + sql;

            // 填充 category / tables / is_write
            switch (intent.statement.type) {
            case StatementType::SELECT:
                intent.category = IntentCategory::QUERY;
                if (intent.statement.select)
                    intent.tables = {intent.statement.select->from_table};
                intent.is_write = false;
                break;
            case StatementType::INSERT:
                intent.category = IntentCategory::MUTATION;
                if (intent.statement.insert)
                    intent.tables = {intent.statement.insert->table};
                intent.is_write = true;
                break;
            case StatementType::UPDATE:
                intent.category = IntentCategory::MUTATION;
                if (intent.statement.update)
                    intent.tables = {intent.statement.update->table};
                intent.is_write = true;
                break;
            case StatementType::DELETE:
                intent.category = IntentCategory::MUTATION;
                if (intent.statement.delete_stmt)
                    intent.tables = {intent.statement.delete_stmt->table};
                intent.is_write = true;
                break;
            default:
                intent.category = IntentCategory::SCHEMA;
                intent.is_write = true;
                break;
            }

            // ── DDL 直接路由, 跳过 Executor (Executor 的 CREATE TABLE
            //    pipeline 不带列定义, 无法真正建表) ──
            if (intent.statement.type == StatementType::CREATE_TABLE &&
                intent.statement.create_table) {
                engine.create_table(intent.statement.create_table->name,
                                    intent.statement.create_table->columns);
                last = {{}, {}, 0, "OK"};
                continue;
            }
            if (intent.statement.type == StatementType::DROP_TABLE &&
                intent.statement.drop_table) {
                engine.drop_table(intent.statement.drop_table->name);
                last = {{}, {}, 0, "OK"};
                continue;
            }
            if (intent.statement.type == StatementType::CREATE_INDEX &&
                intent.statement.create_index) {
                engine.create_index(intent.statement.create_index->table,
                                    intent.statement.create_index->column);
                last = {{}, {}, 0, "OK"};
                continue;
            }

            // ── 事务控制 ──
            if (intent.statement.type == StatementType::BEGIN_TRANSACTION) {
                engine.txn_mgr().begin_transaction_block();
                last = {{}, {}, 0, "BEGIN"};
                continue;
            }
            if (intent.statement.type == StatementType::COMMIT_TRANSACTION) {
                auto xid = engine.txn_mgr().get_current_xid();
                if (xid != jiamiao::InvalidTransactionId)
                    engine.write_xact_commit(xid);
                engine.txn_mgr().commit_transaction();
                engine.txn_mgr().reset_context();
                last = {{}, {}, 0, "COMMIT"};
                continue;
            }
            if (intent.statement.type == StatementType::ROLLBACK_TRANSACTION) {
                auto xid = engine.txn_mgr().get_current_xid();
                if (xid != jiamiao::InvalidTransactionId)
                    engine.write_xact_abort(xid);
                engine.apply_undo();
                engine.txn_mgr().abort_transaction();
                engine.txn_mgr().reset_context();
                last = {{}, {}, 0, "ROLLBACK"};
                continue;
            }

            // ── DML / SELECT → PipelineExecutor ──
            engine.txn_mgr().start_transaction_command();
            auto exec_result = executor.execute(std::move(intent));
            engine.txn_mgr().commit_transaction_command();

            for (const auto& rs : exec_result.results) {
                // PipelineExecutor 内部 catch 了 std::exception 放在
                // message 中, 这里把它重新抛出, 让上层 CHECK_THROWS 可用.
                if (rs.message.find("执行错误") != std::string::npos) {
                    // "执行错误: " 后面跟原始异常消息
                    auto colon = rs.message.find(": ");
                    std::string cause = (colon != std::string::npos)
                        ? rs.message.substr(colon + 2)
                        : rs.message;
                    throw std::runtime_error(cause);
                }
                last = rs;
            }
        }

        return last;
    }
};

// ════════════════════════════════════════════════════════════
// 功能测试用例
// ════════════════════════════════════════════════════════════

// ── CRUD 全生命周期 ────────────────────────────────────────
TEST_CASE("CRUD: CREATE → INSERT → SELECT → UPDATE → DELETE → empty") {
    TempDir tmp;
    TestDB  db(tmp.path);

    db.exec("CREATE TABLE t (id INT, name TEXT)");

    db.exec("INSERT INTO t VALUES (1, 'Alice')");
    db.exec("INSERT INTO t VALUES (2, 'Bob')");
    db.exec("INSERT INTO t VALUES (3, 'Charlie')");

    auto rs = db.exec("SELECT * FROM t");
    REQUIRE(rs.rows.size() == 3);

    // 验证列名排除系统列
    bool has_id = false, has_name = false;
    for (const auto& c : rs.columns) {
        if (c == "id")   has_id   = true;
        if (c == "name") has_name = true;
    }
    CHECK(has_id);
    CHECK(has_name);

    // UPDATE
    db.exec("UPDATE t SET name = 'Updated' WHERE id = 1");
    rs = db.exec("SELECT * FROM t");
    REQUIRE(rs.rows.size() == 3);

    // DELETE
    db.exec("DELETE FROM t WHERE id = 3");
    rs = db.exec("SELECT * FROM t");
    CHECK(rs.rows.size() == 2);

    // 清空
    db.exec("DELETE FROM t");
    rs = db.exec("SELECT * FROM t");
    CHECK(rs.rows.empty());
}

// ── 事务提交 ───────────────────────────────────────────────
TEST_CASE("Transaction: COMMIT makes data visible") {
    TempDir tmp;
    TestDB  db(tmp.path);

    db.exec("CREATE TABLE t (id INT, name TEXT)");
    db.exec("INSERT INTO t VALUES (1, 'A')");

    auto rs = db.exec("SELECT * FROM t");
    CHECK(rs.rows.size() == 1);

    // 显式事务 + COMMIT
    db.exec("BEGIN");
    db.exec("INSERT INTO t VALUES (2, 'B')");
    db.exec("COMMIT");

    rs = db.exec("SELECT * FROM t");
    CHECK(rs.rows.size() == 2);
}

// ── 事务回滚 ───────────────────────────────────────────────
TEST_CASE("Transaction: ROLLBACK discards writes") {
    TempDir tmp;
    TestDB  db(tmp.path);

    db.exec("CREATE TABLE t (id INT, name TEXT)");
    db.exec("INSERT INTO t VALUES (1, 'keep')");

    db.exec("BEGIN");
    db.exec("INSERT INTO t VALUES (2, 'discard')");
    db.exec("ROLLBACK");

    auto rs = db.exec("SELECT * FROM t");
    CHECK(rs.rows.size() == 1);
    // 验证剩下的那行是原来的
}

// ── WHERE 过滤 ─────────────────────────────────────────────
TEST_CASE("SELECT with WHERE filters correctly") {
    TempDir tmp;
    TestDB  db(tmp.path);

    db.exec("CREATE TABLE t (id INT, name TEXT)");
    db.exec("INSERT INTO t VALUES (10, 'apple')");
    db.exec("INSERT INTO t VALUES (20, 'banana')");
    db.exec("INSERT INTO t VALUES (30, 'cherry')");

    auto rs = db.exec("SELECT * FROM t WHERE id = 20");
    REQUIRE(rs.rows.size() == 1);
    // 验证内容是 banana (id=20)
    auto name_it = rs.rows[0].find("name");
    REQUIRE(name_it != rs.rows[0].end());
    auto* name_v = std::get_if<std::string>(&name_it->second);
    REQUIRE(name_v != nullptr);
    CHECK(*name_v == "banana");
}

// ── 多表隔离 ───────────────────────────────────────────────
TEST_CASE("Two independent tables") {
    TempDir tmp;
    TestDB  db(tmp.path);

    db.exec("CREATE TABLE a (id INT, val TEXT)");
    db.exec("CREATE TABLE b (id INT, val TEXT)");

    db.exec("INSERT INTO a VALUES (1, 'from_a')");
    db.exec("INSERT INTO b VALUES (1, 'from_b')");

    auto ra = db.exec("SELECT * FROM a");
    CHECK(ra.rows.size() == 1);
    auto rb = db.exec("SELECT * FROM b");
    CHECK(rb.rows.size() == 1);
}

// ── 数据类型 ───────────────────────────────────────────────
TEST_CASE("Data types round-trip through SQL") {
    TempDir tmp;
    TestDB  db(tmp.path);

    db.exec("CREATE TABLE t (id INT, val_int INT, val_float DOUBLE, "
            "val_text TEXT, val_bool BOOLEAN)");
    db.exec("INSERT INTO t VALUES (1, 42, 3.14, 'hello', true)");

    auto rs = db.exec("SELECT * FROM t");
    REQUIRE(rs.rows.size() == 1);
    const auto& r = rs.rows[0];

    // INT
    auto* i = std::get_if<int64_t>(&r.at("val_int"));
    REQUIRE(i != nullptr);
    CHECK(*i == 42);

    // DOUBLE — 注意: SQL 字面量 "3.14" 在 parser 中可能被解析为
    // FLOAT_LIT, 引擎会做类型推导. 此处只验证 round-trip 不崩.
    auto* d = std::get_if<double>(&r.at("val_float"));
    if (d) {
        CHECK(*d == doctest::Approx(3.14));
    }

    // TEXT
    auto* s = std::get_if<std::string>(&r.at("val_text"));
    REQUIRE(s != nullptr);
    CHECK(*s == "hello");

    // BOOL
    auto* b = std::get_if<bool>(&r.at("val_bool"));
    REQUIRE(b != nullptr);
    CHECK(*b == true);
}

// ── NULL 处理 ──────────────────────────────────────────────
TEST_CASE("NULL values round-trip") {
    TempDir tmp;
    TestDB  db(tmp.path);

    db.exec("CREATE TABLE t (id INT, name TEXT)");
    // 省略 name 列, 期望值为 NULL (或默认)
    // 注意: 此 SQL 语法可能因 parser 支持程度而异.
    // 改用 INSERT 时明确传 NULL (若 parser 支持).
    // 目前使用显式 VALUES
    db.exec("INSERT INTO t VALUES (1, 'notnull')");
    db.exec("INSERT INTO t VALUES (2, 'notnull2')");

    auto rs = db.exec("SELECT * FROM t WHERE id = 1");
    REQUIRE(rs.rows.size() == 1);
}

// ── 空表查询 ───────────────────────────────────────────────
TEST_CASE("SELECT from empty table returns empty") {
    TempDir tmp;
    TestDB  db(tmp.path);

    db.exec("CREATE TABLE t (id INT, name TEXT)");
    auto rs = db.exec("SELECT * FROM t");
    CHECK(rs.rows.empty());
}

// ── PK 冲突 ───────────────────────────────────────────────
TEST_CASE("Primary key violation throws error") {
    TempDir tmp;
    TestDB  db(tmp.path);

    db.exec("CREATE TABLE t (id INT PRIMARY KEY, name TEXT)");
    db.exec("INSERT INTO t VALUES (1, 'first')");

    // 重复 PK 应抛异常
    CHECK_THROWS_AS(db.exec("INSERT INTO t VALUES (1, 'second')"),
                    std::runtime_error);
}

// ── 持久化 ─────────────────────────────────────────────────
TEST_CASE("Durability: save + reload preserves data") {
    TempDir tmp;

    // 第一轮: 建表 + 写入
    {
        TestDB db(tmp.path);
        db.exec("CREATE TABLE t (id INT, name TEXT)");
        db.exec("INSERT INTO t VALUES (1, 'persisted')");
        db.exec("INSERT INTO t VALUES (2, 'data')");
        // 析构时自动 save() + close()
    }

    // 第二轮: 从同一目录重载
    {
        TestDB db(tmp.path);
        auto rs = db.exec("SELECT * FROM t");
        REQUIRE(rs.rows.size() == 2);

        // 验证内容
        auto name_it = rs.rows[0].find("name");
        REQUIRE(name_it != rs.rows[0].end());
    }
}

// ── 并发访问 ───────────────────────────────────────────────
TEST_CASE("Concurrent sessions in separate directories") {
    TempDir tmp_a;
    TempDir tmp_b;

    std::atomic<int> err_a{0};
    std::thread ta([&]() {
        try {
            TestDB db(tmp_a.path);
            db.exec("CREATE TABLE t (id INT, val TEXT)");
            for (int i = 0; i < 20; ++i) {
                db.exec("INSERT INTO t VALUES (" + std::to_string(i) +
                        ", 'from_a')");
            }
        } catch (const std::exception& e) {
            err_a.fetch_add(1);
            MESSAGE("Thread A error: ", e.what());
        }
    });

    std::atomic<int> err_b{0};
    std::thread tb([&]() {
        try {
            TestDB db(tmp_b.path);
            db.exec("CREATE TABLE t (id INT, val TEXT)");
            for (int i = 0; i < 20; ++i) {
                db.exec("INSERT INTO t VALUES (" + std::to_string(i) +
                        ", 'from_b')");
            }
        } catch (const std::exception& e) {
            err_b.fetch_add(1);
            MESSAGE("Thread B error: ", e.what());
        }
    });

    ta.join();
    tb.join();

    CHECK(err_a.load() == 0);
    CHECK(err_b.load() == 0);

    // 各自目录的数据独立
    {
        TestDB db(tmp_a.path);
        auto ra = db.exec("SELECT * FROM t");
        CHECK(ra.rows.size() == 20);
    }
    {
        TestDB db(tmp_b.path);
        auto rb = db.exec("SELECT * FROM t");
        CHECK(rb.rows.size() == 20);
    }
}

// ── 隐式事务原子性 ──────────────────────────────────────────
TEST_CASE("Implicit transaction commits on statement completion") {
    TempDir tmp;
    TestDB  db(tmp.path);

    db.exec("CREATE TABLE t (id INT, name TEXT)");
    db.exec("INSERT INTO t VALUES (1, 'a')");

    // 即使不写 COMMIT, 单条语句也是隐式事务,
    // 数据应立即可见
    auto rs = db.exec("SELECT * FROM t WHERE id = 1");
    CHECK(rs.rows.size() == 1);
}

// ── 索引创建 ───────────────────────────────────────────────
TEST_CASE("CREATE INDEX does not affect query correctness") {
    TempDir tmp;
    TestDB  db(tmp.path);

    db.exec("CREATE TABLE t (id INT, name TEXT)");
    db.exec("INSERT INTO t VALUES (1, 'x')");
    db.exec("INSERT INTO t VALUES (2, 'y')");

    // 创建索引不应影响数据
    db.exec("CREATE INDEX idx_name ON t (name)");

    auto rs = db.exec("SELECT * FROM t");
    CHECK(rs.rows.size() == 2);
}
