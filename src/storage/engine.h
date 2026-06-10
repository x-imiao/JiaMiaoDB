#ifndef JIAMIAODB_STORAGE_ENGINE_H
#define JIAMIAODB_STORAGE_ENGINE_H

#include <atomic>
#include <functional>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <mutex>
#include "../types.h"
#include "wal.h"
#include "checkpoint.h"
#include "transaction.h"
#include "catalog.h"
#include "lock_manager.h"

/* ═══════════════════════════════════════════════════════
   StorageEngine — 存储引擎

   写入路径: WAL (append) → 内存状态 → (可选) Checkpoint
   读取路径: Checkpoint → WAL Replay → 内存状态
   崩溃恢复: 自动，重放 WAL

   并发模型 (Phase 2):
   - lifecycle_mutex_: 保护生命周期 (load/save/close) 和 replay.
     操作极短, 启动期/恢复期单线程即可.
   - mgr_ (LockManager): 表级锁, 每个 public 方法调用使用唯一 call_xid_:
       * DML (insert/update/remove/scan):  按表 Exclusive / Shared
       * DDL (create_table/drop_table/...): Exclusive 锁 __catalog__
         锁串行化所有 schema 变更
     不同表上的并发 DML 互不阻塞; 同表读写串行.
   - call_xid_: 单调递增, 永不重用, 避免与事务 XID 空间冲突
     (XID 从 FirstNormalTransactionId 起, call_xid 从 FirstCallXid 起).
   ═══════════════════════════════════════════════════════ */

struct IndexInfo {
    std::string column;
    std::map<std::string, std::vector<int64_t>> entries; // value → row indices
};

class StorageEngine {
public:
    StorageEngine(const std::string& data_dir, int checkpoint_interval = 5000);
    ~StorageEngine();

    // 生命周期
    void load();
    void save();
    void close();

    // Catalog
    Catalog* catalog() { return catalog_.get(); }
    void create_database(const std::string& name);
    void drop_database(const std::string& name);
    void create_schema(const std::string& db_name, const std::string& schema_name);
    void create_user(const std::string& name, const std::string& password);
    void drop_user(const std::string& name);
    std::string current_db() const;
    void set_current_db(const std::string& name);
    std::vector<std::string> list_databases();
    std::vector<std::string> list_users();
    std::vector<std::string> list_schemas(const std::string& db_name);

    // Schema
    void create_table(const std::string& name, const std::vector<ColumnDef>& columns);
    void drop_table(const std::string& name);
    TableSchema* get_schema(const std::string& name);
    std::vector<std::string> list_tables();
    std::string resolve_table_name(const std::string& name) const;

    // 数据
    Row insert(const std::string& table, const Row& row);
    RowSet scan(const std::string& table);
    int64_t update(const std::string& table, std::function<bool(const Row&)> match, const std::map<std::string, Value>& updates);
    int64_t remove(const std::string& table, std::function<bool(const Row&)> match);

    // 事务感知的数据操作
    jiamiao::TransactionManager& txn_mgr();
    Row insert_with_txn(const std::string& table, const Row& row);
    int64_t update_with_txn(const std::string& table, std::function<bool(const Row&)> match, const std::map<std::string, Value>& updates);
    int64_t remove_with_txn(const std::string& table, std::function<bool(const Row&)> match);

    // 索引
    RowSet index_lookup(const std::string& table, const std::string& column, const Value& value);
    bool has_index(const std::string& table, const std::string& column);
    void create_index(const std::string& table, const std::string& column);

    // MVCC 可见性感知的扫描
    RowSet scan_with_snapshot(const std::string& table, const jiamiao::Snapshot& snap,
                              jiamiao::TransactionId xid, int32_t cid);
    RowSet index_lookup_with_snapshot(const std::string& table, const std::string& column,
                                      const Value& value, const jiamiao::Snapshot& snap,
                                      jiamiao::TransactionId xid, int32_t cid);

    // 事务 WAL 与 Undo
    void apply_undo();
    void write_xact_commit(jiamiao::TransactionId xid);
    void write_xact_abort(jiamiao::TransactionId xid);

private:
    // 锁目标: 所有 catalog / DDL 操作的虚拟目标
    static constexpr const char* kCatalogTarget = "__catalog__";
    // 锁目标: WAL 串行化
    static constexpr const char* kWalTarget     = "__wal__";

    // 内部状态
    std::map<std::string, TableSchema> tables_;
    std::map<std::string, RowSet> data_;
    std::map<std::string, int64_t> row_ids_;
    std::map<std::string, std::vector<IndexInfo>> indexes_;

    std::unique_ptr<WriteAheadLog> wal_;
    std::unique_ptr<CheckpointManager> ckp_mgr_;
    std::unique_ptr<jiamiao::TransactionManager> txn_mgr_;
    std::unique_ptr<Catalog> catalog_;

    std::string data_dir_;
    int checkpoint_interval_;

    // ── 并发原语 (Phase 2) ──
    // lifecycle: load/save/close/replay, 启动期单线程
    std::mutex            lifecycle_mutex_;
    // 表级锁 (取代原 global_mutex_)
    jiamiao::LockManager  mgr_;
    // 每个 public 调用分配的"会话 XID", 永不重用
    std::atomic<uint32_t> call_xid_seq_{jiamiao::FirstNormalTransactionId + 0x10000000u};

    // 内部: 分配 call xid
    jiamiao::TransactionId next_call_xid() {
        return call_xid_seq_.fetch_add(1, std::memory_order_relaxed);
    }

    // 内部方法
    void replay_record(const WALRecord& rec);
    void checkpoint();
    void maybe_checkpoint();
    bool updates_affect_index(const std::string& table, const std::map<std::string, Value>& updates);
    void rebuild_indexes(const std::string& table);
    void update_indexes(const std::string& table, int64_t row_index, const Row& row);

    Row validate(const TableSchema& schema, const Row& row);
    Value coerce(DataType type, const Value& v);

    // WAL 写入
    int64_t next_seq();
    void write_wal(const std::string& op, const std::string& table, const json& data,
                   jiamiao::TransactionId xid = jiamiao::InvalidTransactionId);
    int64_t wal_seq_ = 0;
};

#endif // JIAMIAODB_STORAGE_ENGINE_H
