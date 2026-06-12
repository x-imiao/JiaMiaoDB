/* ═══════════════════════════════════════════════════════════════════════
   wal_payload.cpp — 二进制 WAL payload codec 实现
   ═══════════════════════════════════════════════════════════════════════ */

#include "wal_payload.h"

#include <cstring>

static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
              "WAL payload binary format is little-endian; this platform is big-endian");

namespace jiamiao {

// ═══════════════════════════════════════════════════════════
// 小端字节序原语
// ═══════════════════════════════════════════════════════════
namespace {

inline void put_u8(std::string& b, uint8_t v) {
    b.push_back(static_cast<char>(v));
}
inline void put_u16(std::string& b, uint16_t v) {
    b.push_back(static_cast<char>(v & 0xFFU));
    b.push_back(static_cast<char>((v >> 8) & 0xFFU));
}
inline void put_u32(std::string& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        b.push_back(static_cast<char>((v >> (i * 8)) & 0xFFU));
    }
}
inline void put_u64(std::string& b, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        b.push_back(static_cast<char>((v >> (i * 8)) & 0xFFU));
    }
}
inline void put_i64(std::string& b, int64_t v) {
    put_u64(b, static_cast<uint64_t>(v));
}
inline void put_f64(std::string& b, double v) {
    uint64_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    put_u64(b, bits);
}
inline void put_bytes(std::string& b, const char* data, size_t len) {
    b.append(data, len);
}
inline void put_string_u16(std::string& b, const std::string& s) {
    // 假设 s.size() <= 65535
    put_u16(b, static_cast<uint16_t>(s.size()));
    b.append(s);
}
inline void put_string_u32(std::string& b, const std::string& s) {
    put_u32(b, static_cast<uint32_t>(s.size()));
    b.append(s);
}

// 暴露给外部 codec (checkpoint_codec) 复用的共享位置 reader
// 定义在 jiamiao 命名空间顶层
}  // namespace

bool PayloadReader::read_u8(uint8_t* out) {
    if (remaining() < 1) return false;
    *out = static_cast<uint8_t>(data[pos]); pos += 1; return true;
}
bool PayloadReader::read_u16(uint16_t* out) {
    if (remaining() < 2) return false;
    *out = static_cast<uint8_t>(data[pos])
         | (static_cast<uint16_t>(static_cast<uint8_t>(data[pos + 1])) << 8);
    pos += 2; return true;
}
bool PayloadReader::read_u32(uint32_t* out) {
    if (remaining() < 4) return false;
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i)
        v |= static_cast<uint32_t>(static_cast<uint8_t>(data[pos + i])) << (i * 8);
    *out = v; pos += 4; return true;
}
bool PayloadReader::read_u64(uint64_t* out) {
    if (remaining() < 8) return false;
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= static_cast<uint64_t>(static_cast<uint8_t>(data[pos + i])) << (i * 8);
    *out = v; pos += 8; return true;
}
bool PayloadReader::read_i64(int64_t* out) {
    uint64_t u; if (!read_u64(&u)) return false;
    *out = static_cast<int64_t>(u); return true;
}
bool PayloadReader::read_f64(double* out) {
    uint64_t u; if (!read_u64(&u)) return false;
    std::memcpy(out, &u, sizeof(*out)); return true;
}
bool PayloadReader::read_string_u16(std::string* out) {
    uint16_t len; if (!read_u16(&len)) return false;
    if (remaining() < len) return false;
    out->assign(data + pos, len); pos += len; return true;
}
bool PayloadReader::read_string_u32(std::string* out) {
    uint32_t len; if (!read_u32(&len)) return false;
    if (remaining() < len) return false;
    out->assign(data + pos, len); pos += len; return true;
}

namespace {  // 重新进入匿名 namespace 放内部细节
// (这里原本的 Reader 类已删除, 由 PayloadReader 替代)

// ─── Value (variant) 编解码 ───
void encode_value(std::string& b, const Value& v) {
    std::visit([&](auto&& val) {
        using T = std::decay_t<decltype(val)>;
        if constexpr (std::is_same_v<T, std::nullptr_t>) {
            put_u8(b, static_cast<uint8_t>(WALValTag::Null));
        } else if constexpr (std::is_same_v<T, int64_t>) {
            put_u8(b, static_cast<uint8_t>(WALValTag::Int64));
            put_i64(b, val);
        } else if constexpr (std::is_same_v<T, double>) {
            put_u8(b, static_cast<uint8_t>(WALValTag::Float64));
            put_f64(b, val);
        } else if constexpr (std::is_same_v<T, bool>) {
            put_u8(b, static_cast<uint8_t>(WALValTag::Bool));
            put_u8(b, val ? 1u : 0u);
        } else if constexpr (std::is_same_v<T, std::string>) {
            put_u8(b, static_cast<uint8_t>(WALValTag::String));
            put_string_u32(b, val);
        }
    }, v);
}

bool decode_value(PayloadReader& r, Value* out) {
    uint8_t tag;
    if (!r.read_u8(&tag)) return false;
    switch (static_cast<WALValTag>(tag)) {
        case WALValTag::Null:
            *out = nullptr;
            return true;
        case WALValTag::Int64: {
            int64_t v;
            if (!r.read_i64(&v)) return false;
            *out = v;
            return true;
        }
        case WALValTag::Float64: {
            double v;
            if (!r.read_f64(&v)) return false;
            *out = v;
            return true;
        }
        case WALValTag::Bool: {
            uint8_t v;
            if (!r.read_u8(&v)) return false;
            *out = (v != 0);
            return true;
        }
        case WALValTag::String: {
            std::string v;
            if (!r.read_string_u32(&v)) return false;
            *out = std::move(v);
            return true;
        }
        default:
            return false;  // 未知 tag
    }
}

void encode_row_into(std::string& b, const Row& row) {
    put_u32(b, static_cast<uint32_t>(row.size()));
    for (const auto& [k, v] : row) {
        put_string_u16(b, k);
        encode_value(b, v);
    }
}

bool decode_row_from(PayloadReader& r, Row* out) {
    uint32_t count;
    if (!r.read_u32(&count)) return false;
    Row m;
    for (uint32_t i = 0; i < count; ++i) {
        std::string key;
        if (!r.read_string_u16(&key)) return false;
        Value val;
        if (!decode_value(r, &val)) return false;
        m.emplace(std::move(key), std::move(val));
    }
    *out = std::move(m);
    return true;
}

}  // namespace

