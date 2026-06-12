#include "engine.h"
#include "transaction.h"
#include "lock_manager.h"
#include "tuple.h"
#include "memtable.h"
#include <filesystem>
#include <iostream>
#include <algorithm>
#include <sstream>
#include "json.h"

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
    wal_ = std::make_unique<jiamiao::WriteAheadLogV3>(data_dir + "/wal.log");
    ckp_mgr_ = std::make_unique<CheckpointManager>(data_dir);
    txn_mgr_ = std::make_unique<jiamiao::TransactionManager>();
    catalog_ = std::make_unique<Catalog>();
}

StorageEngine::~StorageEngine() { close(); }

void StorageEngine::load() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);

    // 0. 加载 Catalog
    auto ckp = ckp_mgr_->load();
    wal_seq_ = ckp.last_seq;

    bool is_old_checkpoint = ckp.catalog_data.is_null() || !ckp.catalog_data.contains("databases");
    if (!ckp.catalog_data.is_null() && ckp.catalog_data.contains("databases")) {
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
        if (ckp.table_data.find(t.name) != ckp.table_data.end()) {
            for (const auto& row_json : ckp.table_data[t.name]) {
                Row row;
                for (auto it = row_json.obj_begin(); it != row_json.obj_end(); ++it) {
                    const auto& v = it->second;
                    if (v.is_null()) row[it->first] = nullptr;
                    else if (v.is_int()) row[it->first] = v.get_int();
                    else if (v.is_float()) row[it->first] = v.get_float();
                    else if (v.is_bool()) row[it->first] = v.get_bool();
                    else row[it->first] = v.get_string();
                }
                // Phase 1: checkpoint 不带 xmin/xmax/cid, 用 InvalidTransactionId 占位.
                // load 阶段不需要 MVCC, 系统列由 tuple_to_row_for_visibility 补齐.
                int64_t rid = 0;
                auto rid_it = row.find("_rowid");
                if (rid_it != row.end()) {
                    if (auto* v = std::get_if<int64_t>(&rid_it->second)) rid = *v;
                }
                TupleHeader hdr = header_from_row(qualified_name, rid, row);
                auto t = BinaryRowCodec::row_to_tuple(row, hdr);
                data_[qualified_name]->put(rid, t, ++wal_seq_);
            }
        }

        // 恢复 row_id_counter
        if (ckp.row_id_counters.find(t.name) != ckp.row_id_counters.end()) {
            row_ids_[qualified_name] = ckp.row_id_counters[t.name];
        }

        // 恢复索引
        if (ckp.indexes.find(t.name) != ckp.indexes.end()) {
            for (const auto& idx_json : ckp.indexes[t.name]) {
                IndexInfo idx;
                idx.column = idx_json["column"].get_string();
                for (auto it = idx_json["entries"].obj_begin(); it != idx_json["entries"].obj_end(); ++it) {
                    std::vector<int64_t> indices;
                    for (const auto& v : it->second) indices.push_back(v.get_int());
                    idx.entries[it->first] = indices;
                }
                indexes_[qualified_name].push_back(std::move(idx));
            }
        }
    }

    // 2. 恢复事务状态
    if (ckp.next_xid >= jiamiao::FirstNormalTransactionId) {
        txn_mgr_->set_next_xid(ckp.next_xid);
    }
    if (!ckp.clog_entries.is_null()) {
        txn_mgr_->clog().from_json(ckp.clog_entries);
    }
    txn_mgr_->rebuild_active_from_clog();
    txn_mgr_->reset_context();

    // 3. 重放 WAL (v3 binary 或 v2 JSON fallback)
    wal_->open();
    auto records = wal_->replay(ckp.last_seq);
    for (const auto& v3rec : records) {
        // 把 v3 记录转回 WALRecord (替代 op enum 用 string, data JSON 文本 parse 回 json)
        WALRecord rec;
        rec.seq       = v3rec.seq;
        rec.timestamp = v3rec.timestamp;
        rec.op        = jiamiao::enum_to_op(v3rec.op);
        rec.table     = v3rec.table;
        if (!v3rec.data.empty()) {
            try {
                rec.data = json::parse(v3rec.data);
            } catch (...) {
                rec.data = json();  // 损坏的 JSON, 跳过
                continue;
            }
        }
        replay_record(rec);
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
    json data;
    data["name"] = name;
    write_wal("create_database", name, data);
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
    json data;
    data["name"] = name;
    write_wal("drop_database", name, data);
    maybe_checkpoint();
}

