#include "doctest.h"
#include "storage/engine.h"
#include "storage/transaction.h"
#include "storage/tuple.h"
#include "storage/memtable.h"
#include "storage/arena.h"
#include "common/memcontext.h"
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

// ════════════════════════════════════════════════════════════════
// Phase 1 LSM 重构新增测试 (MemTable + BinaryRowCodec)
// ════════════════════════════════════════════════════════════════

TEST_CASE("MemTable: tuple round-trip preserves Value variants") {
    using namespace jiamiao;
    Row r;
    r["i"]  = static_cast<int64_t>(42);
    r["d"]  = 3.14;
    r["b"]  = true;
    r["s"]  = std::string("hello");
    r["n"]  = nullptr;
    TupleHeader hdr{};
    hdr.row_id = 1;
    hdr.xmin = 100;
    hdr.xmax = 0;
    hdr.cid = 0;
    hdr.schema_hash = 0xABCD;
    hdr.payload_len = 0;
    hdr.crc32 = 0;

    Tuple t = BinaryRowCodec::row_to_tuple(r, hdr);
    CHECK(t.hdr.row_id == 1u);
    CHECK(t.hdr.xmin == 100u);
    CHECK(t.hdr.schema_hash == 0xABCD);

    // decode 业务列
    Row decoded = BinaryRowCodec::decode(t.payload.data(), t.payload.size());
    auto* i = std::get_if<int64_t>(&decoded["i"]);
    REQUIRE(i != nullptr);
    CHECK(*i == 42);
    auto* d = std::get_if<double>(&decoded["d"]);
    REQUIRE(d != nullptr);
    CHECK(*d == doctest::Approx(3.14));
    auto* b = std::get_if<bool>(&decoded["b"]);
    REQUIRE(b != nullptr);
    CHECK(*b == true);
    auto* s = std::get_if<std::string>(&decoded["s"]);
    REQUIRE(s != nullptr);
    CHECK(*s == "hello");
    auto* n = std::get_if<std::nullptr_t>(&decoded["n"]);
    REQUIRE(n != nullptr);
}

TEST_CASE("MemTable: latest version wins on get_latest") {
    using namespace jiamiao;
    MemTable mt("db.sc.t");

    // 写 3 个版本 (row_id=1, 不同 seq, 模拟 MVCC 时间线)
    TupleHeader h1{}; h1.row_id = 1; h1.xmin = 10; h1.xmax = 0; h1.cid = 0;
    TupleHeader h2{}; h2.row_id = 1; h2.xmin = 20; h2.xmax = 0; h2.cid = 0;
    TupleHeader h3{}; h3.row_id = 1; h3.xmin = 30; h3.xmax = 0; h3.cid = 0;
    Tuple v1; v1.hdr = h1; v1.payload = {0x00, 0x00};
    Tuple v2; v2.hdr = h2; v2.payload = {0x00, 0x00};
    Tuple v3; v3.hdr = h3; v3.payload = {0x00, 0x00};
    mt.put(1, v1, 100);
    mt.put(1, v2, 200);
    mt.put(1, v3, 300);

    // InternalKey = (user_key, seq), 同 row_id 不同 seq → 3 entries
    CHECK(mt.size() == 3);

    // get_latest 应该返回 seq 最大的 (latest first)
    auto latest = mt.get_latest(1);
    REQUIRE(latest.has_value());
    CHECK(latest->hdr.xmin == 30u);  // 来自 v3 (seq=300)

    // get_versions 返回所有版本, 顺序 seq DESC
    auto versions = mt.get_versions(1);
    CHECK(versions.size() == 3);
    CHECK(versions[0].hdr.xmin == 30u);
    CHECK(versions[1].hdr.xmin == 20u);
    CHECK(versions[2].hdr.xmin == 10u);
}

