#include "engine.h"
#include "transaction.h"
#include "lock_manager.h"
#include "tuple.h"
#include "memtable.h"
#include "wal_payload.h"
#include "catalog_codec.h"
#include "clog_codec.h"
#include <filesystem>
#include <iostream>
#include <algorithm>
#include <sstream>
#include "common/json.h"

using json = Json;
using jiamiao::LockManager;
using jiamiao::LockMode;
using jiamiao::LockTarget;
using jiamiao::LockTargetType;
using jiamiao::MemTable;
using jiamiao::BinaryRowCodec;
using jiamiao::Tuple;
using jiamiao::TupleHeader;
using jiamiao::TransactionId;
using jiamiao::TransactionStatus;
using jiamiao::WalOp;
using jiamiao::WALRecordV3;

// ── 投影: Tuple → Row (含系统列 _rowid/_xmin/_xmax/_cid) ──
//   用途: 给 check_tuple_visibility / executor / tests 用.
//   Phase 1 边界: 每次物化做一次 decode + 5 个字段, 接受此开销.
static Row tuple_to_row_for_visibility(const Tuple& t) {
    Row r = BinaryRowCodec::tuple_to_row(t);
    r["_rowid"] = static_cast<int64_t>(t.hdr.row_id);
    r["_xmin"]  = static_cast<int64_t>(t.hdr.xmin);
    r["_xmax"]  = static_cast<int64_t>(t.hdr.xmax);
    r["_cid"]   = static_cast<int64_t>(t.hdr.cid);
    return r;
}

// 把 MemTable 全部内容 (按 scan_all 顺序 = row_id ASC, seq DESC) 物化为 RowSet.
// MVCC 由调用方负责 (scan vs scan_with_snapshot).
static RowSet materialize_all(const MemTable& mt) {
    RowSet out;
    auto all = mt.scan_all();
    out.reserve(all.size());
    for (const auto& t : all) {
        out.push_back(tuple_to_row_for_visibility(t));
    }
    return out;
}

// 从 Row (含系统列) 构造 TupleHeader. 默认 xmin=InvalidTransactionId, xmax=0, cid=0
// 适用于非事务写入 (insert / checkpoint 恢复).
static TupleHeader header_from_row(const std::string& qualified, int64_t row_id, const Row& row) {
    TupleHeader h{};
    h.row_id      = static_cast<uint64_t>(row_id);
    h.xmin        = 0;  // 由调用方在 put 前覆盖
    h.xmax        = 0;
    h.cid         = 0;
    h.schema_hash = Tuple::fnv1a_16(qualified);
    h.flags       = 0;
    h.payload_len = 0;
    h.crc32       = 0;
    auto it = row.find("_xmin");
    if (it != row.end()) {
        if (auto* v = std::get_if<int64_t>(&it->second)) h.xmin = static_cast<uint32_t>(*v);
    }
    auto xm = row.find("_xmax");
    if (xm != row.end()) {
        if (auto* v = std::get_if<int64_t>(&xm->second)) h.xmax = static_cast<uint32_t>(*v);
    }
    auto ci = row.find("_cid");
    if (ci != row.end()) {
        if (auto* v = std::get_if<int64_t>(&ci->second)) h.cid = static_cast<uint32_t>(*v);
    }
    return h;
}

StorageEngine::StorageEngine(const std::string& data_dir, int checkpoint_interval)
    : data_dir_(data_dir), checkpoint_interval_(checkpoint_interval), mgr_(30000) {
    if (!std::filesystem::exists(data_dir)) {
        std::filesystem::create_directories(data_dir);
    }

    // Phase 3: 建 MemoryContext 树 (在 jmalloc 可用之前不要 jmalloc)
    //   假设 main() 已调 MemoryContextInit() 创建 TopMemoryContext
    jiamiao::MemoryContext parent = jiamiao::TopMemoryContext;
    if (parent == nullptr) {
        // 防御: 调用方未 init (例如测试), 同步建一个
        jiamiao::MemoryContextInit();
        parent = jiamiao::TopMemoryContext;
    }
    engine_ctx_ = jiamiao::JMAllocSetContextCreate(parent, "EngineContext",
                                                   0, 8 * 1024, 8 * 1024 * 1024);
    wal_ctx_    = jiamiao::JMAllocSetContextCreate(engine_ctx_, "WALContext",
                                                   0, 8 * 1024, 1 * 1024 * 1024);
    vacuum_ctx_ = jiamiao::JMAllocSetContextCreate(engine_ctx_, "VacuumContext",
                                                   0, 8 * 1024, 1 * 1024 * 1024);

    wal_ = std::make_unique<jiamiao::WriteAheadLogV3>(data_dir + "/wal.log");
    ckp_mgr_ = std::make_unique<CheckpointManager>(data_dir);
    txn_mgr_ = std::make_unique<jiamiao::TransactionManager>();
    catalog_ = std::make_unique<Catalog>();
}

StorageEngine::~StorageEngine() {
    close();
    // delete engine_ctx_ 级联释放 wal_ctx_ / vacuum_ctx_
    if (engine_ctx_ != nullptr) {
        jiamiao::MemoryContextDelete(engine_ctx_);
        engine_ctx_ = nullptr;
        wal_ctx_    = nullptr;
        vacuum_ctx_ = nullptr;
    }
}

void StorageEngine::load() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);

    // 0. 加载 Catalog
    auto ckp = ckp_mgr_->load();
    wal_seq_ = ckp.last_seq;

    // O-3: 优先用 binary catalog (v3 checkpoint), 兜底走 JSON (v2 / legacy)
    bool is_old_checkpoint = true;
    if (!ckp.catalog_bytes.empty()) {
        if (!jiamiao::catalog_decode(ckp.catalog_bytes, catalog_.get())) {
            std::cerr << "[Engine] Catalog binary decode failed; falling back to JSON\n";
        } else {
            is_old_checkpoint = false;  // binary catalog 加载成功
        }
    }
    if (is_old_checkpoint && !ckp.catalog_data.is_null() && ckp.catalog_data.contains("databases")) {
        catalog_->from_json(ckp.catalog_data);
    }
    // else: catalog_ already has defaultdb from constructor

    // 1. 加载 Checkpoint 中的表 (旧格式迁移: 非限定表名 → defaultdb.public.*)
    for (const auto& t : ckp.tables) {
        // 如果表名不含 '.' 且是新启动，迁移到 defaultdb.public
        std::string qualified_name = t.name;
        if (qualified_name.find('.') == std::string::npos && is_old_checkpoint) {
            qualified_name = "defaultdb.public." + t.name;
        }
        TableSchema schema = t;
        schema.name = qualified_name;
        tables_[qualified_name] = schema;
        data_[qualified_name] = std::make_unique<MemTable>(qualified_name);
        row_ids_[qualified_name] = 0;
        indexes_[qualified_name] = {};

        // 恢复数据
        if (ckp.table_rows.find(t.name) != ckp.table_rows.end()) {
            for (const auto& [rid, row] : ckp.table_rows[t.name]) {
                // Phase 1: checkpoint 不带 xmin/xmax/cid, 用 InvalidTransactionId 占位.
                // load 阶段不需要 MVCC, 系统列由 tuple_to_row_for_visibility 补齐.
                TupleHeader hdr = header_from_row(qualified_name, rid, row);
                auto tup = BinaryRowCodec::row_to_tuple(row, hdr);
                data_[qualified_name]->put(rid, tup, ++wal_seq_);
            }
        }

        // 恢复 row_id_counter
        if (ckp.row_id_counters.find(t.name) != ckp.row_id_counters.end()) {
            row_ids_[qualified_name] = ckp.row_id_counters[t.name];
        }

        // 恢复索引
        if (ckp.indexes_data.find(t.name) != ckp.indexes_data.end()) {
            indexes_[qualified_name] = ckp.indexes_data[t.name];
        }
    }

    // 2. 恢复事务状态
    if (ckp.next_xid >= jiamiao::FirstNormalTransactionId) {
        txn_mgr_->set_next_xid(ckp.next_xid);
    }
    if (!ckp.clog_bytes.empty()) {
        // O-4: 优先 binary clog
        if (!jiamiao::clog_decode(ckp.clog_bytes, &txn_mgr_->clog())) {
            std::cerr << "[Engine] CLog binary decode failed; falling back to JSON\n";
        }
    } else if (!ckp.clog_entries.is_null()) {
        // 旧 v3 文件 fallback: JSON
        txn_mgr_->clog().from_json(ckp.clog_entries);
    }
    txn_mgr_->rebuild_active_from_clog();
    txn_mgr_->reset_context();

    // 3. 重放 WAL (Phase 3a: v3 binary payload, replay 直接吃 v3 记录)
    wal_->open();
    auto records = wal_->replay(ckp.last_seq);
    for (const auto& v3rec : records) {
        replay_record(v3rec);
    }

    std::cout << "[存储] 已加载 " << tables_.size() << " 个表, "
              << ckp.last_seq << " seq, "
              << records.size() << " 条 WAL 重放\n";
}

