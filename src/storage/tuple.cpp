/* ══════════════════════════════════════════════════════════════════════
   tuple.cpp — BinaryRowCodec 实现 + FNV-1a

   Phase 1: 简单二进制编码, 不带 CRC (Phase 2 升级).
   ══════════════════════════════════════════════════════════════════════ */

#include "tuple.h"

#include <cassert>
#include <cstring>
#include <stdexcept>

namespace jiamiao {

// ── FNV-1a 16-bit ──
uint16_t Tuple::fnv1a_16(const std::string& s) {
    uint32_t h = 0x811C9DC5u;  // FNV-1a 32-bit offset basis, 我们只用低 16
    for (unsigned char c : s) {
        h ^= c;
        h *= 0x01000193u;  // FNV prime
    }
    return static_cast<uint16_t>(h & 0xFFFFu);
}

// ── 辅助: 写小端 u16/u32/u64 ──
namespace {
inline void put_u16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xff));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
}
inline void put_u32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xff));
    }
}
inline void put_u64(std::vector<uint8_t>& out, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xff));
    }
}
inline uint16_t get_u16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}
inline uint32_t get_u32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
inline uint64_t get_u64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<uint64_t>(p[i]) << (i * 8);
    }
    return v;
}
}  // namespace

// ── encode: Row → bytes ──
std::vector<uint8_t> BinaryRowCodec::encode(const Row& row) {
    std::vector<uint8_t> out;
    // 不计入系统列 (header 已有)
    uint16_t ncols = 0;
    for (const auto& [k, _] : row) {
        if (k == "_rowid" || k == "_xmin" || k == "_xmax" || k == "_cid") continue;
        ++ncols;
    }
    put_u16(out, ncols);

    for (const auto& [k, v] : row) {
        if (k == "_rowid" || k == "_xmin" || k == "_xmax" || k == "_cid") continue;

        // name_len + name
        if (k.size() > 0xFFFFu) {
            throw std::runtime_error("BinaryRowCodec: column name too long: " + k);
        }
        put_u16(out, static_cast<uint16_t>(k.size()));
        out.insert(out.end(), k.begin(), k.end());

        // type_tag + value
        std::visit([&out](auto&& val) {
            using T = std::decay_t<decltype(val)>;
            if constexpr (std::is_same_v<T, std::nullptr_t>) {
                out.push_back(0);  // tag=0 null
            } else if constexpr (std::is_same_v<T, int64_t>) {
                out.push_back(1);  // tag=1 i64
                put_u64(out, static_cast<uint64_t>(val));
            } else if constexpr (std::is_same_v<T, double>) {
                out.push_back(2);  // tag=2 f64
                uint64_t bits;
                std::memcpy(&bits, &val, sizeof(bits));
                put_u64(out, bits);
            } else if constexpr (std::is_same_v<T, bool>) {
                out.push_back(3);  // tag=3 bool
                out.push_back(val ? 1 : 0);
            } else if constexpr (std::is_same_v<T, std::string>) {
                out.push_back(4);  // tag=4 string
                if (val.size() > 0xFFFFFFFFu) {
                    throw std::runtime_error("BinaryRowCodec: string value too long");
                }
                put_u32(out, static_cast<uint32_t>(val.size()));
                out.insert(out.end(), val.begin(), val.end());
            }
        }, v);
    }
    return out;
}

// ── decode: bytes → Row ──
Row BinaryRowCodec::decode(const uint8_t* data, size_t len) {
    Row out;
    if (len < 2) return out;  // 空 row

    const uint8_t* p = data;
    const uint8_t* end = data + len;

    uint16_t ncols = get_u16(p);
    p += 2;
    if (p + 2 > end) throw std::runtime_error("BinaryRowCodec: truncated header");

    for (uint16_t i = 0; i < ncols; ++i) {
        if (p + 2 > end) throw std::runtime_error("BinaryRowCodec: truncated name_len");
        uint16_t name_len = get_u16(p);
        p += 2;
        if (p + name_len > end) throw std::runtime_error("BinaryRowCodec: truncated name");
        std::string name(reinterpret_cast<const char*>(p), name_len);
        p += name_len;
        if (p >= end) throw std::runtime_error("BinaryRowCodec: missing type_tag");
        uint8_t tag = *p++;
        Value v;
        switch (tag) {
            case 0: v = nullptr; break;
            case 1:
                if (p + 8 > end) throw std::runtime_error("BinaryRowCodec: truncated i64");
                v = static_cast<int64_t>(get_u64(p));
                p += 8;
                break;
            case 2: {
                if (p + 8 > end) throw std::runtime_error("BinaryRowCodec: truncated f64");
                uint64_t bits = get_u64(p);
                double d;
                std::memcpy(&d, &bits, sizeof(d));
                v = d;
                p += 8;
                break;
            }
            case 3:
                if (p >= end) throw std::runtime_error("BinaryRowCodec: truncated bool");
                v = (*p++ != 0);
                break;
            case 4: {
                if (p + 4 > end) throw std::runtime_error("BinaryRowCodec: truncated string len");
                uint32_t slen = get_u32(p);
                p += 4;
                if (p + slen > end) throw std::runtime_error("BinaryRowCodec: truncated string body");
                v = std::string(reinterpret_cast<const char*>(p), slen);
                p += slen;
                break;
            }
            default:
                throw std::runtime_error("BinaryRowCodec: unknown type_tag " + std::to_string(tag));
        }
        out[name] = std::move(v);
    }
    return out;
}

// ── tuple_to_row: 含系统列投影 ──
Row BinaryRowCodec::tuple_to_row(const Tuple& t) {
    Row r = decode(t.payload.data(), t.payload.size());
    r["_rowid"] = static_cast<int64_t>(t.hdr.row_id);
    r["_xmin"]  = static_cast<int64_t>(t.hdr.xmin);
    r["_xmax"]  = static_cast<int64_t>(t.hdr.xmax);
    r["_cid"]   = static_cast<int64_t>(t.hdr.cid);
    return r;
}

Tuple BinaryRowCodec::row_to_tuple(const Row& row, const TupleHeader& hdr) {
    Tuple t;
    t.hdr = hdr;
    t.payload = encode(row);
    t.hdr.payload_len = static_cast<uint32_t>(t.payload.size());
    return t;
}

}  // namespace jiamiao