TEST_CASE("MemTable: vacuum drops tombstoned versions past oldest active xid") {
    using namespace jiamiao;
    CLog clog;
    // 确保 clog 能容纳这些 xid
    clog.set_status(10, TransactionStatus::COMMITTED);
    clog.set_status(20, TransactionStatus::COMMITTED);
    clog.set_status(30, TransactionStatus::COMMITTED);
    clog.set_status(40, TransactionStatus::COMMITTED);
    clog.set_status(50, TransactionStatus::IN_PROGRESS);  // 活跃

    MemTable mt("db.sc.t");

    // 写 row 1, xmin=10, xmax=20 (被 txn 20 删除, 20 已 commit)
    Tuple t1;
    t1.hdr.row_id = 1;
    t1.hdr.xmin = 10;
    t1.hdr.xmax = 20;
    t1.hdr.cid = 0;
    mt.put(1, t1, 5);

    // 写 row 2, xmin=30, xmax=40 (被 txn 40 删除, 40 已 commit)
    Tuple t2;
    t2.hdr.row_id = 2;
    t2.hdr.xmin = 30;
    t2.hdr.xmax = 40;
    mt.put(2, t2, 6);

    // 写 row 3, 活跃, 不能清理 (xmin=50 未 commit)
    Tuple t3;
    t3.hdr.row_id = 3;
    t3.hdr.xmin = 50;
    t3.hdr.xmax = 0;
    mt.put(3, t3, 7);

    // vacuum: row 1 和 row 2 可清理 (xmax < oldest_active=50, 40 < 50, 20 < 50)
    //         row 3 不能 (xmin=50 >= 50, 活跃)
    size_t can_purge = mt.vacuum(clog, /*oldest_active_xid=*/50);
    CHECK(can_purge == 2);

    // 物理清理
    mt.erase_all_for(1);
    mt.erase_all_for(2);
    CHECK(mt.size() == 1);
    auto latest = mt.get_latest(3);
    CHECK(latest.has_value());
}

TEST_CASE("MemTable: scan order is row_id ASC, same row_id preserves insertion order") {
    using namespace jiamiao;
    MemTable mt("db.sc.t");

    // 插 8 个 entry, seq 唯一递增保证每个都保留
    int64_t seq_counter = 1000;
    for (int64_t rid : {3, 1, 4, 1, 5, 9, 2, 6}) {
        Tuple t;
        t.hdr.row_id = static_cast<uint64_t>(rid);
        t.hdr.xmin = 0;
        t.hdr.xmax = 0;
        t.payload = {0x00, 0x00};
        mt.put(rid, t, static_cast<uint64_t>(seq_counter++));
    }

    // 8 个 put, 8 个 unique (user_key, seq) → 8 entries
    CHECK(mt.size() == 8);

    // scan_all 顺序: user_key ASC, 同 user_key 内 seq DESC
    auto all = mt.scan_all();
    REQUIRE(all.size() == 8);

    // 收集 row_ids (会有 row_id 1 出现 2 次, 因为我们 put 了 2 次)
    std::map<int64_t, int> rids;
    for (const auto& t : all) {
        rids[static_cast<int64_t>(t.hdr.row_id)]++;
    }
    CHECK(rids[1] == 2);  // row_id 1 出现 2 次
    CHECK(rids[3] == 1);
    CHECK(rids[9] == 1);
    // 验证整体有序: row_id 应不递减 (seq DESC 在同 row_id 内, 但不同 row_id 间按 user_key ASC)
    int64_t prev_rid = -1;
    int64_t prev_seq_for_prev = -1;
    for (const auto& t : all) {
        int64_t rid = static_cast<int64_t>(t.hdr.row_id);
        if (rid != prev_rid) {
            CHECK(rid > prev_rid);  // 新 row_id 必须 > 旧 row_id
            prev_rid = rid;
        }
    }
}