void StorageEngine::save() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    checkpoint();
}

void StorageEngine::close() {
    {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        checkpoint();
    }
    wal_->close();
}

/* ─── Catalog ─── */

void StorageEngine::create_database(const std::string& name) {
    auto h = mgr_.acquire(next_call_xid(),
                          {LockTargetType::Table, kCatalogTarget}, LockMode::Exclusive);
    catalog_->create_database(name);
    // O-1: payload 为空, name 在 WALRecordV3.table
    write_wal(WalOp::kCreateDatabase, name, jiamiao::wal_encode_empty());
    maybe_checkpoint();
}

void StorageEngine::drop_database(const std::string& name) {
    auto h = mgr_.acquire(next_call_xid(),
                          {LockTargetType::Table, kCatalogTarget}, LockMode::Exclusive);
    catalog_->drop_database(name);
    // 删除该数据库下的所有表
    std::string prefix = name + ".";
    std::vector<std::string> to_drop;
    for (const auto& [tname, _] : tables_) {
        if (tname.compare(0, prefix.size(), prefix) == 0) {
            to_drop.push_back(tname);
        }
    }
    for (const auto& t : to_drop) {
        tables_.erase(t);
        data_.erase(t);
        row_ids_.erase(t);
        indexes_.erase(t);
    }
    // O-1: payload 为空, name 在 WALRecordV3.table
    write_wal(WalOp::kDropDatabase, name, jiamiao::wal_encode_empty());
    maybe_checkpoint();
}

void StorageEngine::create_schema(const std::string& db_name, const std::string& schema_name) {
    auto h = mgr_.acquire(next_call_xid(),
                          {LockTargetType::Table, kCatalogTarget}, LockMode::Exclusive);
    catalog_->create_schema(db_name, schema_name);
    // O-1: 二进制 payload (schema name); database name 在 WALRecordV3.table
    jiamiao::CreateSchemaPayload p;
    p.schema = schema_name;
    write_wal(WalOp::kCreateSchema, db_name, jiamiao::wal_encode_create_schema(p));
    maybe_checkpoint();
}

void StorageEngine::create_user(const std::string& name, const std::string& password) {
    auto h = mgr_.acquire(next_call_xid(),
                          {LockTargetType::Table, kCatalogTarget}, LockMode::Exclusive);
    catalog_->create_user(name, password);
    // O-1: payload 为空; name 在 WALRecordV3.table. 不记录密码到 WAL.
    write_wal(WalOp::kCreateUser, name, jiamiao::wal_encode_empty());
    maybe_checkpoint();
}

void StorageEngine::drop_user(const std::string& name) {
    auto h = mgr_.acquire(next_call_xid(),
                          {LockTargetType::Table, kCatalogTarget}, LockMode::Exclusive);
    catalog_->drop_user(name);
    // O-1: payload 为空; name 在 WALRecordV3.table
    write_wal(WalOp::kDropUser, name, jiamiao::wal_encode_empty());
    maybe_checkpoint();
}

std::string StorageEngine::current_db() const {
    return catalog_->current_database();
}

void StorageEngine::set_current_db(const std::string& name) {
    catalog_->set_current_database(name);
}

std::vector<std::string> StorageEngine::list_databases() {
    auto h = mgr_.acquire(next_call_xid(),
                          {LockTargetType::Table, kCatalogTarget}, LockMode::Shared);
    return catalog_->list_databases();
}

std::vector<std::string> StorageEngine::list_users() {
    auto h = mgr_.acquire(next_call_xid(),
                          {LockTargetType::Table, kCatalogTarget}, LockMode::Shared);
    return catalog_->list_users();
}

std::vector<std::string> StorageEngine::list_schemas(const std::string& db_name) {
    auto h = mgr_.acquire(next_call_xid(),
                          {LockTargetType::Table, kCatalogTarget}, LockMode::Shared);
    return catalog_->list_schemas(db_name);
}

std::string StorageEngine::resolve_table_name(const std::string& name) const {
    return catalog_->qualify_table_name(name);
}

/* ─── Schema ─── */

void StorageEngine::create_table(const std::string& name, const std::vector<ColumnDef>& columns) {
    auto h = mgr_.acquire(next_call_xid(),
                          {LockTargetType::Table, kCatalogTarget}, LockMode::Exclusive);
    std::string qualified = resolve_table_name(name);
    if (tables_.count(qualified)) {
        throw std::runtime_error("表 \"" + name + "\" 已存在");
    }

    // O-1: 二进制 payload (columns); table name 在 WALRecordV3.table
    jiamiao::CreateTablePayload ctp;
    ctp.columns = columns;
    write_wal(WalOp::kCreateTable, qualified, jiamiao::wal_encode_create_table(ctp));

    TableSchema schema;
    schema.name = qualified;
    schema.columns = columns;
    schema.row_count = 0;
    tables_[qualified] = std::move(schema);
    data_[qualified] = std::make_unique<MemTable>(qualified);
    row_ids_[qualified] = 0;
    indexes_[qualified] = {};

    // PK 自动建索引 (catalog 锁已持有, 内部 create_index 不会重入)
    for (const auto& c : columns) {
        if (c.primary_key) {
            create_index(qualified, c.name);
        }
    }

    maybe_checkpoint();
}

void StorageEngine::drop_table(const std::string& name) {
    auto h = mgr_.acquire(next_call_xid(),
                          {LockTargetType::Table, kCatalogTarget}, LockMode::Exclusive);
    std::string qualified = resolve_table_name(name);
    if (!tables_.count(qualified)) {
        throw std::runtime_error("表 \"" + name + "\" 不存在");
    }

    // O-1: payload 为空; qualified 在 WALRecordV3.table
    write_wal(WalOp::kDropTable, qualified, jiamiao::wal_encode_empty());

    tables_.erase(qualified);
    data_.erase(qualified);
    row_ids_.erase(qualified);
    indexes_.erase(qualified);
    maybe_checkpoint();
}

TableSchema* StorageEngine::get_schema(const std::string& name) {
    auto h = mgr_.acquire(next_call_xid(),
                          {LockTargetType::Table, kCatalogTarget}, LockMode::Shared);
    std::string qualified = resolve_table_name(name);
    auto it = tables_.find(qualified);
    if (it != tables_.end()) return &it->second;
    // 也尝试在非限定名下查找
    it = tables_.find(name);
    if (it != tables_.end()) return &it->second;
    return nullptr;
}

