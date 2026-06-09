#include "engine.h"
#include "transaction.h"
#include <filesystem>
#include <iostream>
#include <algorithm>
#include <sstream>
#include "json.h"

using json = Json;

StorageEngine::StorageEngine(const std::string& data_dir, int checkpoint_interval)
    : data_dir_(data_dir), checkpoint_interval_(checkpoint_interval) {
    if (!std::filesystem::exists(data_dir)) {
        std::filesystem::create_directories(data_dir);
    }
    wal_ = std::make_unique<WriteAheadLog>(data_dir + "/wal.log");
    ckp_mgr_ = std::make_unique<CheckpointManager>(data_dir);
    txn_mgr_ = std::make_unique<jiamiao::TransactionManager>();
}

StorageEngine::~StorageEngine() { close(); }

void StorageEngine::load() {
    std::lock_guard<std::mutex> lock(global_mutex_);

    // 1. 加载 Checkpoint
    auto ckp = ckp_mgr_->load();
    wal_seq_ = ckp.last_seq;

    for (const auto& t : ckp.tables) {
        tables_[t.name] = t;
        data_[t.name] = {};
        row_ids_[t.name] = 0;
        indexes_[t.name] = {};

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
                data_[t.name].push_back(std::move(row));
            }
        }

        // 恢复 row_id_counter
        if (ckp.row_id_counters.find(t.name) != ckp.row_id_counters.end()) {
            row_ids_[t.name] = ckp.row_id_counters[t.name];
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
                indexes_[t.name].push_back(std::move(idx));
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
    std::lock_guard<std::mutex> lock(global_mutex_);
    checkpoint();
}

void StorageEngine::close() {
    checkpoint();
    wal_->close();
}

/* ─── Schema ─── */

void StorageEngine::create_table(const std::string& name, const std::vector<ColumnDef>& columns) {
    std::lock_guard<std::mutex> lock(global_mutex_);
    if (tables_.count(name)) {
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

    write_wal("create_table", name, data);

    TableSchema schema;
    schema.name = name;
    schema.columns = columns;
    schema.row_count = 0;
    tables_[name] = std::move(schema);
    data_[name] = {};
    row_ids_[name] = 0;
    indexes_[name] = {};

    // PK 自动建索引
    for (const auto& c : columns) {
        if (c.primary_key) {
            create_index(name, c.name);
        }
    }

    maybe_checkpoint();
}

void StorageEngine::drop_table(const std::string& name) {
    std::lock_guard<std::mutex> lock(global_mutex_);
    if (!tables_.count(name)) {
        throw std::runtime_error("表 \"" + name + "\" 不存在");
    }

    write_wal("drop_table", name, {});

    tables_.erase(name);
    data_.erase(name);
    row_ids_.erase(name);
    indexes_.erase(name);
    maybe_checkpoint();
}

TableSchema* StorageEngine::get_schema(const std::string& name) {
    auto it = tables_.find(name);
    if (it != tables_.end()) return &it->second;
    return nullptr;
}

std::vector<std::string> StorageEngine::list_tables() {
    std::vector<std::string> names;
    for (const auto& [name, _] : tables_) {
        names.push_back(name);
    }
    return names;
}

/* ─── 数据 ─── */

Row StorageEngine::insert(const std::string& table, const Row& row) {
    std::lock_guard<std::mutex> lock(global_mutex_);
    auto schema_it = tables_.find(table);
    if (schema_it == tables_.end()) {
        throw std::runtime_error("表 \"" + table + "\" 不存在");
    }

    auto validated = validate(schema_it->second, row);
    int64_t id = (row_ids_[table]) + 1;
    row_ids_[table] = id;
    validated["_rowid"] = id;

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
    write_wal("insert", table, data);

    data_[table].push_back(validated);
    update_indexes(table, data_[table].size() - 1, validated);
    schema_it->second.row_count = data_[table].size();
    maybe_checkpoint();

    return validated;
}

RowSet StorageEngine::scan(const std::string& table) {
    std::lock_guard<std::mutex> lock(global_mutex_);
    auto it = data_.find(table);
    if (it == data_.end()) {
        throw std::runtime_error("表 \"" + table + "\" 不存在");
    }
    return it->second;
}

int64_t StorageEngine::update(const std::string& table, std::function<bool(const Row&)> match,
                               const std::map<std::string, Value>& updates) {
    std::lock_guard<std::mutex> lock(global_mutex_);
    auto rows_it = data_.find(table);
    if (rows_it == data_.end()) {
        throw std::runtime_error("表 \"" + table + "\" 不存在");
    }

    bool index_affected = updates_affect_index(table, updates);
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
            write_wal("update", table, data);

            for (const auto& [k, v] : updates) {
                row[k] = v;
            }
            count++;
        }
    }

    if (index_affected) rebuild_indexes(table);
    maybe_checkpoint();
    return count;
}