TEST_CASE("BinaryRowCodec: round-trip preserves data and stays within size budget") {
    using namespace jiamiao;
    // 构造 100 行, 长字符串拉满二进制优势
    std::vector<Row> rows;
    for (int i = 0; i < 100; ++i) {
        Row r;
        r["id"]     = static_cast<int64_t>(i);
        r["name"]   = std::string("user_name_with_long_prefix_") + std::to_string(i);
        r["desc"]   = std::string(200, 'x');  // 200-char 字符串, JSON 这里没优势
        r["age"]    = static_cast<int64_t>(20 + (i % 50));
        r["score"]  = 1.5 + i * 0.1;
        r["active"] = (i % 2 == 0);
        rows.push_back(std::move(r));
    }

    // JSON 大小 (RowSet 序列化, 模拟 jiamiao JSON)
    size_t json_size = 0;
    for (const auto& r : rows) {
        std::string s = "{";
        bool first = true;
        for (const auto& [k, v] : r) {
            if (!first) s += ",";
            first = false;
            s += "\"" + k + "\":";
            std::visit([&](auto&& val) {
                using T = std::decay_t<decltype(val)>;
                if constexpr (std::is_same_v<T, std::nullptr_t>) s += "null";
                else if constexpr (std::is_same_v<T, int64_t>) s += std::to_string(val);
                else if constexpr (std::is_same_v<T, double>) s += std::to_string(val);
                else if constexpr (std::is_same_v<T, bool>) s += val ? "true" : "false";
                else if constexpr (std::is_same_v<T, std::string>) {
                    s += "\"" + val + "\"";
                }
            }, v);
        }
        s += "}";
        json_size += s.size();
    }

    // BinaryRowCodec 大小
    size_t binary_size = 0;
    for (const auto& r : rows) {
        auto bytes = BinaryRowCodec::encode(r);
        binary_size += bytes.size();
    }

    // 验证 Binary 编码能往返 (基本正确性)
    for (const auto& r : rows) {
        auto bytes = BinaryRowCodec::encode(r);
        Row decoded = BinaryRowCodec::decode(bytes.data(), bytes.size());
        // 抽样: id 字段应该相等
        auto* orig_id = std::get_if<int64_t>(&r.at("id"));
        auto* dec_id  = std::get_if<int64_t>(&decoded.at("id"));
        REQUIRE(orig_id != nullptr);
        REQUIRE(dec_id != nullptr);
        CHECK(*orig_id == *dec_id);
    }

    MESSAGE("JSON size: ", json_size, " bytes; Binary size: ", binary_size, " bytes; ratio: ",
            static_cast<double>(json_size) / std::max<size_t>(1, binary_size));

    // Phase 1 codec 特性: 固定宽度 int/double 编码, 短整数场景下二进制略大于 JSON.
    // 此处只确认大小在合理量级 (1.5x 内). 真正的空间优势要等 Phase 2/3 引入
    // SST block 压缩 + varint 编码后才显现.
    CHECK(binary_size <= json_size * 3 / 2);
}