std::vector<std::string> StorageEngine::list_tables() {
    auto h = mgr_.acquire(next_call_xid(),
                          {LockTargetType::Table, kCatalogTarget}, LockMode::Shared);
    std::string prefix = catalog_->current_database() + ".";
    std::vector<std::string> names;
    for (const auto& [name, _] : tables_) {
        if (name.compare(0, prefix.size(), prefix) == 0) {
            names.push_back(name);
        }
    }
    return names;
}

/* ─── 数据 ─── */

Row StorageEngine::insert(const std::string& table, const Row& row) {
    std::string qualified = resolve_table_name(table);
    auto h = mgr_.acquire(next_call_xid(),
                          {LockTargetType::Table, qualified}, LockMode::Exclusive);
    auto schema_it = tables_.find(qualified);
    if (schema_it == tables_.end()) {
        throw std::runtime_error("表 \"" + table + "\" 不存在");
    }

    auto validated = validate(schema_it->second, row);
    int64_t id = (row_ids_[qualified]) + 1;
    row_ids_[qualified] = id;
    validated["_rowid"] = id;
    validated["_xmax"] = static_cast<int64_t>(0);
    validated["_cid"]  = static_cast<int64_t>(0);

    // O-1: 二进制 payload (rowid + row), no json
    jiamiao::InsertPayload ip;
    ip.rowid = id;
    ip.row = validated;
    write_wal(WalOp::kInsert, qualified, jiamiao::wal_encode_insert(ip));

    // Phase 1 LSM: insert → MemTable::put
    TupleHeader hdr = header_from_row(qualified, id, validated);
    auto tup = BinaryRowCodec::row_to_tuple(validated, hdr);
    data_[qualified]->put(id, tup, ++wal_seq_);
    update_indexes(qualified, id, validated);
    schema_it->second.row_count = data_[qualified]->size();
    maybe_checkpoint();

    return validated;
}

RowSet StorageEngine::scan(const std::string& table) {
    std::string qualified = resolve_table_name(table);
    auto h = mgr_.acquire(next_call_xid(),
                          {LockTargetType::Table, qualified}, LockMode::Shared);
    auto it = data_.find(qualified);
    if (it == data_.end()) {
        throw std::runtime_error("表 \"" + table + "\" 不存在");
    }
    // 无 MVCC: 投影所有版本, 但实际无并发写时每 row_id 仅一个版本
    return materialize_all(*it->second);
}

RowSet StorageEngine::scan_with_snapshot(const std::string& table,
                                          const jiamiao::Snapshot& snap,
                                          jiamiao::TransactionId xid,
                                          int32_t cid) {
    std::string qualified = resolve_table_name(table);
    auto h = mgr_.acquire(next_call_xid(),
                          {LockTargetType::Table, qualified}, LockMode::Shared);
    auto it = data_.find(qualified);
    if (it == data_.end()) {
        throw std::runtime_error("表 \"" + table + "\" 不存在");
    }
    RowSet visible;
    auto all = it->second->scan_all();
    for (const auto& tup : all) {
        Row row = tuple_to_row_for_visibility(tup);
        if (jiamiao::check_tuple_visibility(row, xid, snap, cid, txn_mgr_->clog())) {
            visible.push_back(std::move(row));
            // SSI: 记录 SIREAD (仅 SERIALIZABLE 下有效)
            auto rid_it = row.find("_rowid");
            if (rid_it != row.end()) {
                if (auto* rv = std::get_if<int64_t>(&rid_it->second)) {
                    txn_mgr_->register_siread({qualified, *rv});
                }
            }
        }
    }
    return visible;
}

int64_t StorageEngine::update(const std::string& table, std::function<bool(const Row&)> match,
                               const std::map<std::string, Value>& updates) {
    std::string qualified = resolve_table_name(table);
    auto h = mgr_.acquire(next_call_xid(),
                          {LockTargetType::Table, qualified}, LockMode::Exclusive);
    auto rows_it = data_.find(qualified);
    if (rows_it == data_.end()) {
        throw std::runtime_error("表 \"" + table + "\" 不存在");
    }

    bool index_affected = updates_affect_index(qualified, updates);
    int64_t count = 0;

    // Phase 1: 非事务 update = 同 row_id 写新版本 (旧版本被覆盖语义丢失 → 调用方
    // 在非事务下接受此行为, 行为与旧 in-place 相同: 业务列覆盖, 系统列保持)
    auto all_rows = materialize_all(*rows_it->second);
    for (auto& row : all_rows) {
        if (!match(row)) continue;

        int64_t rowid = std::get<int64_t>(row.at("_rowid"));

        // 写新版本: 系统列保持 0/0/0 (非事务), 业务列用 updates 覆盖.
        // 擦除旧版本再 put, 与原 in-place 行为等价.
        for (const auto& [k, v] : updates) {
            row[k] = v;
        }
        row["_xmax"] = static_cast<int64_t>(0);
        row["_cid"]  = static_cast<int64_t>(0);

        // O-1: 二进制 payload (rowid + updates + new_row)
        jiamiao::UpdatePayload up;
        up.rowid = rowid;
        for (const auto& [k, v] : updates) {
            up.updates[k] = v;
        }
        up.new_row = row;
        write_wal(WalOp::kUpdate, qualified, jiamiao::wal_encode_update(up));

        rows_it->second->erase_all_for(rowid);
        TupleHeader hdr = header_from_row(qualified, rowid, row);
        auto tup = BinaryRowCodec::row_to_tuple(row, hdr);
        rows_it->second->put(rowid, tup, ++wal_seq_);
        count++;
    }

    if (index_affected) rebuild_indexes(qualified);
    maybe_checkpoint();
    return count;
}

int64_t StorageEngine::remove(const std::string& table, std::function<bool(const Row&)> match) {
    std::string qualified = resolve_table_name(table);
    auto h = mgr_.acquire(next_call_xid(),
                          {LockTargetType::Table, qualified}, LockMode::Exclusive);
    auto rows_it = data_.find(qualified);
    if (rows_it == data_.end()) {
        throw std::runtime_error("表 \"" + table + "\" 不存在");
    }

    // Phase 1: 非事务 remove = 物理删除匹配行. 做法: 收集要删的 row_id,
    // 重建 MemTable 时跳过这些.
    auto all_rows = materialize_all(*rows_it->second);
    std::vector<int64_t> to_delete;
    for (const auto& row : all_rows) {
        if (!match(row)) continue;
        int64_t rowid = std::get<int64_t>(row.at("_rowid"));
        // O-1: 二进制 payload (rowid, has_xmax=false → 物理删除)
        jiamiao::DeletePayload dp;
        dp.rowid = rowid;
        dp.has_xmax = false;
        write_wal(WalOp::kDelete, qualified, jiamiao::wal_encode_delete(dp));
        to_delete.push_back(rowid);
    }

    if (!to_delete.empty()) {
        auto new_mt = std::make_unique<MemTable>(qualified);
        for (const auto& row : all_rows) {
            int64_t rowid = std::get<int64_t>(row.at("_rowid"));
            if (std::find(to_delete.begin(), to_delete.end(), rowid) != to_delete.end()) continue;
            TupleHeader hdr = header_from_row(qualified, rowid, row);
            auto tup = BinaryRowCodec::row_to_tuple(row, hdr);
            new_mt->put(rowid, tup, ++wal_seq_);
        }
        data_[qualified] = std::move(new_mt);
        rebuild_indexes(qualified);
    }

    auto schema_it = tables_.find(qualified);
    if (schema_it != tables_.end()) {
        schema_it->second.row_count = data_[qualified]->size();
    }

    maybe_checkpoint();
    return static_cast<int64_t>(to_delete.size());
}

