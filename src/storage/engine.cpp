#include "engine.h"
#include "transaction.h"
#include "lock_manager.h"
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

StorageEngine::StorageEngine(const std::string& data_dir, int checkpoint_interval)
    : data_dir_(data_dir), checkpoint_interval_(checkpoint_interval), mgr_(30000) {
    if (!std::filesystem::exists(data_dir)) {
        std::filesystem::create_directories(data_dir);
    }
    wal_ = std::make_unique<WriteAheadLog>(data_dir + "/wal.log");
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
        data_[qualified_name] = {};
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
                data_[qualified_name].push_back(std::move(row));
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

    // 3. 重放 WAL
    wal_->open();
    auto records = wal_->replay(ckp.last_seq);
    for (const auto& rec : records) {
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
    data_[qualified] = {};
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

    data_[qualified].push_back(validated);
    update_indexes(qualified, data_[qualified].size() - 1, validated);
    schema_it->second.row_count = data_[qualified].size();
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
    return it->second;
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
    for (const auto& row : it->second) {
        if (jiamiao::check_tuple_visibility(row, xid, snap, cid, txn_mgr_->clog())) {
            visible.push_back(row);
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

    for (auto& row : rows_it->second) {
        if (match(row)) {
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

            json data;
            data["rowid"] = std::get<int64_t>(row.at("_rowid"));
            data["updates"] = upd_json;
            write_wal("update", qualified, data);

            for (const auto& [k, v] : updates) {
                row[k] = v;
            }
            count++;
        }
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

    auto& rows = rows_it->second;
    std::vector<int64_t> to_delete;

    for (size_t i = 0; i < rows.size(); i++) {
        if (match(rows[i])) {
            json data;
            data["rowid"] = std::get<int64_t>(rows[i].at("_rowid"));
            write_wal("delete", qualified, data);
            to_delete.push_back(i);
        }
    }

    // 从后往前删除
    RowSet kept;
    for (size_t i = 0; i < rows.size(); i++) {
        if (std::find(to_delete.begin(), to_delete.end(), (int64_t)i) == to_delete.end()) {
            kept.push_back(std::move(rows[i]));
        }
    }

    data_[qualified] = std::move(kept);
    rebuild_indexes(qualified);

    auto schema_it = tables_.find(qualified);
    if (schema_it != tables_.end()) {
        schema_it->second.row_count = data_[qualified].size();
    }

    maybe_checkpoint();
    return to_delete.size();
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

    // PRIMARY KEY 唯一性检查
    {
        auto snap = txn_mgr_->get_snapshot();
        auto cid = txn_mgr_->get_current_command_id();
        for (const auto& col : schema_it->second.columns) {
            if (!col.primary_key) continue;
            auto pk_it = validated.find(col.name);
            if (pk_it == validated.end()) continue;
            for (const auto& existing : data_[qualified]) {
                if (!jiamiao::check_tuple_visibility(existing, xid, snap, cid, txn_mgr_->clog()))
                    continue;
                auto ev = existing.find(col.name);
                if (ev != existing.end() && ev->second == pk_it->second) {
                    throw std::runtime_error("重复键值违反唯一约束 \"" + col.name + "\"");
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

    data_[qualified].push_back(validated);
    update_indexes(qualified, data_[qualified].size() - 1, validated);
    schema_it->second.row_count = data_[qualified].size();
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

    for (auto& row : rows_it->second) {
        // 可见性检查: 只更新可见的行
        if (!jiamiao::check_tuple_visibility(row, xid, snap, cid, txn_mgr_->clog()))
            continue;

        if (match(row)) {
            // 保存修改前的行 (用于 Undo, 此时 _xmax 还是旧值)
            Row old_row = row;
            int64_t rowid = std::get<int64_t>(row.at("_rowid"));

            // 1. 标记旧版本的 _xmax
            row["_xmax"] = static_cast<int64_t>(xid);
            row["_cid"]  = static_cast<int64_t>(cid);

            // 2. 创建新行版本 (包含 _xmax 标记后的行数据作为基础)
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
                        for (const auto& existing : rows_it->second) {
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

            // 3. 注册 Undo
            txn_mgr_->add_undo_record(jiamiao::UndoRecord(
                jiamiao::UndoOp::UPDATE, qualified, rowid, old_row, new_row));

            // 4. WAL
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

            new_versions.push_back(std::move(new_row));
            count++;
        }
    }

    // 追加所有新版本
    for (auto& r : new_versions) {
        data_[qualified].push_back(std::move(r));
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
    auto& rows = rows_it->second;
    int64_t count = 0;

    for (size_t i = 0; i < rows.size(); i++) {
        // 可见性检查: 只删除可见的行
        if (!jiamiao::check_tuple_visibility(rows[i], xid, snap, cid, txn_mgr_->clog()))
            continue;

        if (match(rows[i])) {
            int64_t rowid = std::get<int64_t>(rows[i].at("_rowid"));

            // 保存修改前的行 (用于 Undo)
            Row old_row = rows[i];

            // 标记 _xmax (MVCC 删除)
            rows[i]["_xmax"] = static_cast<int64_t>(xid);
            rows[i]["_cid"]  = static_cast<int64_t>(cid);

            // 注册 Undo (old_row 保留了修改前的 _xmax / _cid)
            txn_mgr_->add_undo_record(jiamiao::UndoRecord(
                jiamiao::UndoOp::DELETE, qualified, rowid, old_row, {}));

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

void StorageEngine::apply_undo() {
    const auto& records = txn_mgr_->undo_records();
    jiamiao::TransactionId our_xid = txn_mgr_->get_current_xid();

    // 反向遍历 undo records (后进先出)
    for (auto it = records.rbegin(); it != records.rend(); ++it) {
        const auto& rec = *it;
        auto data_it = data_.find(rec.table_name);
        if (data_it == data_.end()) continue;
        auto& rows = data_it->second;

        switch (rec.op) {
        case jiamiao::UndoOp::INSERT:
            // 回滚 INSERT = 删除由我们插入的行 (匹配 _rowid + _xmin)
            for (size_t i = 0; i < rows.size(); i++) {
                auto rit = rows[i].find("_rowid");
                auto xit = rows[i].find("_xmin");
                if (rit != rows[i].end() && xit != rows[i].end() &&
                    std::get<int64_t>(rit->second) == rec.row_id &&
                    std::get<int64_t>(xit->second) == static_cast<int64_t>(our_xid)) {
                    rows.erase(rows.begin() + i);
                    break;
                }
            }
            break;

        case jiamiao::UndoOp::UPDATE:
            // 回滚 UPDATE:
            // 1. 删除我们创建的新行版本 (匹配 _rowid + _xmin == our_xid)
            for (size_t i = 0; i < rows.size(); i++) {
                auto rit = rows[i].find("_rowid");
                auto xit = rows[i].find("_xmin");
                if (rit != rows[i].end() && xit != rows[i].end() &&
                    std::get<int64_t>(rit->second) == rec.row_id &&
                    std::get<int64_t>(xit->second) == static_cast<int64_t>(our_xid)) {
                    rows.erase(rows.begin() + i);
                    break;
                }
            }
            // 2. 恢复旧版本的 _xmax 和 _cid (匹配 _rowid + _xmax == our_xid)
            for (size_t i = 0; i < rows.size(); i++) {
                auto rit = rows[i].find("_rowid");
                auto xit = rows[i].find("_xmax");
                if (rit != rows[i].end() && xit != rows[i].end() &&
                    std::get<int64_t>(rit->second) == rec.row_id &&
                    std::get<int64_t>(xit->second) == static_cast<int64_t>(our_xid)) {
                    // 从 old_row 恢复
                    auto ox = rec.old_row.find("_xmax");
                    if (ox != rec.old_row.end()) rows[i]["_xmax"] = ox->second;
                    auto oc = rec.old_row.find("_cid");
                    if (oc != rec.old_row.end()) rows[i]["_cid"] = oc->second;
                    break;
                }
            }
            break;

        case jiamiao::UndoOp::DELETE:
            // 回滚 DELETE = 清除 _xmax (匹配 _rowid + _xmax == our_xid)
            for (size_t i = 0; i < rows.size(); i++) {
                auto rit = rows[i].find("_rowid");
                auto xit = rows[i].find("_xmax");
                if (rit != rows[i].end() && xit != rows[i].end() &&
                    std::get<int64_t>(rit->second) == rec.row_id &&
                    std::get<int64_t>(xit->second) == static_cast<int64_t>(our_xid)) {
                    // 从 old_row 恢复 _xmax 和 _cid
                    auto ox = rec.old_row.find("_xmax");
                    if (ox != rec.old_row.end()) rows[i]["_xmax"] = ox->second;
                    auto oc = rec.old_row.find("_cid");
                    if (oc != rec.old_row.end()) rows[i]["_cid"] = oc->second;
                    break;
                }
            }
            break;
        }
    }

    // 重建受影响的索引
    for (auto it = records.rbegin(); it != records.rend(); ++it) {
        rebuild_indexes(it->table_name);
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

            RowSet result;
            auto& rows = data_[qualified];
            for (int64_t i : entry_it->second) {
                if (i >= 0 && i < (int64_t)rows.size()) {
                    result.push_back(rows[i]);
                }
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

            RowSet result;
            auto& rows = data_[qualified];
            for (int64_t i : entry_it->second) {
                if (i >= 0 && i < (int64_t)rows.size()) {
                    if (jiamiao::check_tuple_visibility(rows[i], xid, snap, cid, txn_mgr_->clog())) {
                        result.push_back(rows[i]);
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
    auto idx_it = indexes_.find(table);
    if (idx_it == indexes_.end()) return;

    for (const auto& idx : idx_it->second) {
        if (idx.column == column) return; // 已存在
    }

    IndexInfo idx;
    idx.column = column;
    auto& rows = data_[table];
    for (size_t i = 0; i < rows.size(); i++) {
        auto it = rows[i].find(column);
        if (it != rows[i].end()) {
            idx.entries[value_to_string(it->second)].push_back(i);
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
        data_[rec.table] = {};
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

        auto& rows = data_[rec.table];
        if (rows.empty() && !tables_.count(rec.table)) return;

        Row row;
        for (auto it = rec.data["row"].obj_begin(); it != rec.data["row"].obj_end(); ++it) {
            const auto& v = it->second;
            if (v.is_null()) row[it->first] = nullptr;
            else if (v.is_int()) row[it->first] = v.get_int();
            else if (v.is_float()) row[it->first] = v.get_float();
            else if (v.is_bool()) row[it->first] = v.get_bool();
            else row[it->first] = v.get_string();
        }

        rows.push_back(std::move(row));
        update_indexes(rec.table, rows.size() - 1, rows.back());

        auto schema_it = tables_.find(rec.table);
        if (schema_it != tables_.end()) schema_it->second.row_count = rows.size();

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

        int64_t rowid = rec.data["rowid"].get_int();
        jiamiao::TransactionId xid = rec.data.contains("_xid")
            ? static_cast<jiamiao::TransactionId>(rec.data["_xid"].get_int())
            : jiamiao::InvalidTransactionId;

        // MVCC: 找到当前可见版本 (_rowid 匹配且 _xmax == 0), 标记 _xmax
        for (auto& row : rows_it->second) {
            auto rit = row.find("_rowid");
            auto xmax_it = row.find("_xmax");
            if (rit != row.end() && std::get<int64_t>(rit->second) == rowid) {
                int64_t cur_xmax = 0;
                if (xmax_it != row.end()) cur_xmax = std::get<int64_t>(xmax_it->second);
                if (cur_xmax == 0) {
                    row["_xmax"] = static_cast<int64_t>(xid);
                    break;
                }
            }
        }

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
            rows_it->second.push_back(std::move(new_row));
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
            bool index_affected = updates_affect_index(rec.table, updates);
            for (auto& row : rows_it->second) {
                auto it = row.find("_rowid");
                if (it != row.end() && std::get<int64_t>(it->second) == rowid) {
                    for (const auto& [k, v] : updates) row[k] = v;
                    break;
                }
            }
            if (index_affected) rebuild_indexes(rec.table);
        }

        rebuild_indexes(rec.table);
        auto schema_it = tables_.find(rec.table);
        if (schema_it != tables_.end()) schema_it->second.row_count = rows_it->second.size();
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

        int64_t rowid = rec.data["rowid"].get_int();
        jiamiao::TransactionId xmax_val = rec.data.contains("xmax")
            ? static_cast<jiamiao::TransactionId>(rec.data["xmax"].get_int())
            : jiamiao::InvalidTransactionId;

        auto& rows = rows_it->second;
        if (xmax_val != jiamiao::InvalidTransactionId) {
            // MVCC 格式: 标记 _xmax
            for (size_t i = 0; i < rows.size(); i++) {
                auto it = rows[i].find("_rowid");
                auto xmax_it = rows[i].find("_xmax");
                if (it != rows[i].end() && std::get<int64_t>(it->second) == rowid) {
                    int64_t cur_xmax = 0;
                    if (xmax_it != rows[i].end()) cur_xmax = std::get<int64_t>(xmax_it->second);
                    if (cur_xmax == 0) {
                        rows[i]["_xmax"] = static_cast<int64_t>(xmax_val);
                    }
                    break;
                }
            }
        } else {
            // 兼容旧格式: 物理删除
            for (size_t i = 0; i < rows.size(); i++) {
                auto it = rows[i].find("_rowid");
                if (it != rows[i].end() && std::get<int64_t>(it->second) == rowid) {
                    rows.erase(rows.begin() + i);
                    rebuild_indexes(rec.table);
                    auto schema_it = tables_.find(rec.table);
                    if (schema_it != tables_.end()) schema_it->second.row_count = rows.size();
                    break;
                }
            }
        }
        return;
    }
}

void StorageEngine::checkpoint() {
    Checkpoint ckp;
    ckp.last_seq = wal_seq_;
    ckp.timestamp = time(nullptr);

    for (const auto& [name, schema] : tables_) {
        ckp.tables.push_back(schema);

        json rows_json = json::array();
        auto data_it = data_.find(name);
        if (data_it != data_.end()) {
            for (const auto& row : data_it->second) {
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

    // 序列化事务状态
    ckp.next_xid = txn_mgr_->get_next_xid();
    ckp.clog_entries = txn_mgr_->clog().to_json();

    // 序列化 Catalog
    ckp.catalog_data = catalog_->to_json();

    wal_->sync();
    ckp_mgr_->save(ckp);
}

void StorageEngine::maybe_checkpoint() {
    if (checkpoint_interval_ > 0 && wal_seq_ % checkpoint_interval_ == 0) {
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

    auto& rows = data_[table];
    for (auto& idx : idx_it->second) {
        idx.entries.clear();
        for (size_t i = 0; i < rows.size(); i++) {
            auto cell = rows[i].find(idx.column);
            if (cell != rows[i].end()) {
                idx.entries[value_to_string(cell->second)].push_back(i);
            }
        }
    }
}

void StorageEngine::update_indexes(const std::string& table, int64_t row_index, const Row& row) {
    auto idx_it = indexes_.find(table);
    if (idx_it == indexes_.end()) return;

    for (auto& idx : idx_it->second) {
        auto cell = row.find(idx.column);
        if (cell != row.end()) {
            idx.entries[value_to_string(cell->second)].push_back(row_index);
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
    WALRecord rec{seq, time(nullptr), op, table, enriched};
    wal_->append(rec);
}