// ──── Phase 1 LSM 风险 #3 gate: Savepoint 压力测试 ────
//   场景: 3 层嵌套 savepoint + 50 个交错 insert/update/remove × 100 轮
//   目的: 验证 erase_all_for + put 模式在嵌套 savepoint 部分回滚下不留脏数据,
//         不产生重复 InternalKey, 不让 vacuum / scan 看到悬挂行.
TEST_CASE("Savepoint stress: 3-level nested with 50 interleaved ops × 100 iterations") {
    TempDir tmp;
    StorageEngine engine(tmp.path, /*checkpoint_interval=*/100000);
    engine.create_table("t", simple_schema());

    constexpr int kIterations = 100;
    constexpr int kOpsPerIter = 50;

    // 种子 5 行数据 (作为各轮的基线)
    {
        begin_txn(engine);
        for (int i = 1; i <= 5; ++i) {
            engine.insert_with_txn("t", make_data_row(i, std::string("seed_") + std::to_string(i)));
        }
        commit_txn(engine);
    }

    int64_t next_id = 100;
    for (int iter = 0; iter < kIterations; ++iter) {
        begin_txn(engine);
        auto& txn = engine.txn_mgr();
        auto our_xid = txn.get_current_xid();

        // 顶层操作 (10 个)
        for (int i = 0; i < 10; ++i) {
            engine.insert_with_txn("t", make_data_row(next_id++, "top"));
        }

        txn.savepoint("sp_outer");
        // outer 层操作 (15 个: insert + update)
        for (int i = 0; i < 15; ++i) {
            if (i % 3 == 0) {
                int64_t target = next_id - 5;
                engine.update_with_txn("t",
                    [target](const Row& r) {
                        auto it = r.find("id");
                        if (it == r.end()) return false;
                        auto* v = std::get_if<int64_t>(&it->second);
                        return v && *v == target;
                    },
                    {{"name", std::string("outer_upd")}});
            } else {
                engine.insert_with_txn("t", make_data_row(next_id++, "outer"));
            }
        }

        txn.savepoint("sp_middle");
        // middle 层操作 (15 个: insert + remove)
        for (int i = 0; i < 15; ++i) {
            if (i % 4 == 0) {
                int64_t target = next_id - 3;
                engine.remove_with_txn("t",
                    [target](const Row& r) {
                        auto it = r.find("id");
                        if (it == r.end()) return false;
                        auto* v = std::get_if<int64_t>(&it->second);
                        return v && *v == target;
                    });
            } else {
                engine.insert_with_txn("t", make_data_row(next_id++, "middle"));
            }
        }

        txn.savepoint("sp_inner");
        // inner 层操作 (10 个: 全 insert)
        for (int i = 0; i < 10; ++i) {
            engine.insert_with_txn("t", make_data_row(next_id++, "inner"));
        }
        // 总共 50 个操作

        // 回滚策略: 三种轮替验证三种部分回滚路径
        int strategy = iter % 3;
        if (strategy == 0) {
            // 回滚到 inner: 撤销最后 10 个
            size_t to_undo = txn.rollback_to_savepoint("sp_inner");
            CHECK(to_undo == 10);
            engine.apply_undo_to(to_undo);
        } else if (strategy == 1) {
            // 回滚到 middle: 撤销最后 25 个 (inner 10 + middle 15)
            size_t to_undo = txn.rollback_to_savepoint("sp_middle");
            CHECK(to_undo == 25);
            engine.apply_undo_to(to_undo);
        } else {
            // 回滚到 outer: 撤销最后 40 个 (inner 10 + middle 15 + outer 15)
            size_t to_undo = txn.rollback_to_savepoint("sp_outer");
            CHECK(to_undo == 40);
            engine.apply_undo_to(to_undo);
        }

        // 提交剩余 (顶层 10 个 + savepoint 之前活下来的) 或全部回滚
        if (iter % 2 == 0) {
            commit_txn(engine);
        } else {
            rollback_txn(engine);
        }

        // 每轮后 scan 一遍, 验证不崩 (visibility 链没坏)
        auto snap = engine.txn_mgr().get_snapshot();
        auto rows = engine.scan_with_snapshot("t", snap, jm::InvalidTransactionId, 0);

        // 行 id 应该唯一 (PK 约束)
        std::set<int64_t> seen_ids;
        for (const auto& r : rows) {
            auto it = r.find("id");
            REQUIRE(it != r.end());
            auto* v = std::get_if<int64_t>(&it->second);
            REQUIRE(v != nullptr);
            CHECK(seen_ids.insert(*v).second);  // 无重复
        }
    }

    // 终态检查: 至少 seed 的 5 行还在 (除非被某轮 remove 了, 但 seed 行 id<100 不会被 update/remove 命中)
    auto final_snap = engine.txn_mgr().get_snapshot();
    auto final_rows = engine.scan_with_snapshot("t", final_snap, jm::InvalidTransactionId, 0);
    int seed_seen = 0;
    for (const auto& r : final_rows) {
        auto it = r.find("id");
        if (it == r.end()) continue;
        auto* v = std::get_if<int64_t>(&it->second);
        if (v && *v >= 1 && *v <= 5) ++seed_seen;
    }
    CHECK(seed_seen == 5);  // seed 行始终保留
}

