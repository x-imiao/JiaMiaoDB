#include "checkpoint.h"
#include <fstream>
#include <filesystem>
#include <iostream>

CheckpointManager::CheckpointManager(const std::string& dir) : dir_(dir) {
    if (!std::filesystem::exists(dir)) {
        std::filesystem::create_directories(dir);
    }
}

json Checkpoint::to_json() const {
    json j;
    j["version"] = version;
    j["last_seq"] = last_seq;
    j["timestamp"] = timestamp;

    json tables_arr = json::array();
    for (const auto& t : tables) {
        json cols = json::array();
        for (const auto& c : t.columns) {
            json col;
            col["name"] = c.name;
            col["type"] = data_type_name(c.type);
            col["nullable"] = c.nullable;
            col["primary_key"] = c.primary_key;
            cols.push_back(col);
        }
        json tbl;
        tbl["name"] = t.name;
        tbl["columns"] = cols;
        tbl["row_count"] = t.row_count;
        tables_arr.push_back(tbl);
    }
    j["tables"] = tables_arr;
    j["data"] = table_data;

    json counters = json::object();
    for (const auto& [k, v] : row_id_counters) counters[k] = v;
    j["row_id_counters"] = counters;

    j["indexes"] = indexes;
    return j;
}

Checkpoint Checkpoint::from_json(const json& j) {
    Checkpoint ckp;
    ckp.version = j.value("version", (int64_t)1);
    ckp.last_seq = j.value("last_seq", (int64_t)0);
    ckp.timestamp = j.value("timestamp", (int64_t)0);

    for (const auto& t : j["tables"]) {
        TableSchema schema;
        schema.name = t["name"].get_string();
        schema.row_count = t.value("row_count", (int64_t)0);
        for (const auto& c : t["columns"]) {
            ColumnDef col;
            col.name = c["name"].get_string();
            col.type = data_type_from_name(c.value("type", std::string("TEXT")));
            col.nullable = c.value("nullable", true);
            col.primary_key = c.value("primary_key", false);
            schema.columns.push_back(col);
        }
        ckp.tables.push_back(schema);
    }

    if (j.contains("data")) {
        for (auto it = j["data"].obj_begin(); it != j["data"].obj_end(); ++it) {
            ckp.table_data[it->first] = it->second;
        }
    }
    if (j.contains("row_id_counters")) {
        for (auto it = j["row_id_counters"].obj_begin(); it != j["row_id_counters"].obj_end(); ++it) {
            ckp.row_id_counters[it->first] = it->second.get_int();
        }
    }
    if (j.contains("indexes")) {
        for (auto it = j["indexes"].obj_begin(); it != j["indexes"].obj_end(); ++it) {
            ckp.indexes[it->first] = it->second;
        }
    }

    return ckp;
}

void CheckpointManager::save(const Checkpoint& ckp) {
    std::ofstream file(checkpoint_path());
    if (!file.is_open()) {
        std::cerr << "[Checkpoint] 无法写入: " << checkpoint_path() << "\n";
        return;
    }
    file << ckp.to_json().dump(2);
}

Checkpoint CheckpointManager::load() {
    if (!std::filesystem::exists(checkpoint_path())) return Checkpoint{};
    std::ifstream file(checkpoint_path());
    if (!file.is_open()) return Checkpoint{};
    try {
        std::stringstream ss;
        ss << file.rdbuf();
        json j = json::parse(ss.str());
        return Checkpoint::from_json(j);
    } catch (...) {
        return Checkpoint{};
    }
}

int64_t CheckpointManager::get_last_seq() {
    auto ckp = load();
    return ckp.last_seq;
}
