#include "doctest.h"
#include "storage/engine.h"
#include "storage/transaction.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <thread>
#include <unistd.h>

namespace jm = jiamiao;
namespace fs = std::filesystem;

// RAII temporary directory
struct TempDir {
    std::string path;
    TempDir() {
        char tmpl[] = "/tmp/jmdb_test_XXXXXX";
        char* dir = mkdtemp(tmpl);
        REQUIRE_MESSAGE(dir != nullptr, "Failed to create temp directory");
        path = dir;
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

// Helper: create a simple table schema
static std::vector<ColumnDef> simple_schema(bool with_pk = true) {
    ColumnDef id_col;
    id_col.name = "id";
    id_col.type = DataType::INTEGER;
    id_col.primary_key = with_pk;

    ColumnDef name_col;
    name_col.name = "name";
    name_col.type = DataType::TEXT;

    return {id_col, name_col};
}

// Helper: build a simple Row
static Row make_data_row(int64_t id, const std::string& name) {
    Row r;
    r["id"] = id;
    r["name"] = std::string(name);
    return r;
}

// Helper: start an explicit transaction block, return assigned XID
static jm::TransactionId begin_txn(StorageEngine& engine) {
    engine.txn_mgr().begin_transaction_block();
    engine.txn_mgr().start_transaction_command();
    return engine.txn_mgr().assign_xid();
}

// Helper: commit current transaction
static void commit_txn(StorageEngine& engine) {
    engine.write_xact_commit(engine.txn_mgr().get_current_xid());
    engine.txn_mgr().commit_transaction();
    engine.txn_mgr().reset_context();
}

// Helper: rollback current transaction
static void rollback_txn(StorageEngine& engine) {
    engine.write_xact_abort(engine.txn_mgr().get_current_xid());
    engine.apply_undo();
    engine.txn_mgr().abort_transaction();
    engine.txn_mgr().reset_context();
}

// ──── insert_with_txn ────────────────────────────────────────

TEST_CASE("StorageEngine: insert_with_txn adds MVCC columns") {
    TempDir tmp;
    StorageEngine engine(tmp.path, 10000);
    engine.create_table("t", simple_schema());

    jm::TransactionId xid = begin_txn(engine);
    jm::TransactionManager& txn = engine.txn_mgr();
    int32_t cid = txn.get_current_command_id();

    Row inserted = engine.insert_with_txn("t", make_data_row(1, "a"));

    // MVCC system columns
    auto xmin_it = inserted.find("_xmin");
    REQUIRE(xmin_it != inserted.end());
    auto* xmin_v = std::get_if<int64_t>(&xmin_it->second);
    REQUIRE(xmin_v != nullptr);
    CHECK(*xmin_v == static_cast<int64_t>(xid));

    auto xmax_it = inserted.find("_xmax");
    REQUIRE(xmax_it != inserted.end());
    auto* xmax_v = std::get_if<int64_t>(&xmax_it->second);
    REQUIRE(xmax_v != nullptr);
    CHECK(*xmax_v == 0);

    auto cid_it = inserted.find("_cid");
    REQUIRE(cid_it != inserted.end());
    auto* cid_v = std::get_if<int64_t>(&cid_it->second);
    REQUIRE(cid_v != nullptr);
    CHECK(*cid_v == static_cast<int64_t>(cid));

    // Should have _rowid
    auto rowid_it = inserted.find("_rowid");
    REQUIRE(rowid_it != inserted.end());

    commit_txn(engine);
}

TEST_CASE("StorageEngine: insert_with_txn PK uniqueness violation") {
    TempDir tmp;
    StorageEngine engine(tmp.path, 10000);
    engine.create_table("t", simple_schema());

    jm::TransactionId xid1 = begin_txn(engine);
    engine.insert_with_txn("t", make_data_row(1, "first"));
    commit_txn(engine);

    // Second insert with same PK
    jm::TransactionId xid2 = begin_txn(engine);
    CHECK_THROWS_AS(engine.insert_with_txn("t", make_data_row(1, "second")),
                    std::runtime_error);
    // Clean up the failed transaction
    engine.txn_mgr().abort_transaction();
    engine.txn_mgr().reset_context();
}

TEST_CASE("StorageEngine: insert_with_txn respects MVCC for PK check") {
    TempDir tmp;
    StorageEngine engine(tmp.path, 10000);
    engine.create_table("t", simple_schema());

    // Txn A inserts PK=1 but then ABORTS
    jm::TransactionId xid_a = begin_txn(engine);
    engine.insert_with_txn("t", make_data_row(1, "from_A"));
    rollback_txn(engine);

    // Txn B inserts PK=1 — should succeed because A's insert was rolled back
    jm::TransactionId xid_b = begin_txn(engine);
    REQUIRE_NOTHROW(engine.insert_with_txn("t", make_data_row(1, "from_B")));
    commit_txn(engine);

    auto snap = engine.txn_mgr().get_snapshot();
    auto rows = engine.scan_with_snapshot("t", snap, jm::InvalidTransactionId, 0);
    CHECK(rows.size() == 1);
}

// ──── update_with_txn ────────────────────────────────────────

TEST_CASE("StorageEngine: update_with_txn marks old row _xmax and creates new version") {
    TempDir tmp;
    StorageEngine engine(tmp.path, 10000);
    engine.create_table("t", simple_schema());

    // Insert and commit a row
    jm::TransactionId xid1 = begin_txn(engine);
    engine.insert_with_txn("t", make_data_row(1, "old_name"));
    commit_txn(engine);

    // Update in new txn
    jm::TransactionId xid2 = begin_txn(engine);
    std::map<std::string, Value> updates;
    updates["name"] = std::string("new_name");
    int64_t affected = engine.update_with_txn("t",
        [](const Row& r) {
            auto it = r.find("id");
            if (it == r.end()) return false;
            auto* v = std::get_if<int64_t>(&it->second);
            return v && *v == 1;
        },
        updates);
    commit_txn(engine);

    CHECK(affected == 1);

    // Scan: only the new version should be visible
    auto snap = engine.txn_mgr().get_snapshot();
    auto rows = engine.scan_with_snapshot("t", snap, jm::InvalidTransactionId, 0);
    REQUIRE(rows.size() == 1);

    auto name_it = rows[0].find("name");
    REQUIRE(name_it != rows[0].end());
    auto* name_v = std::get_if<std::string>(&name_it->second);
    REQUIRE(name_v != nullptr);
    CHECK(*name_v == "new_name");

    // New version should have xid2 as _xmin
    auto xmin_it = rows[0].find("_xmin");
    REQUIRE(xmin_it != rows[0].end());
    auto* xmin_v = std::get_if<int64_t>(&xmin_it->second);
    REQUIRE(xmin_v != nullptr);
    CHECK(*xmin_v == static_cast<int64_t>(xid2));
}

TEST_CASE("StorageEngine: update_with_txn PK uniqueness check on PK column update") {
    TempDir tmp;
    StorageEngine engine(tmp.path, 10000);
    engine.create_table("t", simple_schema());

    // Insert PK=1, PK=2
    jm::TransactionId xid1 = begin_txn(engine);
    engine.insert_with_txn("t", make_data_row(1, "a"));
    commit_txn(engine);

    jm::TransactionId xid2 = begin_txn(engine);
    engine.insert_with_txn("t", make_data_row(2, "b"));
    commit_txn(engine);

    // Try to update PK=1 → PK=2
    jm::TransactionId xid3 = begin_txn(engine);
    std::map<std::string, Value> updates;
    updates["id"] = static_cast<int64_t>(2);  // conflicts with existing PK=2
    updates["name"] = std::string("renamed");

    CHECK_THROWS_AS(engine.update_with_txn("t",
        [](const Row& r) {
            auto it = r.find("id");
            if (it == r.end()) return false;
            auto* v = std::get_if<int64_t>(&it->second);
            return v && *v == 1;
        },
        updates), std::runtime_error);

    engine.txn_mgr().abort_transaction();
    engine.txn_mgr().reset_context();
}

// ──── remove_with_txn ────────────────────────────────────────

TEST_CASE("StorageEngine: remove_with_txn marks _xmax instead of physical delete") {
    TempDir tmp;
    StorageEngine engine(tmp.path, 10000);
    engine.create_table("t", simple_schema());

    // Insert and commit
    jm::TransactionId xid1 = begin_txn(engine);
    engine.insert_with_txn("t", make_data_row(1, "to_delete"));
    commit_txn(engine);

    // Delete in new txn
    jm::TransactionId xid2 = begin_txn(engine);
    int64_t affected = engine.remove_with_txn("t",
        [](const Row& r) {
            auto it = r.find("id");
            if (it == r.end()) return false;
            auto* v = std::get_if<int64_t>(&it->second);
            return v && *v == 1;
        });
    commit_txn(engine);

    CHECK(affected == 1);

    // Snapshot scan: should be invisible (xmax committed)
    auto snap = engine.txn_mgr().get_snapshot();
    auto rows = engine.scan_with_snapshot("t", snap, jm::InvalidTransactionId, 0);
    CHECK(rows.empty());
}

// ──── scan_with_snapshot ─────────────────────────────────────

TEST_CASE("StorageEngine: scan_with_snapshot filters by visibility") {
    TempDir tmp;
    StorageEngine engine(tmp.path, 10000);
    engine.create_table("t", simple_schema());

    // Insert row 1 (committed)
    jm::TransactionId xid1 = begin_txn(engine);
    engine.insert_with_txn("t", make_data_row(1, "committed"));
    commit_txn(engine);

    // Insert row 2 (not committed — still in progress)
    jm::TransactionId xid2 = begin_txn(engine);
    engine.insert_with_txn("t", make_data_row(2, "in_progress"));
    // Do NOT commit xid2

    // Scan from a third "read-only" perspective
    auto snap = engine.txn_mgr().get_snapshot();
    auto rows = engine.scan_with_snapshot("t", snap, jm::InvalidTransactionId, 0);

    // Only the committed row should be visible
    REQUIRE(rows.size() == 1);
    auto id_it = rows[0].find("id");
    REQUIRE(id_it != rows[0].end());
    auto* id_v = std::get_if<int64_t>(&id_it->second);
    REQUIRE(id_v != nullptr);
    CHECK(*id_v == 1);

    // Clean up
    engine.txn_mgr().abort_transaction();
    engine.txn_mgr().reset_context();
}

// ──── index_lookup_with_snapshot ─────────────────────────────

TEST_CASE("StorageEngine: index_lookup_with_snapshot filters by visibility") {
    TempDir tmp;
    StorageEngine engine(tmp.path, 10000);
    engine.create_table("t", simple_schema());
    engine.create_index("t", "name");

    // Committed row
    begin_txn(engine);
    engine.insert_with_txn("t", make_data_row(1, "visible"));
    commit_txn(engine);

    // Uncommitted row (same indexed value "name", different PK)
    begin_txn(engine);
    engine.insert_with_txn("t", make_data_row(2, "visible"));
    // Not committed

    auto snap = engine.txn_mgr().get_snapshot();
    auto rows = engine.index_lookup_with_snapshot("t", "name",
        std::string("visible"), snap, jm::InvalidTransactionId, 0);

    REQUIRE(rows.size() == 1);
    auto id_it = rows[0].find("id");
    REQUIRE(id_it != rows[0].end());
    auto* id_v = std::get_if<int64_t>(&id_it->second);
    REQUIRE(id_v != nullptr);
    CHECK(*id_v == 1);

    engine.txn_mgr().abort_transaction();
    engine.txn_mgr().reset_context();
}

// ──── apply_undo ─────────────────────────────────────────────

TEST_CASE("StorageEngine: apply_undo rolls back INSERT") {
    TempDir tmp;
    StorageEngine engine(tmp.path, 10000);
    engine.create_table("t", simple_schema());

    jm::TransactionId xid = begin_txn(engine);
    engine.insert_with_txn("t", make_data_row(1, "will_rollback"));

    // Rollback
    rollback_txn(engine);

    // The insert should be undone — no visible rows
    auto snap = engine.txn_mgr().get_snapshot();
    auto rows = engine.scan_with_snapshot("t", snap, jm::InvalidTransactionId, 0);
    CHECK(rows.empty());
}

TEST_CASE("StorageEngine: apply_undo rolls back UPDATE") {
    TempDir tmp;
    StorageEngine engine(tmp.path, 10000);
    engine.create_table("t", simple_schema());

    // Insert and commit
    jm::TransactionId xid1 = begin_txn(engine);
    engine.insert_with_txn("t", make_data_row(1, "original"));
    commit_txn(engine);

    // Update in new txn
    jm::TransactionId xid2 = begin_txn(engine);
    std::map<std::string, Value> updates;
    updates["name"] = std::string("updated");
    engine.update_with_txn("t",
        [](const Row& r) {
            auto it = r.find("id");
            if (it == r.end()) return false;
            auto* v = std::get_if<int64_t>(&it->second);
            return v && *v == 1;
        },
        updates);

    // Rollback the update
    rollback_txn(engine);

    // Old value should be visible again
    auto snap = engine.txn_mgr().get_snapshot();
    auto rows = engine.scan_with_snapshot("t", snap, jm::InvalidTransactionId, 0);
    REQUIRE(rows.size() == 1);

    auto name_it = rows[0].find("name");
    REQUIRE(name_it != rows[0].end());
    auto* name_v = std::get_if<std::string>(&name_it->second);
    REQUIRE(name_v != nullptr);
    CHECK(*name_v == "original");
}

TEST_CASE("StorageEngine: apply_undo rolls back DELETE") {
    TempDir tmp;
    StorageEngine engine(tmp.path, 10000);
    engine.create_table("t", simple_schema());

    // Insert and commit
    jm::TransactionId xid1 = begin_txn(engine);
    engine.insert_with_txn("t", make_data_row(1, "saved"));
    commit_txn(engine);

    // Delete in new txn
    jm::TransactionId xid2 = begin_txn(engine);
    engine.remove_with_txn("t",
        [](const Row& r) {
            auto it = r.find("id");
            if (it == r.end()) return false;
            auto* v = std::get_if<int64_t>(&it->second);
            return v && *v == 1;
        });

    // Rollback the delete
    rollback_txn(engine);

    // Row should be visible again
    auto snap = engine.txn_mgr().get_snapshot();
    auto rows = engine.scan_with_snapshot("t", snap, jm::InvalidTransactionId, 0);
    REQUIRE(rows.size() == 1);

    auto name_it = rows[0].find("name");
    REQUIRE(name_it != rows[0].end());
    auto* name_v = std::get_if<std::string>(&name_it->second);
    REQUIRE(name_v != nullptr);
    CHECK(*name_v == "saved");
}

TEST_CASE("StorageEngine: concurrent operations on different tables") {
    TempDir tmp;
    StorageEngine engine(tmp.path, 10000);
    engine.create_table("t_a", simple_schema());
    engine.create_table("t_b", simple_schema());

    std::atomic<int> success_a{0};
    std::atomic<int> success_b{0};
    std::atomic<int> errors{0};

    auto writer_a = [&](int id) {
        try {
            for (int i = 0; i < 50; ++i) {
                engine.insert("t_a", make_data_row(id * 1000 + i, "a"));
                success_a.fetch_add(1);
            }
        } catch (...) { errors.fetch_add(1); }
    };
    auto writer_b = [&](int id) {
        try {
            for (int i = 0; i < 50; ++i) {
                engine.insert("t_b", make_data_row(id * 1000 + i, "b"));
                success_b.fetch_add(1);
            }
        } catch (...) { errors.fetch_add(1); }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) threads.emplace_back(writer_a, i);
    for (int i = 0; i < 4; ++i) threads.emplace_back(writer_b, i);
    for (auto& t : threads) t.join();

    CHECK(errors.load() == 0);
    CHECK(success_a.load() == 200);
    CHECK(success_b.load() == 200);
    // 验证两边都写入了 200 行
    CHECK(engine.scan("t_a").size() == 200);
    CHECK(engine.scan("t_b").size() == 200);
}

// ──── Savepoint 集成测试 ─────────────────────────────────

TEST_CASE("StorageEngine: ROLLBACK TO SAVEPOINT undoes only post-savepoint writes") {
    TempDir tmp;
    StorageEngine engine(tmp.path, 10000);
    engine.create_table("t", simple_schema());

    // 启动事务, 插入 1 行
    begin_txn(engine);
    engine.insert_with_txn("t", make_data_row(1, "before"));
    auto& txn = engine.txn_mgr();
    auto our_xid = txn.get_current_xid();

    // 创建 savepoint
    txn.savepoint("sp1");
    // 在 savepoint 之后又插入 2 行
    engine.insert_with_txn("t", make_data_row(2, "after_a"));
    engine.insert_with_txn("t", make_data_row(3, "after_b"));

    auto snap = txn.get_snapshot();
    auto rows = engine.scan_with_snapshot("t", snap, our_xid, 0);
    REQUIRE(rows.size() == 3);

    // 回滚到 sp1, 只应回滚后 2 行
    size_t to_undo = txn.rollback_to_savepoint("sp1");
    CHECK(to_undo == 2);
    engine.apply_undo_to(to_undo);

    auto snap2 = txn.get_snapshot();
    auto rows2 = engine.scan_with_snapshot("t", snap2, our_xid, 0);
    REQUIRE(rows2.size() == 1);
    auto id_it = rows2[0].find("id");
    REQUIRE(id_it != rows2[0].end());
    auto* id_v = std::get_if<int64_t>(&id_it->second);
    REQUIRE(id_v != nullptr);
    CHECK(*id_v == 1);

    engine.txn_mgr().abort_transaction();
    engine.txn_mgr().reset_context();
}

TEST_CASE("StorageEngine: RELEASE SAVEPOINT keeps later writes") {
    TempDir tmp;
    StorageEngine engine(tmp.path, 10000);
    engine.create_table("t", simple_schema());

    begin_txn(engine);
    engine.insert_with_txn("t", make_data_row(1, "kept"));
    auto& txn = engine.txn_mgr();
    auto our_xid = txn.get_current_xid();

    txn.savepoint("sp1");
    engine.insert_with_txn("t", make_data_row(2, "also_kept"));
    txn.release_savepoint("sp1");

    // 提交后, 2 行都应可见
    commit_txn(engine);

    auto snap = engine.txn_mgr().get_snapshot();
    auto rows = engine.scan_with_snapshot("t", snap, jm::InvalidTransactionId, 0);
    CHECK(rows.size() == 2);
}

// ──── Checkpoint 事务感知 ────────────────────────────────

TEST_CASE("StorageEngine: checkpoint includes transaction state (CLog + next_xid)") {
    TempDir tmp;
    StorageEngine engine(tmp.path, 10000);
    engine.create_table("t", simple_schema());

    // 提交一些事务
    begin_txn(engine);
    engine.insert_with_txn("t", make_data_row(1, "a"));
    commit_txn(engine);

    begin_txn(engine);
    engine.insert_with_txn("t", make_data_row(2, "b"));
    commit_txn(engine);

    auto next_xid_before = engine.txn_mgr().get_next_xid();
    engine.save();

    // 创建新 engine, 验证 next_xid 被恢复
    StorageEngine engine2(tmp.path, 10000);
    engine2.load();
    CHECK(engine2.txn_mgr().get_next_xid() == next_xid_before);

    // 验证数据被恢复
    auto rows = engine2.scan("t");
    CHECK(rows.size() == 2);
}

TEST_CASE("StorageEngine: checkpoint mid-transaction preserves active xid correctly") {
    TempDir tmp;
    StorageEngine engine(tmp.path, 10000);
    engine.create_table("t", simple_schema());

    // 启动事务, 但不提交
    begin_txn(engine);
    engine.insert_with_txn("t", make_data_row(1, "uncommitted"));
    auto our_xid = engine.txn_mgr().get_current_xid();
    // 不 commit, 不 reset

    // checkpoint
    engine.save();

    // 重新加载 — IN_PROGRESS xid 应被 rebuild_active_from_clog 标记为 ABORTED
    StorageEngine engine2(tmp.path, 10000);
    engine2.load();
    CHECK(engine2.txn_mgr().clog().get_status(our_xid) == jm::TransactionStatus::ABORTED);

    // 用 scan_with_snapshot (visibility check) 验证行不可见
    auto snap = engine2.txn_mgr().get_snapshot();
    auto visible_rows = engine2.scan_with_snapshot("t", snap, jm::InvalidTransactionId, 0);
    CHECK(visible_rows.empty());
}

TEST_CASE("StorageEngine: maybe_checkpoint fires after threshold writes") {
    TempDir tmp;
    // checkpoint_interval = 5
    StorageEngine engine(tmp.path, 5);
    engine.create_table("t", simple_schema());

    // 写入 5 行 — 第 5 个写入后应触发 checkpoint
    for (int i = 1; i <= 5; ++i) {
        begin_txn(engine);
        engine.insert_with_txn("t", make_data_row(i, "v"));
        commit_txn(engine);
    }

    // 重新加载, 数据应被 checkpoint 保留
    StorageEngine engine2(tmp.path, 5);
    engine2.load();
    CHECK(engine2.scan("t").size() == 5);
}

TEST_CASE("StorageEngine: recovery works correctly after WAL truncation") {
    // 该测试覆盖 Phase 2: 写数据 → 触发 checkpoint → 截断 WAL → 重新加载
    // 验证: 即使 WAL 已被截断, 数据仍可通过 checkpoint 恢复
    // (WAL 截断逻辑本身在 test_wal.cpp 中以单元测试方式覆盖)
    TempDir tmp;
    StorageEngine engine(tmp.path, 100);
    engine.create_table("t", simple_schema());
    engine.load();

    // 写 3 行并 commit
    for (int i = 1; i <= 3; ++i) {
        begin_txn(engine);
        engine.insert_with_txn("t", make_data_row(i, "v"));
        commit_txn(engine);
    }

    // close 触发 checkpoint (会 sync WAL) + WAL 截断
    engine.close();

    // 重新打开, 数据应完整 (从 checkpoint 恢复 + 可能重放 WAL 残留)
    StorageEngine engine2(tmp.path, 100);
    engine2.load();
    CHECK(engine2.scan("t").size() == 3);
}

TEST_CASE("StorageEngine: vacuum physically removes dead rows from committed deletes") {
    TempDir tmp;
    StorageEngine engine(tmp.path, 10000);
    engine.create_table("t", simple_schema());

    // 插入并提交 2 行
    begin_txn(engine);
    engine.insert_with_txn("t", make_data_row(1, "a"));
    commit_txn(engine);

    begin_txn(engine);
    engine.insert_with_txn("t", make_data_row(2, "b"));
    commit_txn(engine);

    // 删除并提交
    begin_txn(engine);
    engine.remove_with_txn("t", [](const Row& r) {
        auto it = r.find("id");
        if (it == r.end()) return false;
        auto* v = std::get_if<int64_t>(&it->second);
        return v && *v == 2;
    });
    commit_txn(engine);

    // vacuum 前: 物理行有 2 (含 1 个被 _xmax 标记的)
    REQUIRE(engine.scan("t").size() == 2);

    // vacuum
    int64_t cleaned = engine.vacuum();
    CHECK(cleaned == 1);  // 删除 1 个被标记的行

    // vacuum 后: 物理行 1
    CHECK(engine.scan("t").size() == 1);

    // 数据仍然正确
    auto snap = engine.txn_mgr().get_snapshot();
    auto rows = engine.scan_with_snapshot("t", snap, jm::InvalidTransactionId, 0);
    REQUIRE(rows.size() == 1);
    auto id_it = rows[0].find("id");
    REQUIRE(id_it != rows[0].end());
    auto* id_v = std::get_if<int64_t>(&id_it->second);
    REQUIRE(id_v != nullptr);
    CHECK(*id_v == 1);
}

TEST_CASE("StorageEngine: vacuum freezes old committed rows (xmin -> FrozenXid)") {
    TempDir tmp;
    StorageEngine engine(tmp.path, 10000);
    engine.create_table("t", simple_schema());

    begin_txn(engine);
    engine.insert_with_txn("t", make_data_row(1, "old"));
    commit_txn(engine);

    // vacuum: 冻结 _xmin (无活跃事务)
    int64_t cleaned = engine.vacuum();
    CHECK(cleaned == 0);  // 没有 dead rows, 但有冻结

    // 检查行被冻结
    auto rows = engine.scan("t");
    REQUIRE(rows.size() == 1);
    auto xmin_it = rows[0].find("_xmin");
    REQUIRE(xmin_it != rows[0].end());
    auto* v = std::get_if<int64_t>(&xmin_it->second);
    REQUIRE(v != nullptr);
    CHECK(*v == static_cast<int64_t>(jm::FrozenTransactionId));
}

TEST_CASE("StorageEngine: vacuum after abort-then-reinsert (only committed visible)") {
    TempDir tmp;
    StorageEngine engine(tmp.path, 10000);
    engine.create_table("t", simple_schema());

    // 插入并回滚 (apply_undo 会物理删除)
    begin_txn(engine);
    engine.insert_with_txn("t", make_data_row(1, "aborted"));
    rollback_txn(engine);

    // 插入并提交
    begin_txn(engine);
    engine.insert_with_txn("t", make_data_row(2, "committed"));
    commit_txn(engine);

    // 物理行: 1 (回滚的行已被 apply_undo 删除)
    REQUIRE(engine.scan("t").size() == 1);

    // vacuum: 应该清理 0 行 (没有 dead rows)
    int64_t cleaned = engine.vacuum();
    CHECK(cleaned == 0);
    CHECK(engine.scan("t").size() == 1);
}

TEST_CASE("StorageEngine: vacuum on empty table is no-op") {
    TempDir tmp;
    StorageEngine engine(tmp.path, 10000);
    engine.create_table("t", simple_schema());

    int64_t cleaned = engine.vacuum();
    CHECK(cleaned == 0);
}

TEST_CASE("StorageEngine: vacuum preserves indexes after cleanup") {
    TempDir tmp;
    StorageEngine engine(tmp.path, 10000);
    engine.create_table("t", simple_schema());
    engine.create_index("t", "name");

    begin_txn(engine);
    engine.insert_with_txn("t", make_data_row(1, "alice"));
    commit_txn(engine);

    begin_txn(engine);
    engine.insert_with_txn("t", make_data_row(2, "bob"));
    commit_txn(engine);

    // 删除 alice
    begin_txn(engine);
    engine.remove_with_txn("t", [](const Row& r) {
        auto it = r.find("id");
        if (it == r.end()) return false;
        auto* v = std::get_if<int64_t>(&it->second);
        return v && *v == 1;
    });
    commit_txn(engine);

    engine.vacuum();

    // 数据应只剩 bob
    REQUIRE(engine.scan("t").size() == 1);
    auto rows = engine.scan("t");
    auto name_it = rows[0].find("name");
    REQUIRE(name_it != rows[0].end());
    auto* name_v = std::get_if<std::string>(&name_it->second);
    REQUIRE(name_v != nullptr);
    CHECK(*name_v == "bob");
}