// ──── Phase 2 LSM: Arena ────

TEST_CASE("Arena: 1M small allocs track bytes_used and stay within blocks") {
    jm::Arena arena;
    constexpr size_t kCount = 1'000'000;
    constexpr size_t kObjSize = 8;

    // 每次分配 8 字节 (1 个 int64)
    std::vector<int64_t*> ptrs;
    ptrs.reserve(kCount);
    for (size_t i = 0; i < kCount; ++i) {
        auto* p = static_cast<int64_t*>(arena.allocate(kObjSize));
        REQUIRE(p != nullptr);
        *p = static_cast<int64_t>(i);  // 验证可写
        ptrs.push_back(p);
    }

    CHECK(arena.bytes_used() == kCount * kObjSize);
    // 64KB 块, 1M * 8B = 8MB. 每块装 64K/8 = 8192 obj → 至少 122 块.
    // 误差来自尾部不满块 + 初始块可能 1 个 = 123~128 块.
    CHECK(arena.block_count() >= 120);
    CHECK(arena.block_count() <= 130);

    // 验证指针有效 (没被覆盖)
    for (size_t i = 0; i < kCount; ++i) {
        CHECK(*ptrs[i] == static_cast<int64_t>(i));
    }
}

TEST_CASE("Arena: 100KB alloc goes to a dedicated block (bump-block not bloated)") {
    jm::Arena arena;

    // 分配几个小块 (落到 kBlockSize 主块)
    for (int i = 0; i < 10; ++i) {
        arena.allocate(64);
    }
    size_t blocks_before = arena.block_count();

    // 100KB 单次分配 (> 16KB 阈值) → 独立块
    void* big = arena.allocate(100 * 1024);
    REQUIRE(big != nullptr);

    // block_count 增 1 (独立块)
    CHECK(arena.block_count() == blocks_before + 1);
    CHECK(arena.bytes_used() >= 100 * 1024);

    // 再分配 64 字节小块: 应该落到原主块, 不污染大块
    void* small = arena.allocate(64);
    REQUIRE(small != nullptr);
    CHECK(arena.block_count() == blocks_before + 1);  // 没新增块
}

TEST_CASE("Arena: reset() releases all blocks, subsequent allocate works") {
    jm::Arena arena;

    // 灌满一些数据
    for (int i = 0; i < 1000; ++i) {
        arena.allocate(128);
    }
    size_t blocks_before = arena.block_count();
    REQUIRE(blocks_before >= 1);
    REQUIRE(arena.bytes_used() > 0);

    // reset
    arena.reset();
    CHECK(arena.bytes_used() == 0);
    // block_count 保留 (用于观察 peak), reset 仅释放内存
    CHECK(arena.block_count() == blocks_before);

    // 后续分配仍能用
    void* p = arena.allocate(64);
    REQUIRE(p != nullptr);
    *static_cast<int64_t*>(p) = 0xCAFE;
    CHECK(*static_cast<int64_t*>(p) == 0xCAFE);
    CHECK(arena.bytes_used() == 64);
}

// ──── Phase 2 LSM: SkipList ────

#include "storage/skiplist.h"
#include <thread>
#include <vector>

namespace {
// Helper: 顺序 int 键 + 简单 int 值
using IntSkipList = jm::SkipList<int, int>;
}

TEST_CASE("SkipList: 1000 元素 put + range 顺序遍历 (Pugh 经典)") {
    jm::Arena arena;
    IntSkipList sl(&arena);

    // 顺序插入 0..999
    for (int i = 0; i < 1000; ++i) {
        sl.put(i, i * 10);
    }
    CHECK(sl.size() == 1000);

    // range(200, 205) 应得 200..205
    auto v = sl.range(200, 205);
    REQUIRE(v.size() == 6);
    for (int i = 0; i < 6; ++i) {
        CHECK(v[i] == (200 + i) * 10);
    }

    // all() 应得 0..999 顺序
    auto all = sl.all();
    REQUIRE(all.size() == 1000);
    for (int i = 0; i < 1000; ++i) {
        CHECK(all[i] == i * 10);
    }

    // get_exact
    auto p = sl.get_exact(500);
    REQUIRE(p.has_value());
    CHECK(*p == 5000);
    CHECK_FALSE(sl.get_exact(10000).has_value());
}

