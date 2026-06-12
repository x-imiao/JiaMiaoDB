#include "checkpoint.h"
#include "checkpoint_codec.h"
#include <fstream>
#include <filesystem>
#include <iostream>
#include <cstring>

CheckpointManager::CheckpointManager(const std::string& dir) : dir_(dir) {
    if (!std::filesystem::exists(dir)) {
        std::filesystem::create_directories(dir);
    }
}

/* ═══════════════════════════════════════════════════════════
   Row ↔ JSON 桥接 (O-2 起: table_rows / indexes_data 改为强类型,
   to_json() 仍需生成 JSON 供 legacy fallback 用)
   ═══════════════════════════════════════════════════════════ */
namespace {

json row_to_json(const Row& row) {
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
    return r;
}

Row json_to_row(const json& r) {
    Row row;
    for (auto it = r.obj_begin(); it != r.obj_end(); ++it) {
        const auto& v = it->second;
        if (v.is_null())        row[it->first] = nullptr;
        else if (v.is_int())    row[it->first] = v.get_int();
        else if (v.is_float())  row[it->first] = v.get_float();
        else if (v.is_bool())   row[it->first] = v.get_bool();
        else if (v.is_string()) row[it->first] = v.get_string();
        // 其他类型 (array/object) 跳过
    }
    return row;
}

}  // namespace

/* ═══════════════════════════════════════════════════════════
   JSON 形式 (legacy 兼容, 由 CheckpointManager::load fallback 调用)
   ═══════════════════════════════════════════════════════════ */

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

    // table_data: 强类型 → JSON 数组
    json data_obj = json::object();
    for (const auto& [name, rows] : table_rows) {
        json arr = json::array();
        for (const auto& [rowid, row] : rows) {
            json r = row_to_json(row);
            r["_rowid"] = static_cast<int64_t>(rowid);
            arr.push_back(r);
        }
        data_obj[name] = arr;
    }
    j["data"] = data_obj;

    json counters = json::object();
    for (const auto& [k, v] : row_id_counters) counters[k] = v;
    j["row_id_counters"] = counters;

    // indexes: 强类型 → JSON 数组
    json idx_obj = json::object();
    for (const auto& [name, idxs] : indexes_data) {
        json arr = json::array();
        for (const auto& idx : idxs) {
            json ij;
            ij["column"] = idx.column;
            json entries;
            for (const auto& [k, indices] : idx.entries) {
                json iarr = json::array();
                for (int64_t v : indices) iarr.push_back(v);
                entries[k] = iarr;
            }
            ij["entries"] = entries;
            arr.push_back(ij);
        }
        idx_obj[name] = arr;
    }
    j["indexes"] = idx_obj;

    j["next_xid"] = static_cast<int64_t>(next_xid);
    j["clog"] = clog_entries;
    j["catalog"] = catalog_data;
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
            const auto& rows_json = it->second;
            for (const auto& row_json : rows_json) {
                Row row = json_to_row(row_json);
                int64_t rid = 0;
                auto rid_it = row.find("_rowid");
                if (rid_it != row.end()) {
                    if (auto* v = std::get_if<int64_t>(&rid_it->second)) rid = *v;
                }
                ckp.table_rows[it->first].emplace_back(rid, std::move(row));
            }
        }
    }
    if (j.contains("row_id_counters")) {
        for (auto it = j["row_id_counters"].obj_begin(); it != j["row_id_counters"].obj_end(); ++it) {
            ckp.row_id_counters[it->first] = it->second.get_int();
        }
    }
    if (j.contains("indexes")) {
        for (auto it = j["indexes"].obj_begin(); it != j["indexes"].obj_end(); ++it) {
            const auto& idxs_json = it->second;
            for (const auto& idx_json : idxs_json) {
                IndexInfo idx;
                idx.column = idx_json["column"].get_string();
                for (auto eit = idx_json["entries"].obj_begin(); eit != idx_json["entries"].obj_end(); ++eit) {
                    std::vector<int64_t> indices;
                    for (const auto& v : eit->second) indices.push_back(v.get_int());
                    idx.entries[eit->first] = std::move(indices);
                }
                ckp.indexes_data[it->first].push_back(std::move(idx));
            }
        }
    }
    if (j.contains("next_xid")) {
        ckp.next_xid = static_cast<uint32_t>(j["next_xid"].get_int());
    }
    if (j.contains("clog")) {
        ckp.clog_entries = j["clog"];
    }
    if (j.contains("catalog")) {
        ckp.catalog_data = j["catalog"];
    }

    return ckp;
}