/* ─── 事务感知操作 ─── */

jiamiao::TransactionManager& StorageEngine::txn_mgr() {
    return *txn_mgr_;
}

Row StorageEngine::insert_with_txn(const std::string& table, const Row& row) {
    std::string qualified = resolve_table_name(table);
    auto h = mgr_.acquire(next_call_xid(),
                          {LockTargetType::Table, qualified}, LockMode::Exclusive);
    auto schema_it = tables_.find(qualified);
    if (schema_it == tables_.end()) {
        throw std::runtime_error("表 \"" + table + "\" 不存在");
    }

    auto validated = validate(schema_it->second, row);
    int64_t id = row_ids_[qualified] + 1;
    row_ids_[qualified] = id;
    validated["_rowid"] = id;

    // 分配 XID (首次写触发)
    jiamiao::TransactionId xid = txn_mgr_->assign_xid();
    validated["_xmin"] = static_cast<int64_t>(xid);
    validated["_xmax"] = static_cast<int64_t>(0);  // 未删除
    validated["_cid"]  = static_cast<int64_t>(txn_mgr_->get_current_command_id());

    // PRIMARY KEY 唯一性检查 + SSI SIREAD 注册 (防 phantom)
    {
        auto snap = txn_mgr_->get_snapshot();
        auto cid = txn_mgr_->get_current_command_id();
        auto all = data_[qualified]->scan_all();
        for (const auto& col : schema_it->second.columns) {
            if (!col.primary_key) continue;
            auto pk_it = validated.find(col.name);
            if (pk_it == validated.end()) continue;
            for (const auto& tup : all) {
                Row existing = tuple_to_row_for_visibility(tup);
                if (!jiamiao::check_tuple_visibility(existing, xid, snap, cid, txn_mgr_->clog()))
                    continue;
                auto ev = existing.find(col.name);
                if (ev != existing.end() && ev->second == pk_it->second) {
                    throw std::runtime_error("重复键值违反唯一约束 \"" + col.name + "\"");
                }
                // SSI: 把所有已存在的 PK 行注册为 SIREAD
                auto rid_it = existing.find("_rowid");
                if (rid_it != existing.end()) {
                    if (auto* rv = std::get_if<int64_t>(&rid_it->second)) {
                        txn_mgr_->register_siread({qualified, *rv});
                    }
                }
            }
        }
    }

    // 注册 Undo 记录
    txn_mgr_->add_undo_record(jiamiao::UndoRecord(
        jiamiao::UndoOp::INSERT, qualified, id, {}, validated));

    // O-1: WAL 二进制 payload (rowid + row); xid 在 WALRecordV3.xid
    jiamiao::InsertPayload ip;
    ip.rowid = id;
    ip.row = validated;
    write_wal(WalOp::kInsert, qualified, jiamiao::wal_encode_insert(ip), xid);

    // Phase 1: insert → MemTable::put. 系统列已在 validated 中 (_xmin=xid, _xmax=0, _cid=cid)
    TupleHeader hdr = header_from_row(qualified, id, validated);
    auto tup = BinaryRowCodec::row_to_tuple(validated, hdr);
    data_[qualified]->put(id, tup, ++wal_seq_);
    update_indexes(qualified, id, validated);
    schema_it->second.row_count = data_[qualified]->size();
    maybe_checkpoint();

    return validated;
}

int64_t StorageEngine::update_with_txn(const std::string& table,
                                        std::function<bool(const Row&)> match,
                                        const std::map<std::string, Value>& updates) {
    std::string qualified = resolve_table_name(table);
    auto h = mgr_.acquire(next_call_xid(),
                          {LockTargetType::Table, qualified}, LockMode::Exclusive);
    auto rows_it = data_.find(qualified);
    if (rows_it == data_.end()) {
        throw std::runtime_error("表 \"" + table + "\" 不存在");
    }

    jiamiao::TransactionId xid = txn_mgr_->assign_xid();
    auto snap = txn_mgr_->get_snapshot();
    int32_t cid = txn_mgr_->get_current_command_id();
    bool index_affected = updates_affect_index(qualified, updates);
    int64_t count = 0;
    std::vector<Row> new_versions;

    auto all = rows_it->second->scan_all();
    for (const auto& tup : all) {
        Row row = tuple_to_row_for_visibility(tup);
        // 可见性检查: 只更新可见的行
        if (!jiamiao::check_tuple_visibility(row, xid, snap, cid, txn_mgr_->clog()))
            continue;

        if (match(row)) {
            // 保存修改前的行 (用于 Undo, 此时 _xmax 还是旧值)
            Row old_row = row;
            int64_t rowid = std::get<int64_t>(row.at("_rowid"));

            // SSI: 注册 rw-antidependency (本事务写入该行, 与所有读过它的活跃事务形成 rw 边)
            txn_mgr_->register_write({qualified, rowid});

            // 1. 创建新行版本 (基于 old_row)
            Row new_row = row;
            for (const auto& [k, v] : updates) {
                new_row[k] = v;
            }
            // 覆盖系统列为新版本的值
            new_row["_xmin"] = static_cast<int64_t>(xid);
            new_row["_xmax"] = static_cast<int64_t>(0);
            new_row["_cid"]  = static_cast<int64_t>(cid);

            // PK 唯一性检查 (如果更新了 PK 列)
            {
                auto schema_it = tables_.find(qualified);
                if (schema_it != tables_.end()) {
                    for (const auto& col : schema_it->second.columns) {
                        if (!col.primary_key) continue;
                        auto up_it = updates.find(col.name);
                        if (up_it == updates.end()) continue; // 未修改此 PK 列
                        for (const auto& existing_tup : all) {
                            Row existing = tuple_to_row_for_visibility(existing_tup);
                            if (!jiamiao::check_tuple_visibility(existing, xid, snap, cid, txn_mgr_->clog()))
                                continue;
                            // 排除正在更新的行自身 (通过 _rowid)
                            auto er = existing.find("_rowid");
                            if (er != existing.end() && std::get<int64_t>(er->second) == rowid)
                                continue;
                            auto ev = existing.find(col.name);
                            if (ev != existing.end() && ev->second == up_it->second) {
                                throw std::runtime_error("重复键值违反唯一约束 \"" + col.name + "\"");
                            }
                        }
                    }
                }
            }

            // 2. 注册 Undo
            txn_mgr_->add_undo_record(jiamiao::UndoRecord(
                jiamiao::UndoOp::UPDATE, qualified, rowid, old_row, new_row));

            // 3. O-1: WAL 二进制 payload (rowid + updates + new_row); xid 在 WALRecordV3.xid
            jiamiao::UpdatePayload up;
            up.rowid = rowid;
            for (const auto& [k, v] : updates) {
                up.updates[k] = v;
            }
            up.new_row = new_row;
            write_wal(WalOp::kUpdate, qualified, jiamiao::wal_encode_update(up), xid);

            // 4. 物理操作 MemTable: 擦除旧版本 + 写新版本. 与原 in-place 行为等价:
            //    旧版本在 MemTable 中不再存在 (原行为是 row._xmax=xid), 扫描时仅新版本可见.
            rows_it->second->erase_all_for(rowid);
            TupleHeader hdr = header_from_row(qualified, rowid, new_row);
            auto tup = BinaryRowCodec::row_to_tuple(new_row, hdr);
            rows_it->second->put(rowid, tup, ++wal_seq_);
            new_versions.push_back(std::move(new_row));
            count++;
        }
    }

    if (index_affected) rebuild_indexes(qualified);
    maybe_checkpoint();
    return count;
}