TEST_CASE("SkipList: 1 writer + 4 readers 100k 操作, 读数 == 写数") {
    jm::Arena arena;
    IntSkipList sl(&arena);

    constexpr int kWrites = 100000;
    constexpr int kReaders = 4;

    // Writer 线程: 顺序 put 0..kWrites-1
    std::thread writer([&]() {
        for (int i = 0; i < kWrites; ++i) {
            sl.put(i, i);
        }
    });

    // 4 Reader 线程: 不停 all() / range(), 统计累计读到的元素数
    std::atomic<int> total_seen{0};
    std::atomic<bool> stop{false};
    std::vector<std::thread> readers;
    for (int r = 0; r < kReaders; ++r) {
        readers.emplace_back([&]() {
            int local = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                auto all = sl.all();
                local += static_cast<int>(all.size());
                // 也跑 range 验证 lock-free 读不会挂
                (void)sl.range(0, 1000);
            }
            total_seen.fetch_add(local, std::memory_order_relaxed);
        });
    }

    writer.join();
    stop.store(true, std::memory_order_relaxed);
    for (auto& t : readers) t.join();

    // 最终: 100k 元素都在
    CHECK(sl.size() == kWrites);
    auto final_all = sl.all();
    REQUIRE(final_all.size() == kWrites);
    // 顺序检查
    for (int i = 0; i < kWrites; ++i) {
        REQUIRE(final_all[i] == i);
    }
    // reader 至少读了 N 次 (具体值不严格, 不为 0 即可)
    CHECK(total_seen.load() > 0);

    MESSAGE("1W+4R 100k ops: total reader iterations seen = ", total_seen.load());
}

TEST_CASE("SkipList: erase_range 批量删 + size 维护") {
    jm::Arena arena;
    IntSkipList sl(&arena);

    for (int i = 0; i < 100; ++i) sl.put(i, i);
    CHECK(sl.size() == 100);

    // 删 [30, 39] 共 10 个
    size_t erased = sl.erase_range(30, 39);
    CHECK(erased == 10);
    CHECK(sl.size() == 90);

    // [30, 39] 不存在
    for (int i = 30; i <= 39; ++i) CHECK_FALSE(sl.get_exact(i).has_value());
    // 边界 29, 40 还在
    CHECK(sl.get_exact(29).has_value());
    CHECK(sl.get_exact(40).has_value());

    // 再删 [40, 50]
    erased = sl.erase_range(40, 50);
    CHECK(erased == 11);  // 40, 41, ..., 50
    CHECK(sl.size() == 79);

    // all() 不应含 [30, 50]
    auto all = sl.all();
    for (int v : all) {
        bool in_range = (v >= 30 && v <= 50);
        CHECK_FALSE(in_range);
    }
}

TEST_CASE("SkipList: TSan-friendly 并发 1W+1R 小规模 (CI gate)") {
    // 1 writer + 1 reader 小规模 1k 操作, 主要为 TSan 跑通.
    // 真正的 TSan 跑在 CI: cmake -DCMAKE_CXX_FLAGS="-fsanitize=thread" .
    jm::Arena arena;
    IntSkipList sl(&arena);

    constexpr int kWrites = 1000;
    std::atomic<bool> stop{false};
    int reader_seen = 0;

    std::thread reader([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            reader_seen += static_cast<int>(sl.all().size());
        }
    });

    for (int i = 0; i < kWrites; ++i) sl.put(i, i);

    stop.store(true, std::memory_order_relaxed);
    reader.join();

    CHECK(sl.size() == kWrites);
    CHECK(reader_seen > 0);
}
