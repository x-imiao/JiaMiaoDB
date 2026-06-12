/* ═══════════════════════════════════════════════════════════════════════
   checkpoint_codec.cpp — O-2 Checkpoint 二进制 codec 实现
   ═══════════════════════════════════════════════════════════════════════ */

#include "checkpoint_codec.h"
#include "wal_payload.h"
#include "clog_codec.h"

#include <cstring>

static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
              "Checkpoint binary format is little-endian; this platform is big-endian");

namespace jiamiao {

// ═══════════════════════════════════════════════════════════
// 小端字节序原语 (局部复制, 避免暴露头文件依赖)
// ═══════════════════════════════════════════════════════════
namespace {

inline void put_u8(std::string& b, uint8_t v) { b.push_back(static_cast<char>(v)); }
inline void put_u16(std::string& b, uint16_t v) {
    b.push_back(static_cast<char>(v & 0xFFU));
    b.push_back(static_cast<char>((v >> 8) & 0xFFU));
}
inline void put_u32(std::string& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<char>((v >> (i * 8)) & 0xFFU));
}
inline void put_i64(std::string& b, int64_t v) {
    for (int i = 0; i < 8; ++i) b.push_back(static_cast<char>((v >> (i * 8)) & 0xFFU));
}
inline void put_string_u16(std::string& b, const std::string& s) {
    put_u16(b, static_cast<uint16_t>(s.size()));
    b.append(s);
}
inline void put_string_u32(std::string& b, const std::string& s) {
    put_u32(b, static_cast<uint32_t>(s.size()));
    b.append(s);
}

// ColumnDef 编码
void encode_column(std::string& b, const ColumnDef& c) {
    put_string_u16(b, c.name);
    put_u8(b, static_cast<uint8_t>(c.type));
    put_u8(b, c.nullable ? 1u : 0u);
    put_u8(b, c.primary_key ? 1u : 0u);
}

bool decode_column(PayloadReader& r, ColumnDef* c) {
    if (!r.read_string_u16(&c->name)) return false;
    uint8_t tid, nullable, pk;
    if (!r.read_u8(&tid))      return false;
    if (!r.read_u8(&nullable)) return false;
    if (!r.read_u8(&pk))       return false;
    c->type        = static_cast<DataType>(tid);
    c->nullable    = (nullable != 0);
    c->primary_key = (pk != 0);
    return true;
}

// IndexInfo 编码
void encode_index(std::string& b, const IndexInfo& idx) {
    put_string_u16(b, idx.column);
    put_u32(b, static_cast<uint32_t>(idx.entries.size()));
    for (const auto& [k, indices] : idx.entries) {
        put_string_u16(b, k);
        put_u32(b, static_cast<uint32_t>(indices.size()));
        for (int64_t v : indices) put_i64(b, v);
    }
}

bool decode_index(PayloadReader& r, IndexInfo* idx) {
    if (!r.read_string_u16(&idx->column)) return false;
    uint32_t entry_count;
    if (!r.read_u32(&entry_count)) return false;
    for (uint32_t i = 0; i < entry_count; ++i) {
        std::string k;
        if (!r.read_string_u16(&k)) return false;
        uint32_t idx_count;
        if (!r.read_u32(&idx_count)) return false;
        std::vector<int64_t> indices;
        indices.reserve(idx_count);
        for (uint32_t j = 0; j < idx_count; ++j) {
            int64_t v;
            if (!r.read_i64(&v)) return false;
            indices.push_back(v);
        }
        idx->entries[k] = std::move(indices);
    }
    return true;
}

}  // namespace

