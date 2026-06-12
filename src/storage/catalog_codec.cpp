/* ═══════════════════════════════════════════════════════════════════════
   catalog_codec.cpp — O-3 Catalog 二进制 codec 实现
   ═══════════════════════════════════════════════════════════════════════ */

#include "catalog_codec.h"

static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
              "Catalog binary format is little-endian; this platform is big-endian");

namespace jiamiao {

namespace {

inline void put_u16(std::string& b, uint16_t v) {
    b.push_back(static_cast<char>(v & 0xFFU));
    b.push_back(static_cast<char>((v >> 8) & 0xFFU));
}
inline void put_u32(std::string& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<char>((v >> (i * 8)) & 0xFFU));
}
inline void put_string_u16(std::string& b, const std::string& s) {
    put_u16(b, static_cast<uint16_t>(s.size()));
    b.append(s);
}

class Reader {
public:
    Reader(const char* d, size_t l) : data_(d), len_(l), pos_(0) {}
    bool eof() const { return pos_ >= len_; }
    size_t remaining() const { return len_ - pos_; }
    bool read_u16(uint16_t* out) {
        if (remaining() < 2) return false;
        *out = static_cast<uint8_t>(data_[pos_])
             | (static_cast<uint16_t>(static_cast<uint8_t>(data_[pos_ + 1])) << 8);
        pos_ += 2; return true;
    }
    bool read_u32(uint32_t* out) {
        if (remaining() < 4) return false;
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i)
            v |= static_cast<uint32_t>(static_cast<uint8_t>(data_[pos_ + i])) << (i * 8);
        *out = v; pos_ += 4; return true;
    }
    bool read_string_u16(std::string* out) {
        uint16_t len; if (!read_u16(&len)) return false;
        if (remaining() < len) return false;
        out->assign(data_ + pos_, len); pos_ += len; return true;
    }
private:
    const char* data_; size_t len_; size_t pos_;
};

}  // namespace

// ═══════════════════════════════════════════════════════════
// Public encode
// ═══════════════════════════════════════════════════════════
std::string catalog_encode(const Catalog& cat) {
    std::string b;
    b.reserve(256);

    Catalog::Snapshot snap = cat.internal_snapshot();

    put_u32(b, kCatalogFormatVersion);
    put_string_u16(b, snap.current_db);

    // databases
    put_u32(b, static_cast<uint32_t>(snap.databases.size()));
    for (const auto& [db_name, schemas] : snap.databases) {
        put_string_u16(b, db_name);
        put_u32(b, static_cast<uint32_t>(schemas.size()));
        for (const auto& sn : schemas) {
            put_string_u16(b, sn);
        }
    }

    // users
    put_u32(b, static_cast<uint32_t>(snap.users.size()));
    for (const auto& [uname, pw, salt] : snap.users) {
        put_string_u16(b, uname);
        put_string_u16(b, pw);
        put_string_u16(b, salt);
    }

    return b;
}

// ═══════════════════════════════════════════════════════════
// Public decode
// ═══════════════════════════════════════════════════════════
bool catalog_decode(const std::string& bytes, Catalog* cat) {
    if (!cat) return false;
    Reader r(bytes.data(), bytes.size());

    uint32_t fmt;
    if (!r.read_u32(&fmt)) return false;
    if (fmt != kCatalogFormatVersion) return false;

    std::string cur_db;
    if (!r.read_string_u16(&cur_db)) return false;

    // databases
    uint32_t db_count;
    if (!r.read_u32(&db_count)) return false;
    for (uint32_t i = 0; i < db_count; ++i) {
        std::string db_name;
        if (!r.read_string_u16(&db_name)) return false;
        uint32_t schema_count;
        if (!r.read_u32(&schema_count)) return false;

        // 先确保 database 存在
        if (!cat->database_exists(db_name)) {
            cat->create_database(db_name);
        }
        for (uint32_t j = 0; j < schema_count; ++j) {
            std::string sn;
            if (!r.read_string_u16(&sn)) return false;
            if (!cat->schema_exists(db_name, sn)) {
                cat->create_schema(db_name, sn);
            }
        }
    }

    // users
    uint32_t user_count;
    if (!r.read_u32(&user_count)) return false;
    for (uint32_t i = 0; i < user_count; ++i) {
        std::string uname, pw, salt;
        if (!r.read_string_u16(&uname)) return false;
        if (!r.read_string_u16(&pw))    return false;
        if (!r.read_string_u16(&salt))  return false;
        // 绕过 hash_password: 直接灌入 hash + salt
        cat->internal_set_user(uname, pw, salt);
    }

    if (!cur_db.empty()) {
        cat->set_current_database(cur_db);
    }
    return r.eof();
}

}  // namespace jiamiao