int64_t StorageEngine::remove(const std::string& table, std::function<bool(const Row&)> match) {
    std::lock_guard<std::mutex> lock(global_mutex_);
    auto rows_it = data_.find(table);
    if (rows_it == data_.end()) {
        throw std::runtime_error("表 \"" + table + "\" 不存在");
    }

    auto& rows = rows_it->second;
    std::vector<int64_t> to_delete;

    for (size_t i = 0; i < rows.size(); i++) {
        if (match(rows[i])) {
            json data;
            data["rowid"] = std::get<int64_t>(rows[i].at("_rowid"));
            write_wal("delete", table, data);
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

    data_[table] = std::move(kept);
    rebuild_indexes(table);

    auto schema_it = tables_.find(table);
    if (schema_it != tables_.end()) {
        schema_it->second.row_count = data_[table].size();
    }

    maybe_checkpoint();
    return to_delete.size();
}

/* ─── 事务感知操作 ─── */

jiamiao::TransactionManager& StorageEngine::txn_mgr() {
    return *txn_mgr_;
}

Row StorageEngine::insert_with_txn(const std::string& table, const Row& row) {
    std::lock_guard<std::mutex> lock(global_mutex_);
    auto schema_it = tables_.find(table);
    if (schema_it == tables_.end()) {
        throw std::runtime_error("表 \"" + table + "\" 不存在");
    }

    auto validated = validate(schema_it->second, row);
    int64_t id = row_ids_[table] + 1;
    row_ids_[table] = id;
    validated["_rowid"] = id;

    // 分配 XID (首次写触发)
    jiamiao::TransactionId xid = txn_mgr_->assign_xid();
    validated["_xmin"] = static_cast<int64_t>(xid);

    // 注册 Undo 记录
    txn_mgr_->add_undo_record(jiamiao::UndoRecord(
        jiamiao::UndoOp::INSERT, table, id, {}, validated));

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
    write_wal("insert", table, data, xid);

    data_[table].push_back(validated);
    update_indexes(table, data_[table].size() - 1, validated);
    schema_it->second.row_count = data_[table].size();
    maybe_checkpoint();

    return validated;
}

int64_t StorageEngine::update_with_txn(const std::string& table,
                                        std::function<bool(const Row&)> match,
                                        const std::map<std::string, Value>& updates) {
    std::lock_guard<std::mutex> lock(global_mutex_);
    auto rows_it = data_.find(table);
    if (rows_it == data_.end()) {
        throw std::runtime_error("表 \"" + table + "\" 不存在");
    }

    jiamiao::TransactionId xid = txn_mgr_->assign_xid();
    bool index_affected = updates_affect_index(table, updates);
    int64_t count = 0;

    for (auto& row : rows_it->second) {
        if (match(row)) {
            // 保存旧行用于 Undo
            Row old_row = row;
            int64_t rowid = std::get<int64_t>(row.at("_rowid"));

            // 注册 Undo
            Row new_row = row;
            for (const auto& [k, v] : updates) {
                new_row[k] = v;
            }
            new_row["_xmin"] = static_cast<int64_t>(xid);
            txn_mgr_->add_undo_record(jiamiao::UndoRecord(
                jiamiao::UndoOp::UPDATE, table, rowid, old_row, new_row));

            // WAL
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
            data["rowid"] = rowid;
            data["updates"] = upd_json;
            write_wal("update", table, data, xid);

            // 应用更新
            for (const auto& [k, v] : updates) {
                row[k] = v;
            }
            row["_xmin"] = static_cast<int64_t>(xid);
            count++;
        }
    }

    if (index_affected) rebuild_indexes(table);
    maybe_checkpoint();
    return count;
}

int64_t StorageEngine::remove_with_txn(const std::string& table,
                                        std::function<bool(const Row&)> match) {
    std::lock_guard<std::mutex> lock(global_mutex_);
    auto rows_it = data_.find(table);
    if (rows_it == data_.end()) {
        throw std::runtime_error("表 \"" + table + "\" 不存在");
    }

    jiamiao::TransactionId xid = txn_mgr_->assign_xid();
    auto& rows = rows_it->second;
    std::vector<int64_t> to_delete;

    for (size_t i = 0; i < rows.size(); i++) {
        if (match(rows[i])) {
            int64_t rowid = std::get<int64_t>(rows[i].at("_rowid"));

            // 注册 Undo
            txn_mgr_->add_undo_record(jiamiao::UndoRecord(
                jiamiao::UndoOp::DELETE, table, rowid, rows[i], {}));

            // WAL
            json data;
            data["rowid"] = rowid;
            write_wal("delete", table, data, xid);

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

    data_[table] = std::move(kept);
    rebuild_indexes(table);

    auto schema_it = tables_.find(table);
    if (schema_it != tables_.end()) {
        schema_it->second.row_count = data_[table].size();
    }

    maybe_checkpoint();
    return to_delete.size();
}

void StorageEngine::apply_undo() {
    const auto& records = txn_mgr_->undo_records();

    // 反向遍历 undo records (后进先出)
    for (auto it = records.rbegin(); it != records.rend(); ++it) {
        const auto& rec = *it;
        auto data_it = data_.find(rec.table_name);
        if (data_it == data_.end()) continue;
        auto& rows = data_it->second;

        switch (rec.op) {
        case jiamiao::UndoOp::INSERT:
            // 回滚 INSERT = 删除插入的行
            for (size_t i = 0; i < rows.size(); i++) {
                auto rit = rows[i].find("_rowid");
                if (rit != rows[i].end() && std::get<int64_t>(rit->second) == rec.row_id) {
                    rows.erase(rows.begin() + i);
                    break;
                }
            }
            break;

        case jiamiao::UndoOp::UPDATE:
            // 回滚 UPDATE = 恢复旧行
            for (size_t i = 0; i < rows.size(); i++) {
                auto rit = rows[i].find("_rowid");
                if (rit != rows[i].end() && std::get<int64_t>(rit->second) == rec.row_id) {
                    rows[i] = rec.old_row;  // 恢复旧行（含原始 _xmin）
                    rows[i]["_rowid"] = rec.row_id;
                    break;
                }
            }
            break;

        case jiamiao::UndoOp::DELETE:
            // 回滚 DELETE = 重新插入旧行
            rows.push_back(rec.old_row);
            break;
        }
    }

    // 重建受影响的索引
    for (auto it = records.rbegin(); it != records.rend(); ++it) {
        rebuild_indexes(it->table_name);
    }
}

void StorageEngine::write_xact_commit(jiamiao::TransactionId xid) {
    std::lock_guard<std::mutex> lock(global_mutex_);
    json data;
    data["_xid"] = static_cast<int64_t>(xid);
    write_wal("xact_commit", "", data, xid);
}

void StorageEngine::write_xact_abort(jiamiao::TransactionId xid) {
    std::lock_guard<std::mutex> lock(global_mutex_);
    json data;
    data["_xid"] = static_cast<int64_t>(xid);
    write_wal("xact_abort", "", data, xid);
}

/* ─── 索引 ─── */

RowSet StorageEngine::index_lookup(const std::string& table, const std::string& column, const Value& value) {
    std::lock_guard<std::mutex> lock(global_mutex_);
    auto idx_it = indexes_.find(table);
    if (idx_it == indexes_.end()) return {};

    std::string key = value_to_string(value);
    for (const auto& idx : idx_it->second) {
        if (idx.column == column) {
            auto entry_it = idx.entries.find(key);
            if (entry_it == idx.entries.end()) return {};

            RowSet result;
            auto& rows = data_[table];
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

bool StorageEngine::has_index(const std::string& table, const std::string& column) {
    auto idx_it = indexes_.find(table);
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
        std::map<std::string, Value> updates;
        for (auto it = rec.data["updates"].obj_begin(); it != rec.data["updates"].obj_end(); ++it) {
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
                for (const auto& [k, v] : updates) {
                    row[k] = v;
                }
                break;
            }
        }

        if (index_affected) rebuild_indexes(rec.table);
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
        auto& rows = rows_it->second;
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