// ═══════════════════════════════════════════════════════════
// Public encode
// ═══════════════════════════════════════════════════════════
std::string checkpoint_encode(const Checkpoint& ckp) {
    std::string b;
    b.reserve(1024);

    // 头部
    put_u32(b, kCheckpointFormatVersion);
    put_i64(b, ckp.version);
    put_i64(b, ckp.last_seq);
    put_i64(b, ckp.timestamp);
    put_u32(b, ckp.next_xid);

    // tables
    put_u32(b, static_cast<uint32_t>(ckp.tables.size()));
    for (const auto& t : ckp.tables) {
        put_string_u16(b, t.name);
        put_u32(b, static_cast<uint32_t>(t.columns.size()));
        for (const auto& c : t.columns) encode_column(b, c);
        put_i64(b, t.row_count);
    }

    // table_data
    put_u32(b, static_cast<uint32_t>(ckp.table_rows.size()));
    for (const auto& [name, rows] : ckp.table_rows) {
        put_string_u16(b, name);
        put_u32(b, static_cast<uint32_t>(rows.size()));
        for (const auto& [rowid, row] : rows) {
            put_i64(b, rowid);
            b += wal_encode_row(row);
        }
    }

    // row_id_counters
    put_u32(b, static_cast<uint32_t>(ckp.row_id_counters.size()));
    for (const auto& [k, v] : ckp.row_id_counters) {
        put_string_u16(b, k);
        put_i64(b, v);
    }

    // indexes_data
    put_u32(b, static_cast<uint32_t>(ckp.indexes_data.size()));
    for (const auto& [name, idxs] : ckp.indexes_data) {
        put_string_u16(b, name);
        put_u32(b, static_cast<uint32_t>(idxs.size()));
        for (const auto& idx : idxs) encode_index(b, idx);
    }

    // O-4: clog 段从 JSON 字符串改为二进制 clog_encode(clog)
    //   优先用 ckp.clog_bytes (binary); 兜底走 ckp.clog_entries.dump() 走 JSON
    std::string clog_bin;
    if (!ckp.clog_bytes.empty()) {
        clog_bin = ckp.clog_bytes;
    } else if (!ckp.clog_entries.is_null()) {
        // 兼容路径: 仅有 JSON. dump() 为字符串嵌入 (旧 O-3 行为).
        clog_bin = ckp.clog_entries.dump();
    }
    put_string_u32(b, clog_bin);

    // O-3: catalog 段从 JSON 字符串改为二进制 catalog_encode(catalog)
    //   优先用 ckp.catalog_bytes (binary); 兜底走 ckp.catalog_data.dump() 走 JSON
    std::string cat_bin;
    if (!ckp.catalog_bytes.empty()) {
        cat_bin = ckp.catalog_bytes;
    } else if (!ckp.catalog_data.is_null()) {
        // 兼容路径: 仅有 JSON. dump() 为字符串嵌入 (旧 O-2 行为).
        cat_bin = ckp.catalog_data.dump();
    }
    put_string_u32(b, cat_bin);

    return b;
}

// ═══════════════════════════════════════════════════════════
// 共享段解码: tables / table_data / row_id_counters / indexes_data
// (v2 和 v3 这些段布局相同, 抽出共用)
// ═══════════════════════════════════════════════════════════
namespace {

bool decode_common_segments(PayloadReader& r, Checkpoint* ckp) {
    // tables
    uint32_t table_count;
    if (!r.read_u32(&table_count)) return false;
    for (uint32_t i = 0; i < table_count; ++i) {
        TableSchema t;
        if (!r.read_string_u16(&t.name)) return false;
        uint32_t col_count;
        if (!r.read_u32(&col_count)) return false;
        for (uint32_t j = 0; j < col_count; ++j) {
            ColumnDef c;
            if (!decode_column(r, &c)) return false;
            t.columns.push_back(std::move(c));
        }
        if (!r.read_i64(&t.row_count)) return false;
        ckp->tables.push_back(std::move(t));
    }

    // table_data
    uint32_t td_count;
    if (!r.read_u32(&td_count)) return false;
    for (uint32_t i = 0; i < td_count; ++i) {
        std::string name;
        if (!r.read_string_u16(&name)) return false;
        uint32_t row_count;
        if (!r.read_u32(&row_count)) return false;
        auto& rows = ckp->table_rows[name];
        rows.reserve(row_count);
        for (uint32_t j = 0; j < row_count; ++j) {
            int64_t rid;
            if (!r.read_i64(&rid)) return false;
            Row row;
            if (!wal_decode_row(r, &row)) return false;
            rows.emplace_back(rid, std::move(row));
        }
    }

    // row_id_counters
    uint32_t c_count;
    if (!r.read_u32(&c_count)) return false;
    for (uint32_t i = 0; i < c_count; ++i) {
        std::string k;
        if (!r.read_string_u16(&k)) return false;
        int64_t v;
        if (!r.read_i64(&v)) return false;
        ckp->row_id_counters[k] = v;
    }

    // indexes_data
    uint32_t id_count;
    if (!r.read_u32(&id_count)) return false;
    for (uint32_t i = 0; i < id_count; ++i) {
        std::string name;
        if (!r.read_string_u16(&name)) return false;
        uint32_t idx_count;
        if (!r.read_u32(&idx_count)) return false;
        auto& idxs = ckp->indexes_data[name];
        idxs.reserve(idx_count);
        for (uint32_t j = 0; j < idx_count; ++j) {
            IndexInfo idx;
            if (!decode_index(r, &idx)) return false;
            idxs.push_back(std::move(idx));
        }
    }
    return true;
}

}  // namespace