void StorageEngine::create_schema(const std::string& db_name, const std::string& schema_name) {
    auto h = mgr_.acquire(next_call_xid(),
                          {LockTargetType::Table, kCatalogTarget}, LockMode::Exclusive);
    catalog_->create_schema(db_name, schema_name);
    json data;
    data["database"] = db_name;
    data["schema"] = schema_name;
    write_wal("create_schema", db_name, data);
    maybe_checkpoint();
}

void StorageEngine::create_user(const std::string& name, const std::string& password) {
    auto h = mgr_.acquire(next_call_xid(),
                          {LockTargetType::Table, kCatalogTarget}, LockMode::Exclusive);
    catalog_->create_user(name, password);
    json data;
    data["name"] = name;
    // 不记录密码到 WAL
    write_wal("create_user", name, data);
    maybe_checkpoint();
}

void StorageEngine::drop_user(const std::string& name) {
    auto h = mgr_.acquire(next_call_xid(),
                          {LockTargetType::Table, kCatalogTarget}, LockMode::Exclusive);
    catalog_->drop_user(name);
    json data;
    data["name"] = name;
    write_wal("drop_user", name, data);
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

    json data;
    data["columns"] = json::array();
    for (const auto& c : columns) {
        json col;
        col["name"] = c.name;
        col["type"] = data_type_name(c.type);
        col["nullable"] = c.nullable;
        col["primary_key"] = c.primary_key;
        data["columns"].push_back(col);
    }

    write_wal("create_table", qualified, data);

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

    write_wal("drop_table", qualified, {});

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

    json row_json;
    for (const auto& [k, v] : validated) {
        std::visit([&](auto&& val) {
            using T = std::decay_t<decltype(val)>;
            if constexpr (std::is_same_v<T, std::nullptr_t>) row_json[k] = nullptr;
            else if constexpr (std::is_same_v<T, int64_t>) row_json[k] = val;
            else if constexpr (std::is_same_v<T, double>) row_json[k] = val;
            else if constexpr (std::is_same_v<T, bool>) row_json[k] = val;
            else if constexpr (std::is_same_v<T, std::string>) row_json[k] = val;
        }, v);
    }

    json data;
    data["row"] = row_json;
    data["rowid"] = id;
    write_wal("insert", qualified, data);

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

        json upd_json;
        for (const auto& [k, v] : updates) {
            std::visit([&](auto&& val) {
                using T = std::decay_t<decltype(val)>;
                if constexpr (std::is_same_v<T, std::nullptr_t>) upd_json[k] = nullptr;
                else if constexpr (std::is_same_v<T, int64_t>) upd_json[k] = val;
                else if constexpr (std::is_same_v<T, double>) upd_json[k] = val;
                else if constexpr (std::is_same_v<T, bool>) upd_json[k] = val;
                else if constexpr (std::is_same_v<T, std::string>) upd_json[k] = val;
            }, v);
        }

        int64_t rowid = std::get<int64_t>(row.at("_rowid"));
        json data;
        data["rowid"] = rowid;
        data["updates"] = upd_json;
        write_wal("update", qualified, data);

        // 写新版本: 系统列保持 0/0/0 (非事务), 业务列用 updates 覆盖.
        // 擦除旧版本再 put, 与原 in-place 行为等价.
        for (const auto& [k, v] : updates) {
            row[k] = v;
        }
        row["_xmax"] = static_cast<int64_t>(0);
        row["_cid"]  = static_cast<int64_t>(0);
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
        json data;
        data["rowid"] = rowid;
        write_wal("delete", qualified, data);
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

    // WAL
    json row_json;
    for (const auto& [k, v] : validated) {
        std::visit([&](auto&& val) {
            using T = std::decay_t<decltype(val)>;
            if constexpr (std::is_same_v<T, std::nullptr_t>) row_json[k] = nullptr;
            else if constexpr (std::is_same_v<T, int64_t>) row_json[k] = val;
            else if constexpr (std::is_same_v<T, double>) row_json[k] = val;
            else if constexpr (std::is_same_v<T, bool>) row_json[k] = val;
            else if constexpr (std::is_same_v<T, std::string>) row_json[k] = val;
        }, v);
    }
    json data;
    data["row"] = row_json;
    data["rowid"] = id;
    write_wal("insert", qualified, data, xid);

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

            // 3. WAL
            json upd_json;
            for (const auto& [k, v] : updates) {
                std::visit([&](auto&& val) {
                    using T = std::decay_t<decltype(val)>;
                    if constexpr (std::is_same_v<T, std::nullptr_t>) upd_json[k] = nullptr;
                    else if constexpr (std::is_same_v<T, int64_t>) upd_json[k] = val;
                    else if constexpr (std::is_same_v<T, double>) upd_json[k] = val;
                    else if constexpr (std::is_same_v<T, bool>) upd_json[k] = val;
                    else if constexpr (std::is_same_v<T, std::string>) upd_json[k] = val;
                }, v);
            }

            // 序列化新行
            json new_row_json;
            for (const auto& [k, v] : new_row) {
                std::visit([&](auto&& val) {
                    using T = std::decay_t<decltype(val)>;
                    if constexpr (std::is_same_v<T, std::nullptr_t>) new_row_json[k] = nullptr;
                    else if constexpr (std::is_same_v<T, int64_t>) new_row_json[k] = val;
                    else if constexpr (std::is_same_v<T, double>) new_row_json[k] = val;
                    else if constexpr (std::is_same_v<T, bool>) new_row_json[k] = val;
                    else if constexpr (std::is_same_v<T, std::string>) new_row_json[k] = val;
                }, v);
            }

            json data;
            data["rowid"] = rowid;
            data["updates"] = upd_json;
            data["new_row"] = new_row_json;
            write_wal("update", qualified, data, xid);

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

            // WAL
            json data;
            data["rowid"] = rowid;
            data["xmax"]  = static_cast<int64_t>(xid);
            write_wal("delete", qualified, data, xid);

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
    json data;
    data["_xid"] = static_cast<int64_t>(xid);
    write_wal("xact_commit", "", data, xid);
}