int64_t StorageEngine::remove_with_txn(const std::string& table,
                                        std::function<bool(const Row&)> match) {
    std::string qualified = resolve_table_name(table);
    auto h = mgr_.acquire(next_call_xid(),
                          {LockTargetType::Table, qualified}, LockMode::Exclusive);
    auto rows_it = data_.find(qualified);
    if (rows_it == data_.end()) {
        throw std::runtime_error("表 \"" + table + "\" 不存在");
    }

    jiamiao::TransactionId xid = txn_mgr_->assign_xid();
    auto snap = txn_mgr_->get_snapshot();
    int32_t cid = txn_mgr_->get_current_command_id();
    int64_t count = 0;

    auto all = rows_it->second->scan_all();
    for (const auto& tup : all) {
        Row row = tuple_to_row_for_visibility(tup);
        // 可见性检查: 只删除可见的行
        if (!jiamiao::check_tuple_visibility(row, xid, snap, cid, txn_mgr_->clog()))
            continue;

        if (match(row)) {
            int64_t rowid = std::get<int64_t>(row.at("_rowid"));

            // 保存修改前的行 (用于 Undo)
            Row old_row = row;

            // SSI: 注册 rw-antidependency
            txn_mgr_->register_write({qualified, rowid});

            // 构造 tombstone row (在 new_row 中保持旧数据, 改 _xmax/_cid 标记删除)
            // 旧版本在 MemTable 中保留 (其 _xmax 仍为 0). visibility 仍可见.
            // Phase 1: 靠"重建受影响 row_id" 做 undo (见 apply_undo_record).
            Row tombstone_row = old_row;
            tombstone_row["_xmax"] = static_cast<int64_t>(xid);
            tombstone_row["_cid"]  = static_cast<int64_t>(cid);

            // 注册 Undo (old_row 保留了修改前的 _xmax / _cid)
            txn_mgr_->add_undo_record(jiamiao::UndoRecord(
                jiamiao::UndoOp::DELETE, qualified, rowid, old_row, {}));

            // 写 tombstone 版本: 擦除旧版本 + put 新 tombstone. 与原 in-place 行为等价:
            //    原行为是 row._xmax=xid; 我们通过 put 一个带 xmax 的 tuple 模拟.
            //    旧版本在 MemTable 中不再存在, 扫描时仅 tombstone 可见 (但 invisible by xmax).
            rows_it->second->erase_all_for(rowid);
            TupleHeader hdr = header_from_row(qualified, rowid, tombstone_row);
            auto tombstone_tup = BinaryRowCodec::row_to_tuple(tombstone_row, hdr);
            rows_it->second->put(rowid, tombstone_tup, ++wal_seq_);

            // O-1: WAL 二进制 payload (rowid + xmax); xid 在 WALRecordV3.xid
            jiamiao::DeletePayload dp;
            dp.rowid = rowid;
            dp.has_xmax = true;
            dp.xmax = static_cast<uint32_t>(xid);
            write_wal(WalOp::kDelete, qualified, jiamiao::wal_encode_delete(dp), xid);

            count++;
        }
    }

    maybe_checkpoint();
    return count;
}

// 反向应用单条 undo 记录 (内部使用)
// Phase 1 策略: 重建受影响 row_id 的 MemTable 状态. 把 rec.row_id 的所有版本擦除,
// 然后按 rec.op 决定是否放回 rec.old_row (作为新 Tuple). 这样做最简单且与原行为等价:
//   INSERT undo: 擦掉该 row_id, 不放回任何东西
//   UPDATE undo: 擦掉该 row_id, 放回 old_row (恢复 pre-txn 状态)
//   DELETE undo: 擦掉该 row_id, 放回 old_row (恢复 pre-txn 状态, 等价于清除 tombstone)
static void apply_undo_record(const jiamiao::UndoRecord& rec,
                              std::map<std::string, std::unique_ptr<MemTable>>& data_,
                              jiamiao::TransactionId our_xid,
                              int64_t& wal_seq) {
    (void)our_xid;  // Phase 1: 不再用 our_xid 定位 (erase_all_for 已覆盖所有版本)
    auto data_it = data_.find(rec.table_name);
    if (data_it == data_.end()) return;
    auto& mt = data_it->second;

    // 1. 物理擦除该 row_id 全部版本
    mt->erase_all_for(rec.row_id);

    // 2. 按 op 决定是否回填
    if (rec.op == jiamiao::UndoOp::INSERT) {
        // 不回填, 等同"删除"
        return;
    }
    // UPDATE / DELETE: 回填 old_row (含原 _xmin/_xmax/_cid)
    if (rec.old_row.empty()) {
        // 防御: 没有 old_row 时不回填, 避免半残
        return;
    }
    TupleHeader hdr = header_from_row(rec.table_name, rec.row_id, rec.old_row);
    auto tup = BinaryRowCodec::row_to_tuple(rec.old_row, hdr);
    mt->put(rec.row_id, tup, static_cast<uint64_t>(++wal_seq));
}

void StorageEngine::apply_undo() {
    const auto& records = txn_mgr_->undo_records();
    jiamiao::TransactionId our_xid = txn_mgr_->get_current_xid();

    // 反向遍历所有 undo records
    for (auto it = records.rbegin(); it != records.rend(); ++it) {
        apply_undo_record(*it, data_, our_xid, wal_seq_);
    }

    // 重建受影响的索引
    for (auto it = records.rbegin(); it != records.rend(); ++it) {
        rebuild_indexes(it->table_name);
    }
}

void StorageEngine::apply_undo_to(size_t count) {
    const auto& records = txn_mgr_->undo_records();
    jiamiao::TransactionId our_xid = txn_mgr_->get_current_xid();

    // 只回滚最后 count 条 (反向, 后进先出)
    if (count > records.size()) count = records.size();
    size_t start = records.size() - count;
    for (size_t i = records.size(); i > start; --i) {
        apply_undo_record(records[i - 1], data_, our_xid, wal_seq_);
    }
    // 索引重建
    for (size_t i = records.size(); i > start; --i) {
        rebuild_indexes(records[i - 1].table_name);
    }
}

void StorageEngine::write_xact_commit(jiamiao::TransactionId xid) {
    auto h = mgr_.acquire(next_call_xid(),
                          {LockTargetType::Table, kWalTarget}, LockMode::Exclusive);
    // O-1: payload 为空, xid 在 WALRecordV3.xid
    write_wal(WalOp::kXactCommit, "", jiamiao::wal_encode_empty(), xid);
}

void StorageEngine::write_xact_abort(jiamiao::TransactionId xid) {
    auto h = mgr_.acquire(next_call_xid(),
                          {LockTargetType::Table, kWalTarget}, LockMode::Exclusive);
    // O-1: payload 为空, xid 在 WALRecordV3.xid
    write_wal(WalOp::kXactAbort, "", jiamiao::wal_encode_empty(), xid);
}

