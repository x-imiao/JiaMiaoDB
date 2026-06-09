#ifndef JIAMIAODB_STORAGE_ENGINE_H
#define JIAMIAODB_STORAGE_ENGINE_H

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

/* ═══════════════════════════════════════════════════════
   StorageEngine — 存储引擎

   写入路径: WAL (append) → 内存状态 → (可选) Checkpoint
   读取路径: Checkpoint → WAL Replay → 内存状态
   崩溃恢复: 自动，重放 WAL
   线程安全: 表级锁
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

    // Schema
    void create_table(const std::string& name, const std::vector<ColumnDef>& columns);
    void drop_table(const std::string& name);
    TableSchema* get_schema(const std::string& name);
    std::vector<std::string> list_tables();

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
    // 内部状态
    std::map<std::string, TableSchema> tables_;
    std::map<std::string, RowSet> data_;
    std::map<std::string, int64_t> row_ids_;
    std::map<std::string, std::vector<IndexInfo>> indexes_;

    std::unique_ptr<WriteAheadLog> wal_;
    std::unique_ptr<CheckpointManager> ckp_mgr_;
    std::unique_ptr<jiamiao::TransactionManager> txn_mgr_;

    std::string data_dir_;
    int checkpoint_interval_;
    std::mutex global_mutex_;

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