// ═══════════════════════════════════════════════════════════
// Public decode (v2 / v3 dispatcher)
// ═══════════════════════════════════════════════════════════
bool checkpoint_decode(const std::string& bytes,
                       Checkpoint* ckp,
                       std::string* out_clog_json,
                       std::string* out_catalog_json) {
    if (!ckp) return false;
    PayloadReader r(bytes);

    uint32_t fmt;
    if (!r.read_u32(&fmt)) return false;

    // 头部 (v2 / v3 公共)
    if (fmt == kCheckpointFormatVersionV3 || fmt == kCheckpointFormatVersionV2) {
        if (!r.read_i64(&ckp->version))    return false;
        if (!r.read_i64(&ckp->last_seq))   return false;
        if (!r.read_i64(&ckp->timestamp))  return false;
        if (!r.read_u32(&ckp->next_xid))   return false;
    } else {
        return false;  // 未知版本
    }

    // 共享段
    if (!decode_common_segments(r, ckp)) return false;

    // 尾部: clog + catalog
    //   v3: clog=binary (sniff 4B magic "CLGB", 旧文件无 magic 走 JSON fallback),
    //         catalog=binary
    //   v2: clog=JSON, catalog=JSON
    std::string clog_blob;
    if (out_clog_json) {
        if (!r.read_string_u32(out_clog_json)) return false;
        clog_blob = *out_clog_json;
    } else {
        if (!r.read_string_u32(&clog_blob)) return false;
    }

    std::string catalog_blob;
    if (out_catalog_json) {
        if (!r.read_string_u32(out_catalog_json)) return false;
        catalog_blob = *out_catalog_json;
    } else {
        if (!r.read_string_u32(&catalog_blob)) return false;
    }

    if (fmt == kCheckpointFormatVersionV3) {
        // 始终视为 binary Catalog. caller 用 catalog_decode 还原
        ckp->catalog_bytes = std::move(catalog_blob);
        ckp->catalog_data = json(nullptr);  // 清掉旧 JSON

        // O-4: clog 嗅探 — 旧 v3 文件嵌入 JSON 数组 (无 magic), 新 v3 文件嵌入
        //   binary (开头 4B "CLGB"). 用 magic 区分, 找不到则走 JSON 兜底.
        ckp->clog_entries = json(nullptr);
        ckp->clog_bytes.clear();
        if (clog_blob.size() >= kCLogMagicLen &&
            std::memcmp(clog_blob.data(), kCLogMagic, kCLogMagicLen) == 0) {
            ckp->clog_bytes = std::move(clog_blob);
        } else if (!clog_blob.empty()) {
            try { ckp->clog_entries = json::parse(clog_blob); }
            catch (...) { return false; }
        }
    } else {
        // v2 视为 JSON 字符串 (clog + catalog)
        ckp->catalog_data = json(nullptr);
        if (!catalog_blob.empty()) {
            try { ckp->catalog_data = json::parse(catalog_blob); }
            catch (...) { return false; }
        }
        ckp->clog_entries = json(nullptr);
        if (!clog_blob.empty()) {
            try { ckp->clog_entries = json::parse(clog_blob); }
            catch (...) { return false; }
        }
        ckp->clog_bytes.clear();
    }

    return r.eof();
}

}  // namespace jiamiao