int64_t StorageEngine::vacuum() {
    // 持有 catalog 锁, 防止 DDL 与 vacuum 并发
    auto h = mgr_.acquire(next_call_xid(),
                          {LockTargetType::Table, kCatalogTarget}, LockMode::Shared);

    int64_t total_cleaned = 0;
    auto& clog = txn_mgr_->clog();
    auto oldest = txn_mgr_->get_oldest_active_xid();

    for (auto& [tname, mt] : data_) {
        int64_t before = static_cast<int64_t>(mt->size());
        // 物化所有版本, 决定哪些 row_id 物理擦除 / 冻结
        auto all = mt->scan_all();
        std::vector<int64_t> to_erase;
        std::vector<int64_t> to_freeze;
        for (const auto& tup : all) {
            Row row = tuple_to_row_for_visibility(tup);
            auto xmin_it = row.find("_xmin");
            auto xmax_it = row.find("_xmax");
            if (xmin_it == row.end() || xmax_it == row.end()) continue;

            auto* xmin_v = std::get_if<int64_t>(&xmin_it->second);
            auto* xmax_v = std::get_if<int64_t>(&xmax_it->second);
            if (!xmin_v || !xmax_v) continue;

            jiamiao::TransactionId xmin = static_cast<jiamiao::TransactionId>(*xmin_v);
            jiamiao::TransactionId xmax = static_cast<jiamiao::TransactionId>(*xmax_v);

            // 跳过活跃事务的行
            if (xmin >= oldest && xmin != jiamiao::FrozenTransactionId) continue;
            if (xmax != 0 && xmax >= oldest) continue;

            auto xmin_status = clog.get_status(xmin);
            auto xmax_status = (xmax == 0) ? jiamiao::TransactionStatus::COMMITTED
                                            : clog.get_status(xmax);

            int64_t rowid = static_cast<int64_t>(tup.hdr.row_id);

            // 情况 1: xmin 已回滚 → 行不应存在, 物理删除该 row_id
            if (xmin_status == jiamiao::TransactionStatus::ABORTED) {
                to_erase.push_back(rowid);
                continue;
            }

            // 情况 2: xmin 已提交, xmax 已提交 → 行已被删除, 物理擦除
            if (xmin_status == jiamiao::TransactionStatus::COMMITTED &&
                xmax != 0 && xmax_status == jiamiao::TransactionStatus::COMMITTED) {
                to_erase.push_back(rowid);
                continue;
            }

            // 情况 3: xmin 已提交 (非冻结), xmax 仍有效 → 冻结 xmin
            // 做法: 把当前版本擦除, put 一个新版本 (xmin=Frozen, 业务数据不变).
            //   与原 in-place 行为等价: 读到的 _xmin=FrozenTransactionId.
            //   推迟到本块结束后批量执行 (erase 阶段 + freeze 阶段分开).
            if (xmin_status == jiamiao::TransactionStatus::COMMITTED &&
                xmin != jiamiao::FrozenTransactionId) {
                to_freeze.push_back(rowid);
            }
        }

        // 阶段 2: 物理擦除 dead row_ids
        std::sort(to_erase.begin(), to_erase.end());
        to_erase.erase(std::unique(to_erase.begin(), to_erase.end()), to_erase.end());
        for (int64_t rid : to_erase) {
            mt->erase_all_for(rid);
        }
        total_cleaned += static_cast<int64_t>(to_erase.size());

        // 阶段 3: 冻结: 擦除 + put frozen 版本
        std::sort(to_freeze.begin(), to_freeze.end());
        to_freeze.erase(std::unique(to_freeze.begin(), to_freeze.end()), to_freeze.end());
        for (int64_t rid : to_freeze) {
            auto opt = mt->get_latest(rid);
            if (!opt) continue;
            Row row = tuple_to_row_for_visibility(*opt);
            row["_xmin"] = static_cast<int64_t>(jiamiao::FrozenTransactionId);
            mt->erase_all_for(rid);
            TupleHeader hdr = header_from_row(tname, rid, row);
            auto tup = BinaryRowCodec::row_to_tuple(row, hdr);
            mt->put(rid, tup, ++wal_seq_);
        }

        // 更新行数 + 索引
        auto schema_it = tables_.find(tname);
        if (schema_it != tables_.end()) {
            schema_it->second.row_count = static_cast<int64_t>(mt->size());
        }
        if (before != static_cast<int64_t>(mt->size())) {
            rebuild_indexes(tname);
        }
    }

    // 同时尝试 anti-wraparound (顺便清理 CLog)
    txn_mgr_->maybe_anti_wraparound();

    return total_cleaned;
}

/* ─── 索引 ─── */

RowSet StorageEngine::index_lookup(const std::string& table, const std::string& column, const Value& value) {
    std::string qualified = resolve_table_name(table);
    auto h = mgr_.acquire(next_call_xid(),
                          {LockTargetType::Table, qualified}, LockMode::Shared);
    auto idx_it = indexes_.find(qualified);
    if (idx_it == indexes_.end()) return {};

    std::string key = value_to_string(value);
    for (const auto& idx : idx_it->second) {
        if (idx.column == column) {
            auto entry_it = idx.entries.find(key);
            if (entry_it == idx.entries.end()) return {};

            // Phase 1: index 存的是 row_id, 走 MemTable::get_latest(rid) 拿最新版
            RowSet result;
            auto data_it = data_.find(qualified);
            if (data_it == data_.end()) return {};
            for (int64_t rid : entry_it->second) {
                auto opt = data_it->second->get_latest(rid);
                if (opt) result.push_back(tuple_to_row_for_visibility(*opt));
            }
            return result;
        }
    }
    return {};
}

RowSet StorageEngine::index_lookup_with_snapshot(const std::string& table,
                                                   const std::string& column,
                                                   const Value& value,
                                                   const jiamiao::Snapshot& snap,
                                                   jiamiao::TransactionId xid,
                                                   int32_t cid) {
    std::string qualified = resolve_table_name(table);
    auto h = mgr_.acquire(next_call_xid(),
                          {LockTargetType::Table, qualified}, LockMode::Shared);
    auto idx_it = indexes_.find(qualified);
    if (idx_it == indexes_.end()) return {};

    std::string key = value_to_string(value);
    for (const auto& idx : idx_it->second) {
        if (idx.column == column) {
            auto entry_it = idx.entries.find(key);
            if (entry_it == idx.entries.end()) return {};

            // Phase 1: index 存 row_id, 走 get_latest(rid) 拿最新版
            RowSet result;
            auto data_it = data_.find(qualified);
            if (data_it == data_.end()) return {};
            for (int64_t rid : entry_it->second) {
                auto opt = data_it->second->get_latest(rid);
                if (!opt) continue;
                Row row = tuple_to_row_for_visibility(*opt);
                if (jiamiao::check_tuple_visibility(row, xid, snap, cid, txn_mgr_->clog())) {
                    result.push_back(std::move(row));
                    // SSI: 记录 SIREAD (仅 SERIALIZABLE 下有效)
                    auto rid_it = row.find("_rowid");
                    if (rid_it != row.end()) {
                        if (auto* rv = std::get_if<int64_t>(&rid_it->second)) {
                            txn_mgr_->register_siread({qualified, *rv});
                        }
                    }
                }
            }
            return result;
        }
    }
    return {};
}

bool StorageEngine::has_index(const std::string& table, const std::string& column) {
    std::string qualified = resolve_table_name(table);
    auto idx_it = indexes_.find(qualified);
    if (idx_it == indexes_.end()) return false;
    for (const auto& idx : idx_it->second) {
        if (idx.column == column) return true;
    }
    return false;
}