/* ═══════════════════════════════════════════════════════════
   二进制形式 — 委托给 checkpoint_codec (去 header 里直接做)
   ═══════════════════════════════════════════════════════════ */

std::string Checkpoint::to_binary() const {
    return jiamiao::checkpoint_encode(*this);
}

bool Checkpoint::from_binary(const std::string& bytes,
                             Checkpoint* ckp,
                             std::string* out_clog_json,
                             std::string* out_catalog_json) {
    return jiamiao::checkpoint_decode(bytes, ckp, out_clog_json, out_catalog_json);
}

/* ═══════════════════════════════════════════════════════════
   CheckpointManager
   ═══════════════════════════════════════════════════════════ */

void CheckpointManager::save(const Checkpoint& ckp) {
    // 写 [8B magic] + [binary payload]
    std::string binary = ckp.to_binary();
    std::ofstream file(checkpoint_path(), std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "[Checkpoint] 无法写入: " << checkpoint_path() << "\n";
        return;
    }
    file.write(jiamiao::kCheckpointMagic, jiamiao::kCheckpointMagicLen);
    file.write(binary.data(), static_cast<std::streamsize>(binary.size()));
    file.flush();
}

Checkpoint CheckpointManager::load() {
    using namespace jiamiao;
    if (!std::filesystem::exists(checkpoint_path())) {
        // 兼容老路径: checkpoint.json
        std::string legacy = dir_ + "/checkpoint.json";
        if (!std::filesystem::exists(legacy)) return Checkpoint{};
        std::ifstream lf(legacy);
        if (!lf.is_open()) return Checkpoint{};
        try {
            std::stringstream ss; ss << lf.rdbuf();
            return Checkpoint::from_json(json::parse(ss.str()));
        } catch (...) {
            return Checkpoint{};
        }
    }
    std::ifstream file(checkpoint_path(), std::ios::binary);
    if (!file.is_open()) return Checkpoint{};
    std::stringstream ss; ss << file.rdbuf();
    std::string all = ss.str();
    if (all.size() < kCheckpointMagicLen) return Checkpoint{};

    // 1) 检测 magic
    if (std::memcmp(all.data(), kCheckpointMagic, kCheckpointMagicLen) == 0) {
        Checkpoint ckp;
        std::string clog_json, catalog_json;
        if (!Checkpoint::from_binary(all.substr(kCheckpointMagicLen),
                                     &ckp, &clog_json, &catalog_json)) {
            std::cerr << "[Checkpoint] 二进制解析失败, 走空启动\n";
            return Checkpoint{};
        }
        // 嵌入的 JSON 字符串 → 重新 parse
        if (!clog_json.empty()) {
            try { ckp.clog_entries = json::parse(clog_json); }
            catch (...) { ckp.clog_entries = json(nullptr); }
        }
        if (!catalog_json.empty()) {
            try { ckp.catalog_data = json::parse(catalog_json); }
            catch (...) { ckp.catalog_data = json(nullptr); }
        }
        return ckp;
    }

    // 2) 没有 magic: 旧版 JSON 文件 (text)
    try {
        return Checkpoint::from_json(json::parse(all));
    } catch (...) {
        return Checkpoint{};
    }
}

int64_t CheckpointManager::get_last_seq() {
    auto ckp = load();
    return ckp.last_seq;
}