// ═══════════════════════════════════════════════════════════
// Public Row codec
// ═══════════════════════════════════════════════════════════
std::string wal_encode_row(const Row& row) {
    std::string b;
    b.reserve(64);
    encode_row_into(b, row);
    return b;
}

bool wal_decode_row(const std::string& bytes, Row* out) {
    PayloadReader r(bytes);
    if (!decode_row_from(r, out)) return false;
    return r.eof();
}

// 共享位置的 decode: 供其它 codec (checkpoint_codec) 复用
bool wal_decode_value(PayloadReader& r, Value* out) { return decode_value(r, out); }
bool wal_decode_row  (PayloadReader& r, Row*   out) { return decode_row_from(r, out); }

// ═══════════════════════════════════════════════════════════
// Insert
// ═══════════════════════════════════════════════════════════
std::string wal_encode_insert(const InsertPayload& p) {
    std::string b;
    b.reserve(64);
    put_i64(b, p.rowid);
    encode_row_into(b, p.row);
    return b;
}

bool wal_decode_insert(const std::string& bytes, InsertPayload* out) {
    PayloadReader r(bytes);
    if (!r.read_i64(&out->rowid)) return false;
    if (!decode_row_from(r, &out->row)) return false;
    return r.eof();
}

// ═══════════════════════════════════════════════════════════
// Update
// ═══════════════════════════════════════════════════════════
std::string wal_encode_update(const UpdatePayload& p) {
    std::string b;
    b.reserve(128);
    put_i64(b, p.rowid);
    encode_row_into(b, p.updates);
    encode_row_into(b, p.new_row);
    return b;
}

bool wal_decode_update(const std::string& bytes, UpdatePayload* out) {
    PayloadReader r(bytes);
    if (!r.read_i64(&out->rowid)) return false;
    if (!decode_row_from(r, &out->updates)) return false;
    if (!decode_row_from(r, &out->new_row)) return false;
    return r.eof();
}

// ═══════════════════════════════════════════════════════════
// Delete
// ═══════════════════════════════════════════════════════════
std::string wal_encode_delete(const DeletePayload& p) {
    std::string b;
    b.reserve(16);
    put_i64(b, p.rowid);
    put_u8(b, p.has_xmax ? 1u : 0u);
    if (p.has_xmax) put_u32(b, p.xmax);
    return b;
}

bool wal_decode_delete(const std::string& bytes, DeletePayload* out) {
    PayloadReader r(bytes);
    if (!r.read_i64(&out->rowid)) return false;
    uint8_t flag;
    if (!r.read_u8(&flag)) return false;
    out->has_xmax = (flag != 0);
    if (out->has_xmax) {
        if (!r.read_u32(&out->xmax)) return false;
    } else {
        out->xmax = 0;
    }
    return r.eof();
}

// ═══════════════════════════════════════════════════════════
// CreateTable
// ═══════════════════════════════════════════════════════════
std::string wal_encode_create_table(const CreateTablePayload& p) {
    std::string b;
    b.reserve(64);
    put_u32(b, static_cast<uint32_t>(p.columns.size()));
    for (const auto& c : p.columns) {
        put_string_u16(b, c.name);
        put_u8(b, static_cast<uint8_t>(c.type));
        put_u8(b, c.nullable ? 1u : 0u);
        put_u8(b, c.primary_key ? 1u : 0u);
    }
    return b;
}

bool wal_decode_create_table(const std::string& bytes, CreateTablePayload* out) {
    PayloadReader r(bytes);
    uint32_t col_count;
    if (!r.read_u32(&col_count)) return false;
    out->columns.clear();
    out->columns.reserve(col_count);
    for (uint32_t i = 0; i < col_count; ++i) {
        ColumnDef c;
        if (!r.read_string_u16(&c.name)) return false;
        uint8_t tid, nullable, pk;
        if (!r.read_u8(&tid))      return false;
        if (!r.read_u8(&nullable)) return false;
        if (!r.read_u8(&pk))       return false;
        c.type        = static_cast<DataType>(tid);
        c.nullable    = (nullable != 0);
        c.primary_key = (pk != 0);
        out->columns.push_back(std::move(c));
    }
    return r.eof();
}

// ═══════════════════════════════════════════════════════════
// CreateSchema
// ═══════════════════════════════════════════════════════════
std::string wal_encode_create_schema(const CreateSchemaPayload& p) {
    std::string b;
    b.reserve(8 + p.schema.size());
    put_string_u16(b, p.schema);
    return b;
}

bool wal_decode_create_schema(const std::string& bytes, CreateSchemaPayload* out) {
    PayloadReader r(bytes);
    if (!r.read_string_u16(&out->schema)) return false;
    return r.eof();
}

}  // namespace jiamiao