void StorageEngine::write_xact_abort(jiamiao::TransactionId xid) {
    auto h = mgr_.acquire(next_call_xid(),
                          {LockTargetType::Table, kWalTarget}, LockMode::Exclusive);
    json data;
    data["_xid"] = static_cast<int64_t>(xid);
    write_wal("xact_abort", "", data, xid);
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

void StorageEngine::replay_record(const WALRecord& rec) {
    // Catalog 操作
    if (rec.op == "create_database") {
        std::string db_name = rec.data["name"].get_string();
        if (!catalog_->database_exists(db_name)) {
            catalog_->create_database(db_name);
        }
        return;
    }
    if (rec.op == "drop_database") {
        std::string db_name = rec.data["name"].get_string();
        if (catalog_->database_exists(db_name)) {
            catalog_->drop_database(db_name);
        }
        return;
    }
    if (rec.op == "create_schema") {
        std::string db_name = rec.data["database"].get_string();
        std::string schema_name = rec.data["schema"].get_string();
        if (!catalog_->schema_exists(db_name, schema_name)) {
            catalog_->create_schema(db_name, schema_name);
        }
        return;
    }
    if (rec.op == "create_user") {
        std::string user_name = rec.data["name"].get_string();
        if (!catalog_->user_exists(user_name)) {
            // 重放时不恢复密码 (密码已通过 checkpoint 恢复)
        }
        return;
    }
    if (rec.op == "drop_user") {
        std::string user_name = rec.data["name"].get_string();
        if (catalog_->user_exists(user_name)) {
            catalog_->drop_user(user_name);
        }
        return;
    }

    if (rec.op == "create_table") {
        TableSchema schema;
        schema.name = rec.table;
        schema.row_count = 0;
        for (const auto& c : rec.data["columns"]) {
            ColumnDef col;
            col.name = c["name"].get_string();
            col.type = data_type_from_name(c.value("type", std::string("TEXT")));
            col.nullable = c.value("nullable", true);
            col.primary_key = c.value("primary_key", false);
            schema.columns.push_back(col);
        }
        tables_[rec.table] = std::move(schema);
        data_[rec.table] = std::make_unique<MemTable>(rec.table);
        row_ids_[rec.table] = 0;
        indexes_[rec.table] = {};

        for (const auto& c : tables_[rec.table].columns) {
            if (c.primary_key) create_index(rec.table, c.name);
        }
        return;
    }

    if (rec.op == "drop_table") {
        tables_.erase(rec.table);
        data_.erase(rec.table);
        row_ids_.erase(rec.table);
        indexes_.erase(rec.table);
        return;
    }

    if (rec.op == "xact_commit") {
        if (rec.data.contains("_xid")) {
            jiamiao::TransactionId xid = static_cast<jiamiao::TransactionId>(
                rec.data["_xid"].get_int());
            txn_mgr_->clog().set_status(xid, jiamiao::TransactionStatus::COMMITTED);
        }
        return;
    }

    if (rec.op == "xact_abort") {
        if (rec.data.contains("_xid")) {
            jiamiao::TransactionId xid = static_cast<jiamiao::TransactionId>(
                rec.data["_xid"].get_int());
            txn_mgr_->clog().set_status(xid, jiamiao::TransactionStatus::ABORTED);
        }
        return;
    }

    if (rec.op == "insert") {
        // 事务过滤: 跳过未提交或已回滚的记录
        if (rec.data.contains("_xid")) {
            jiamiao::TransactionId xid = static_cast<jiamiao::TransactionId>(
                rec.data["_xid"].get_int());
            auto status = txn_mgr_->clog().get_status(xid);
            if (status != jiamiao::TransactionStatus::COMMITTED) return;
        }

        auto mt_it = data_.find(rec.table);
        if (mt_it == data_.end()) {
            // 表还没创建 (checkpoint 还没载完, create_table 还没 replay) — 跳过
            return;
        }
        auto& mt = mt_it->second;

        Row row;
        for (auto it = rec.data["row"].obj_begin(); it != rec.data["row"].obj_end(); ++it) {
            const auto& v = it->second;
            if (v.is_null()) row[it->first] = nullptr;
            else if (v.is_int()) row[it->first] = v.get_int();
            else if (v.is_float()) row[it->first] = v.get_float();
            else if (v.is_bool()) row[it->first] = v.get_bool();
            else row[it->first] = v.get_string();
        }
        int64_t rid = 0;
        if (rec.data.contains("rowid")) {
            rid = rec.data["rowid"].get_int();
        } else {
            auto rit = row.find("_rowid");
            if (rit != row.end()) {
                if (auto* v = std::get_if<int64_t>(&rit->second)) rid = *v;
            }
        }

        // Phase 1: insert → put. 系统列从 row 中取 (xmin/xmax/cid)
        TupleHeader hdr = header_from_row(rec.table, rid, row);
        auto tup = BinaryRowCodec::row_to_tuple(row, hdr);
        mt->put(rid, tup, ++wal_seq_);
        update_indexes(rec.table, rid, row);

        auto schema_it = tables_.find(rec.table);
        if (schema_it != tables_.end()) schema_it->second.row_count = static_cast<int64_t>(mt->size());

        if (rec.data.contains("rowid")) {
            row_ids_[rec.table] = std::max(row_ids_[rec.table], rec.data["rowid"].get_int());
        }
        return;
    }

    if (rec.op == "update") {
        // 事务过滤
        if (rec.data.contains("_xid")) {
            jiamiao::TransactionId xid = static_cast<jiamiao::TransactionId>(
                rec.data["_xid"].get_int());
            auto status = txn_mgr_->clog().get_status(xid);
            if (status != jiamiao::TransactionStatus::COMMITTED) return;
        }

        auto rows_it = data_.find(rec.table);
        if (rows_it == data_.end()) return;
        auto& mt = rows_it->second;

        int64_t rowid = rec.data["rowid"].get_int();
        jiamiao::TransactionId xid = rec.data.contains("_xid")
            ? static_cast<jiamiao::TransactionId>(rec.data["_xid"].get_int())
            : jiamiao::InvalidTransactionId;

        // 从 WAL 恢复新行版本
        if (rec.data.contains("new_row")) {
            Row new_row;
            for (auto it = rec.data["new_row"].obj_begin();
                 it != rec.data["new_row"].obj_end(); ++it) {
                const auto& v = it->second;
                if (v.is_null()) new_row[it->first] = nullptr;
                else if (v.is_int()) new_row[it->first] = v.get_int();
                else if (v.is_float()) new_row[it->first] = v.get_float();
                else if (v.is_bool()) new_row[it->first] = v.get_bool();
                else new_row[it->first] = v.get_string();
            }
            // Phase 1: put 新版本. 旧版本保留 (_xmax=0, 不可见需靠 scan_with_snapshot 过滤)
            (void)xid;  // Phase 1: 旧版本标记 _xmax 需要新写一个版本, 暂略 (与 update_with_txn 同样的限制)
            TupleHeader hdr = header_from_row(rec.table, rowid, new_row);
            auto tup = BinaryRowCodec::row_to_tuple(new_row, hdr);
            mt->put(rowid, tup, ++wal_seq_);
        } else {
            // 兼容旧格式 (无 new_row): 原地更新
            std::map<std::string, Value> updates;
            for (auto it = rec.data["updates"].obj_begin();
                 it != rec.data["updates"].obj_end(); ++it) {
                const auto& v = it->second;
                if (v.is_null()) updates[it->first] = nullptr;
                else if (v.is_int()) updates[it->first] = v.get_int();
                else if (v.is_float()) updates[it->first] = v.get_float();
                else if (v.is_bool()) updates[it->first] = v.get_bool();
                else updates[it->first] = v.get_string();
            }
            // 取最新版本, 应用 updates, put
            auto opt = mt->get_latest(rowid);
            if (opt) {
                Row row = tuple_to_row_for_visibility(*opt);
                for (const auto& [k, v] : updates) row[k] = v;
                TupleHeader hdr = header_from_row(rec.table, rowid, row);
                auto tup = BinaryRowCodec::row_to_tuple(row, hdr);
                mt->put(rowid, tup, ++wal_seq_);
            }
            if (updates_affect_index(rec.table, updates)) rebuild_indexes(rec.table);
        }

        rebuild_indexes(rec.table);
        auto schema_it = tables_.find(rec.table);
        if (schema_it != tables_.end()) schema_it->second.row_count = static_cast<int64_t>(mt->size());
        return;
    }

    if (rec.op == "delete") {
        // 事务过滤
        if (rec.data.contains("_xid")) {
            jiamiao::TransactionId xid = static_cast<jiamiao::TransactionId>(
                rec.data["_xid"].get_int());
            auto status = txn_mgr_->clog().get_status(xid);
            if (status != jiamiao::TransactionStatus::COMMITTED) return;
        }

        auto rows_it = data_.find(rec.table);
        if (rows_it == data_.end()) return;
        auto& mt = rows_it->second;

        int64_t rowid = rec.data["rowid"].get_int();
        jiamiao::TransactionId xmax_val = rec.data.contains("xmax")
            ? static_cast<jiamiao::TransactionId>(rec.data["xmax"].get_int())
            : jiamiao::InvalidTransactionId;

        if (xmax_val != jiamiao::InvalidTransactionId) {
            // MVCC 格式: 写 tombstone 版本 (xmax=xmax_val)
            auto opt = mt->get_latest(rowid);
            if (opt) {
                Row row = tuple_to_row_for_visibility(*opt);
                row["_xmax"] = static_cast<int64_t>(xmax_val);
                TupleHeader hdr = header_from_row(rec.table, rowid, row);
                auto tup = BinaryRowCodec::row_to_tuple(row, hdr);
                mt->put(rowid, tup, ++wal_seq_);
            }
        } else {
            // 兼容旧格式: 物理删除
            mt->erase_all_for(rowid);
            rebuild_indexes(rec.table);
            auto schema_it = tables_.find(rec.table);
            if (schema_it != tables_.end()) schema_it->second.row_count = static_cast<int64_t>(mt->size());
        }
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

        json rows_json = json::array();
        auto data_it = data_.find(name);
        if (data_it != data_.end()) {
            auto all = data_it->second->scan_all();
            for (const auto& tup : all) {
                Row row = tuple_to_row_for_visibility(tup);
                json r;
                for (const auto& [k, v] : row) {
                    std::visit([&](auto&& val) {
                        using T = std::decay_t<decltype(val)>;
                        if constexpr (std::is_same_v<T, std::nullptr_t>) r[k] = nullptr;
                        else if constexpr (std::is_same_v<T, int64_t>) r[k] = val;
                        else if constexpr (std::is_same_v<T, double>) r[k] = val;
                        else if constexpr (std::is_same_v<T, bool>) r[k] = val;
                        else if constexpr (std::is_same_v<T, std::string>) r[k] = val;
                    }, v);
                }
                rows_json.push_back(r);
            }
        }
        ckp.table_data[name] = rows_json;
        ckp.row_id_counters[name] = row_ids_[name];

        // 序列化索引
        json idx_arr = json::array();
        auto idx_it = indexes_.find(name);
        if (idx_it != indexes_.end()) {
            for (const auto& idx : idx_it->second) {
                json ij;
                ij["column"] = idx.column;
                json entries;
                for (const auto& [k, indices] : idx.entries) {
                    json arr = json::array();
                    for (int64_t v : indices) arr.push_back(v);
                    entries[k] = arr;
                }
                ij["entries"] = entries;
                idx_arr.push_back(ij);
            }
        }
        ckp.indexes[name] = idx_arr;
    }

    // 序列化事务状态: next_xid + CLog (含 SSI 状态)
    ckp.next_xid = txn_mgr_->get_next_xid();
    ckp.clog_entries = txn_mgr_->clog().to_json();

    // 序列化 Catalog
    ckp.catalog_data = catalog_->to_json();

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

void StorageEngine::write_wal(const std::string& op, const std::string& table,
                               const json& data, jiamiao::TransactionId xid) {
    int64_t seq = next_seq();
    json enriched = data;
    if (xid != jiamiao::InvalidTransactionId) {
        enriched["_xid"] = static_cast<int64_t>(xid);
    }
    jiamiao::WALRecordV3 rec;
    rec.seq       = seq;
    rec.timestamp = time(nullptr);
    rec.op        = jiamiao::op_to_enum(op);
    rec.table     = table;
    rec.xid       = (xid != jiamiao::InvalidTransactionId) ? static_cast<uint32_t>(xid) : 0u;
    rec.data      = enriched.dump();
    wal_->append(rec);
}