void StorageEngine::create_index(const std::string& table, const std::string& column) {
    std::string qualified = resolve_table_name(table);
    auto idx_it = indexes_.find(qualified);
    if (idx_it == indexes_.end()) return;

    for (const auto& idx : idx_it->second) {
        if (idx.column == column) return; // 已存在
    }

    IndexInfo idx;
    idx.column = column;
    auto data_it = data_.find(qualified);
    if (data_it == data_.end()) {
        idx_it->second.push_back(std::move(idx));
        return;
    }
    // Phase 1: index 存 row_id, 不是行位置
    auto all = data_it->second->scan_all();
    for (const auto& tup : all) {
        Row row = tuple_to_row_for_visibility(tup);
        auto it = row.find(column);
        if (it != row.end()) {
            int64_t rid = static_cast<int64_t>(tup.hdr.row_id);
            idx.entries[value_to_string(it->second)].push_back(rid);
        }
    }
    idx_it->second.push_back(std::move(idx));
}

/* ─── 内部 ─── */

void StorageEngine::replay_record(const WALRecordV3& rec) {
    // O-1: 直接吃 v3 二进制 payload, 不再走 json::parse
    // xid 在 rec.xid (0 表示无 xid); op 是 enum tag; table 是 qualified name.
    const jiamiao::TransactionId rec_xid =
        (rec.xid != 0) ? static_cast<jiamiao::TransactionId>(rec.xid)
                       : jiamiao::InvalidTransactionId;
    const auto op = static_cast<jiamiao::WalOp>(rec.op);

    // Catalog 操作 (payload 为空 / 简短)
    switch (op) {
        case jiamiao::WalOp::kCreateDatabase: {
            if (!catalog_->database_exists(rec.table)) {
                catalog_->create_database(rec.table);
            }
            return;
        }
        case jiamiao::WalOp::kDropDatabase: {
            if (catalog_->database_exists(rec.table)) {
                catalog_->drop_database(rec.table);
            }
            return;
        }
        case jiamiao::WalOp::kCreateSchema: {
            jiamiao::CreateSchemaPayload p;
            if (!jiamiao::wal_decode_create_schema(rec.data, &p)) return;
            if (!catalog_->schema_exists(rec.table, p.schema)) {
                catalog_->create_schema(rec.table, p.schema);
            }
            return;
        }
        case jiamiao::WalOp::kCreateUser: {
            // 重放时不恢复密码 (密码已通过 checkpoint 恢复). 仅占位.
            return;
        }
        case jiamiao::WalOp::kDropUser: {
            if (catalog_->user_exists(rec.table)) {
                catalog_->drop_user(rec.table);
            }
            return;
        }

        case jiamiao::WalOp::kCreateTable: {
            jiamiao::CreateTablePayload p;
            if (!jiamiao::wal_decode_create_table(rec.data, &p)) return;
            TableSchema schema;
            schema.name = rec.table;
            schema.row_count = 0;
            schema.columns = std::move(p.columns);
            tables_[rec.table] = std::move(schema);
            data_[rec.table] = std::make_unique<MemTable>(rec.table);
            row_ids_[rec.table] = 0;
            indexes_[rec.table] = {};

            for (const auto& c : tables_[rec.table].columns) {
                if (c.primary_key) create_index(rec.table, c.name);
            }
            return;
        }

        case jiamiao::WalOp::kDropTable: {
            tables_.erase(rec.table);
            data_.erase(rec.table);
            row_ids_.erase(rec.table);
            indexes_.erase(rec.table);
            return;
        }

        case jiamiao::WalOp::kXactCommit: {
            if (rec_xid != jiamiao::InvalidTransactionId) {
                txn_mgr_->clog().set_status(rec_xid, jiamiao::TransactionStatus::COMMITTED);
            }
            return;
        }
        case jiamiao::WalOp::kXactAbort: {
            if (rec_xid != jiamiao::InvalidTransactionId) {
                txn_mgr_->clog().set_status(rec_xid, jiamiao::TransactionStatus::ABORTED);
            }
            return;
        }

        case jiamiao::WalOp::kInsert: {
            // 事务过滤: 跳过未提交或已回滚的记录
            if (rec_xid != jiamiao::InvalidTransactionId) {
                auto status = txn_mgr_->clog().get_status(rec_xid);
                if (status != jiamiao::TransactionStatus::COMMITTED) return;
            }
            auto mt_it = data_.find(rec.table);
            if (mt_it == data_.end()) return;  // 表还没 create_table replay 到
            auto& mt = mt_it->second;

            jiamiao::InsertPayload p;
            if (!jiamiao::wal_decode_insert(rec.data, &p)) return;

            TupleHeader hdr = header_from_row(rec.table, p.rowid, p.row);
            auto tup = BinaryRowCodec::row_to_tuple(p.row, hdr);
            mt->put(p.rowid, tup, ++wal_seq_);
            update_indexes(rec.table, p.rowid, p.row);

            auto schema_it = tables_.find(rec.table);
            if (schema_it != tables_.end())
                schema_it->second.row_count = static_cast<int64_t>(mt->size());

            row_ids_[rec.table] = std::max(row_ids_[rec.table], p.rowid);
            return;
        }

        case jiamiao::WalOp::kUpdate: {
            if (rec_xid != jiamiao::InvalidTransactionId) {
                auto status = txn_mgr_->clog().get_status(rec_xid);
                if (status != jiamiao::TransactionStatus::COMMITTED) return;
            }
            auto rows_it = data_.find(rec.table);
            if (rows_it == data_.end()) return;
            auto& mt = rows_it->second;

            jiamiao::UpdatePayload p;
            if (!jiamiao::wal_decode_update(rec.data, &p)) return;

            // O-1: 总是有 new_row, put 新版本 (旧版本保留, MVCC 由 scan_with_snapshot 过滤)
            TupleHeader hdr = header_from_row(rec.table, p.rowid, p.new_row);
            auto tup = BinaryRowCodec::row_to_tuple(p.new_row, hdr);
            mt->put(p.rowid, tup, ++wal_seq_);

            if (updates_affect_index(rec.table, p.updates)) rebuild_indexes(rec.table);
            rebuild_indexes(rec.table);
            auto schema_it = tables_.find(rec.table);
            if (schema_it != tables_.end())
                schema_it->second.row_count = static_cast<int64_t>(mt->size());
            return;
        }

        case jiamiao::WalOp::kDelete: {
            if (rec_xid != jiamiao::InvalidTransactionId) {
                auto status = txn_mgr_->clog().get_status(rec_xid);
                if (status != jiamiao::TransactionStatus::COMMITTED) return;
            }
            auto rows_it = data_.find(rec.table);
            if (rows_it == data_.end()) return;
            auto& mt = rows_it->second;

            jiamiao::DeletePayload p;
            if (!jiamiao::wal_decode_delete(rec.data, &p)) return;

            if (p.has_xmax) {
                // MVCC tombstone: 取最新版本, 标 _xmax, put 回去
                auto opt = mt->get_latest(p.rowid);
                if (opt) {
                    Row row = tuple_to_row_for_visibility(*opt);
                    row["_xmax"] = static_cast<int64_t>(p.xmax);
                    TupleHeader hdr = header_from_row(rec.table, p.rowid, row);
                    auto tup = BinaryRowCodec::row_to_tuple(row, hdr);
                    mt->put(p.rowid, tup, ++wal_seq_);
                }
            } else {
                // 物理删除
                mt->erase_all_for(p.rowid);
                rebuild_indexes(rec.table);
                auto schema_it = tables_.find(rec.table);
                if (schema_it != tables_.end())
                    schema_it->second.row_count = static_cast<int64_t>(mt->size());
            }
            return;
        }

        case jiamiao::WalOp::kUnknown:
        default:
            return;
    }
}

