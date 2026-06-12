/* ═══════════════════════════════════════════════════════════════════════
   clog_codec.cpp — CLog 二进制 codec 实现 (O-4)
   ═══════════════════════════════════════════════════════════════════════ */

#include "clog_codec.h"

#include <cstring>

static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
              "CLog binary format is little-endian; this platform is big-endian");

namespace jiamiao {

namespace {

inline void put_u32(std::string& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<char>((v >> (i * 8)) & 0xFFU));
}
inline void put_u8(std::string& b, uint8_t v) { b.push_back(static_cast<char>(v)); }

inline bool read_u32(const char*& p, const char* end, uint32_t* out) {
    if (end - p < 4) return false;
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= (static_cast<uint8_t>(p[i]) << (i * 8));
    *out = v;
    p += 4;
    return true;
}
inline bool read_u8(const char*& p, const char* end, uint8_t* out) {
    if (end - p < 1) return false;
    *out = static_cast<uint8_t>(*p);
    p += 1;
    return true;
}

}  // namespace

std::string clog_encode(const CLog& clog) {
    // 拿到所有已知 xid (caller 上锁: checkpoint 阶段单线程; CLog 自身 mutex 不阻塞 enumerate)
    auto xids = clog.all_xids();
    std::string b;
    b.reserve(4 + 4 + 4 + xids.size() * 5);

    b.append(kCLogMagic, kCLogMagicLen);    // 4B
    put_u32(b, kCLogFormatVersion);          // u32 fmt
    put_u32(b, static_cast<uint32_t>(xids.size()));  // u32 count

    for (TransactionId xid : xids) {
        put_u32(b, xid);                     // u32 xid
        put_u8(b, static_cast<uint8_t>(clog.get_status(xid)));  // u8 status
    }
    return b;
}

bool clog_decode(const std::string& bytes, CLog* clog) {
    if (!clog) return false;
    if (bytes.size() < kCLogMagicLen + 4 + 4) return false;

    // 嗅探 magic (caller 已确认, 但二次校验更稳)
    if (std::memcmp(bytes.data(), kCLogMagic, kCLogMagicLen) != 0) return false;

    const char* p   = bytes.data() + kCLogMagicLen;
    const char* end = bytes.data() + bytes.size();

    uint32_t fmt;
    if (!read_u32(p, end, &fmt)) return false;
    if (fmt != kCLogFormatVersion) return false;

    uint32_t count;
    if (!read_u32(p, end, &count)) return false;

    for (uint32_t i = 0; i < count; ++i) {
        uint32_t xid;
        uint8_t  status_byte;
        if (!read_u32(p, end, &xid))        return false;
        if (!read_u8(p, end, &status_byte)) return false;
        // 0..3 范围; 越界视为损坏
        if (status_byte > 3) return false;
        clog->set_status(static_cast<TransactionId>(xid),
                         static_cast<TransactionStatus>(status_byte));
    }
    return p == end;
}

}  // namespace jiamiao