void StorageEngine::checkpoint() {
    // Checkpoint 事务感知:
    // 1. 调用方应持有 lifecycle_mutex_ (save/close/load 已持有)
    //    maybe_checkpoint() 入口会先获取, 防止与 DML 路径并发
    // 2. 先 sync WAL, 确保所有已写入的 WAL 条目都持久化
    // 3. 序列化数据 + 事务状态 (next_xid, CLog) + Catalog
    // 4. 写入 checkpoint.json

    // 强制 WAL 刷盘 (确保 last_seq 之前的所有记录都持久化)
    {
        auto h = mgr_.acquire(next_call_xid(),
                              {LockTargetType::Table, kWalTarget}, LockMode::Exclusive);
        wal_->sync();
    }

    Checkpoint ckp;
    ckp.last_seq = wal_seq_;
    ckp.timestamp = time(nullptr);

    for (const auto& [name, schema] : tables_) {
        ckp.tables.push_back(schema);

        // O-2: 强类型 table_rows, 无 JSON 中间层
        auto& rows = ckp.table_rows[name];
        auto data_it = data_.find(name);
        if (data_it != data_.end()) {
            auto all = data_it->second->scan_all();
            for (const auto& tup : all) {
                Row row = tuple_to_row_for_visibility(tup);
                int64_t rid = 0;
                auto rid_it = row.find("_rowid");
                if (rid_it != row.end()) {
                    if (auto* v = std::get_if<int64_t>(&rid_it->second)) rid = *v;
                }
                rows.emplace_back(rid, std::move(row));
            }
        }
        ckp.row_id_counters[name] = row_ids_[name];

        // O-2: 强类型 indexes_data, 无 JSON 中间层
        auto idx_it = indexes_.find(name);
        if (idx_it != indexes_.end()) {
            ckp.indexes_data[name] = idx_it->second;
        }
    }

    // 序列化事务状态: next_xid + CLog (含 SSI 状态)
    ckp.next_xid = txn_mgr_->get_next_xid();
    // O-4: 改为 binary CLog (clog_encode)
    ckp.clog_bytes = jiamiao::clog_encode(txn_mgr_->clog());
    ckp.clog_entries = json(nullptr);  // 显式清空 JSON 兜底

    // O-3: 序列化 Catalog 为二进制
    ckp.catalog_bytes = jiamiao::catalog_encode(*catalog_);
    ckp.catalog_data = json(nullptr);  // 清掉旧字段

    // 写入 checkpoint
    ckp_mgr_->save(ckp);

    // 截断 WAL: 保留的 WAL 记录对应的修改已落盘到 checkpoint, 可丢弃
    // (崩溃恢复时: 加载 checkpoint + 重放保留的 WAL)
    {
        auto h = mgr_.acquire(next_call_xid(),
                              {LockTargetType::Table, kWalTarget}, LockMode::Exclusive);
        wal_->truncate(ckp.last_seq);
        wal_->sync();
        wal_seq_ = wal_->current_seq();
    }
}

void StorageEngine::maybe_checkpoint() {
    if (checkpoint_interval_ > 0 && wal_seq_ % checkpoint_interval_ == 0) {
        // 持 lifecycle_mutex_ 防止与 DML 并发
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        checkpoint();
    }
}

bool StorageEngine::updates_affect_index(const std::string& table, const std::map<std::string, Value>& updates) {
    auto idx_it = indexes_.find(table);
    if (idx_it == indexes_.end()) return false;
    for (const auto& idx : idx_it->second) {
        if (updates.count(idx.column)) return true;
    }
    return false;
}

void StorageEngine::rebuild_indexes(const std::string& table) {
    auto idx_it = indexes_.find(table);
    if (idx_it == indexes_.end()) return;

    auto data_it = data_.find(table);
    if (data_it == data_.end()) return;
    for (auto& idx : idx_it->second) {
        idx.entries.clear();
        auto all = data_it->second->scan_all();
        for (const auto& tup : all) {
            Row row = tuple_to_row_for_visibility(tup);
            auto cell = row.find(idx.column);
            if (cell != row.end()) {
                int64_t rid = static_cast<int64_t>(tup.hdr.row_id);
                idx.entries[value_to_string(cell->second)].push_back(rid);
            }
        }
    }
}

void StorageEngine::update_indexes(const std::string& table, int64_t row_id, const Row& row) {
    auto idx_it = indexes_.find(table);
    if (idx_it == indexes_.end()) return;

    // Phase 1: row_id 是 row_id (从 row["_rowid"] 来), 不是行位置.
    // 重复检查: 同 row_id 不重复注册 (即 update 同一行只记一次)
    for (auto& idx : idx_it->second) {
        auto cell = row.find(idx.column);
        if (cell == row.end()) continue;
        std::string k = value_to_string(cell->second);
        auto& list = idx.entries[k];
        if (std::find(list.begin(), list.end(), row_id) == list.end()) {
            list.push_back(row_id);
        }
    }
}

Row StorageEngine::validate(const TableSchema& schema, const Row& row) {
    Row result;
    for (const auto& col : schema.columns) {
        auto it = row.find(col.name);
        if (it != row.end()) {
            result[col.name] = coerce(col.type, it->second);
        } else if (col.primary_key && col.type == DataType::INTEGER) {
            result[col.name] = (int64_t)(row_ids_[schema.name] + 1);
        } else if (col.default_value.has_value()) {
            result[col.name] = col.default_value.value();
        } else if (!col.nullable) {
            throw std::runtime_error("字段 \"" + col.name + "\" 不能为 NULL");
        } else {
            result[col.name] = nullptr;
        }
    }
    return result;
}

Value StorageEngine::coerce(DataType type, const Value& v) {
    if (std::holds_alternative<std::nullptr_t>(v)) return nullptr;
    switch (type) {
        case DataType::INTEGER:
        case DataType::BIGINT:
            if (auto i = std::get_if<int64_t>(&v)) return *i;
            if (auto d = std::get_if<double>(&v)) return (int64_t)*d;
            if (auto s = std::get_if<std::string>(&v)) return (int64_t)std::stoll(*s);
            return v;
        case DataType::FLOAT:
        case DataType::DOUBLE:
            if (auto d = std::get_if<double>(&v)) return *d;
            if (auto i = std::get_if<int64_t>(&v)) return (double)*i;
            if (auto s = std::get_if<std::string>(&v)) return std::stod(*s);
            return v;
        case DataType::BOOLEAN:
            if (auto b = std::get_if<bool>(&v)) return *b;
            if (auto i = std::get_if<int64_t>(&v)) return *i != 0;
            if (auto s = std::get_if<std::string>(&v)) return *s == "true" || *s == "1";
            return v;
        case DataType::TEXT:
        case DataType::DATE:
        case DataType::TIMESTAMP:
            if (auto* s = std::get_if<std::string>(&v)) return *s;
            return value_to_string(v);
        default:
            return v;
    }
}

int64_t StorageEngine::next_seq() {
    return ++wal_seq_;
}

void StorageEngine::write_wal(jiamiao::WalOp op, const std::string& table,
                               std::string payload, jiamiao::TransactionId xid) {
    int64_t seq = next_seq();
    // O-1: payload 已是二进制 bytes; xid 直接放 WALRecordV3.xid (header), 不再嵌入 payload.
    // Phase 3: encode() 的 byte buffer 走 jmalloc (WALContext).
    // 出 scope 时 buffer 整体 reset, 实际字节已 write 到 fd.
    jiamiao::ScopeContext wal_scope(wal_ctx_);
    jiamiao::WALRecordV3 rec;
    rec.seq       = seq;
    rec.timestamp = time(nullptr);
    rec.op        = static_cast<uint16_t>(op);
    rec.table     = table;
    rec.xid       = (xid != jiamiao::InvalidTransactionId) ? static_cast<uint32_t>(xid) : 0u;
    rec.data      = std::move(payload);
    wal_->append(rec);
}
