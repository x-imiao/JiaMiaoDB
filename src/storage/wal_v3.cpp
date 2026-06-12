/* ═══════════════════════════════════════════════════════════════════════
   wal_v3.cpp — Phase 2 binary WAL 实现 (length-prefix + CRC32 + 可选 fsync)
   ═══════════════════════════════════════════════════════════════════════ */

#include "wal_v3.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unordered_map>

static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
              "WAL v3 binary format is little-endian; this platform is big-endian");

namespace jiamiao {

// ═══════════════════════════════════════════════════════════
// op string ↔ enum 映射
// ═══════════════════════════════════════════════════════════
namespace {
struct OpMap {
    std::unordered_map<std::string, uint16_t> s2e;
    std::unordered_map<uint16_t, std::string> e2s;
    OpMap() {
        auto add = [&](const char* s, WalOp e) {
            s2e.emplace(s, static_cast<uint16_t>(e));
            e2s.emplace(static_cast<uint16_t>(e), s);
        };
        add("insert",          WalOp::kInsert);
        add("update",          WalOp::kUpdate);
        add("delete",          WalOp::kDelete);
        add("xact_commit",     WalOp::kXactCommit);
        add("xact_abort",      WalOp::kXactAbort);
        add("create_table",    WalOp::kCreateTable);
        add("drop_table",      WalOp::kDropTable);
        add("create_database", WalOp::kCreateDatabase);
        add("drop_database",   WalOp::kDropDatabase);
        add("create_schema",   WalOp::kCreateSchema);
        add("create_user",     WalOp::kCreateUser);
        add("drop_user",       WalOp::kDropUser);
    }
};
const OpMap& op_map() {
    static OpMap m;
    return m;
}
}  // namespace

uint16_t op_to_enum(const std::string& op_str) {
    const auto& m = op_map().s2e;
    auto it = m.find(op_str);
    if (it == m.end()) return static_cast<uint16_t>(WalOp::kUnknown);
    return it->second;
}

std::string enum_to_op(uint16_t op) {
    const auto& m = op_map().e2s;
    auto it = m.find(op);
    if (it == m.end()) return "";
    return it->second;
}

// ═══════════════════════════════════════════════════════════
// CRC32 (Ethernet/zlib polynomial 0xEDB88320)
// ═══════════════════════════════════════════════════════════
namespace {
struct Crc32Table {
    uint32_t t[256];
    Crc32Table() {
        constexpr uint32_t kPoly = 0xEDB88320U;
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) ? (kPoly ^ (c >> 1)) : (c >> 1);
            }
            t[i] = c;
        }
    }
};
const Crc32Table& crc32_table() {
    static Crc32Table tbl;
    return tbl;
}
}  // namespace

uint32_t crc32_compute(const uint8_t* data, size_t len) {
    const auto& tbl = crc32_table();
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < len; ++i) {
        crc = tbl.t[(crc ^ data[i]) & 0xFFU] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFU;
}

// ═══════════════════════════════════════════════════════════
// Binary encode helpers (small-endian, 直接写 raw 字节)
// ═══════════════════════════════════════════════════════════
namespace {
template <typename Alloc>
inline void put_u16(std::vector<uint8_t, Alloc>& b, uint16_t v) {
    b.push_back(static_cast<uint8_t>(v & 0xFFU));
    b.push_back(static_cast<uint8_t>((v >> 8) & 0xFFU));
}
template <typename Alloc>
inline void put_u32(std::vector<uint8_t, Alloc>& b, uint32_t v) {
    b.push_back(static_cast<uint8_t>(v & 0xFFU));
    b.push_back(static_cast<uint8_t>((v >> 8) & 0xFFU));
    b.push_back(static_cast<uint8_t>((v >> 16) & 0xFFU));
    b.push_back(static_cast<uint8_t>((v >> 24) & 0xFFU));
}
template <typename Alloc>
inline void put_u64(std::vector<uint8_t, Alloc>& b, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        b.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFFU));
    }
}
inline uint16_t read_u16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}
inline uint32_t read_u32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0])
         | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16)
         | (static_cast<uint32_t>(p[3]) << 24);
}
inline uint64_t read_u64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<uint64_t>(p[i]) << (i * 8);
    }
    return v;
}
}  // namespace

// ═══════════════════════════════════════════════════════════
// WALRecordV3::encode / decode
// ═══════════════════════════════════════════════════════════
std::vector<uint8_t, JMAlloc<uint8_t>> WALRecordV3::encode() const {
    std::vector<uint8_t, JMAlloc<uint8_t>> buf;
    buf.reserve(38 + table.size() + data.size());

    // magic + version + op + seq + timestamp + tlen + table + xid + data_len + data
    put_u32(buf, kWalV3Magic);
    put_u16(buf, kWalV3Version);
    put_u16(buf, op);
    put_u64(buf, static_cast<uint64_t>(seq));
    put_u64(buf, static_cast<uint64_t>(timestamp));
    put_u16(buf, static_cast<uint16_t>(table.size()));
    buf.insert(buf.end(), table.begin(), table.end());
    put_u32(buf, xid);
    put_u32(buf, static_cast<uint32_t>(data.size()));
    buf.insert(buf.end(), data.begin(), data.end());

    // crc32: 覆盖 buf 当前全部内容
    uint32_t crc = crc32_compute(buf.data(), buf.size());
    put_u32(buf, crc);

    return buf;
}

size_t WALRecordV3::decode(const uint8_t* buf, size_t len, WALRecordV3* out) {
    // 最小: 4(magic) + 2(ver) + 2(op) + 8(seq) + 8(ts) + 2(tlen) + 0(table) + 4(xid) + 4(dlen) + 0(data) + 4(crc) = 38
    constexpr size_t kMinHeader = 4 + 2 + 2 + 8 + 8 + 2;
    if (len < kMinHeader) return 0;

    size_t pos = 0;
    uint32_t magic = read_u32(buf + pos); pos += 4;
    if (magic != kWalV3Magic) return kCrcFailed;  // 非 v3 magic, 调用方应 fallback

    uint16_t version = read_u16(buf + pos); pos += 2;
    if (version != kWalV3Version) return kCrcFailed;

    uint16_t op = read_u16(buf + pos); pos += 2;
    uint64_t seq = read_u64(buf + pos); pos += 8;
    uint64_t ts  = read_u64(buf + pos); pos += 8;
    uint16_t tlen = read_u16(buf + pos); pos += 2;

    if (len < pos + tlen + 4 + 4) return 0;  // 不完整
    std::string table(reinterpret_cast<const char*>(buf + pos), tlen);
    pos += tlen;

    uint32_t xid = read_u32(buf + pos); pos += 4;
    uint32_t dlen = read_u32(buf + pos); pos += 4;

    if (len < pos + dlen + 4) return 0;  // 不完整
    std::string data(reinterpret_cast<const char*>(buf + pos), dlen);
    pos += dlen;

    uint32_t stored_crc = read_u32(buf + pos); pos += 4;

    // 校验 CRC: 覆盖 buf[0..pos-4]
    uint32_t computed = crc32_compute(buf, pos - 4);
    if (computed != stored_crc) return kCrcFailed;

    out->op        = op;
    out->seq       = static_cast<int64_t>(seq);
    out->timestamp = static_cast<int64_t>(ts);
    out->table     = std::move(table);
    out->xid       = xid;
    out->data      = std::move(data);
    return pos;
}

// ═══════════════════════════════════════════════════════════
// WriteAheadLogV3
// ═══════════════════════════════════════════════════════════
WriteAheadLogV3::WriteAheadLogV3(const std::string& path, SyncMode mode)
    : path_(path), sync_mode_(mode) {}

WriteAheadLogV3::~WriteAheadLogV3() {
    close();
}

void WriteAheadLogV3::open() {
    std::lock_guard<std::mutex> lk(mutex_);
    if (fd_ >= 0) return;
    fd_ = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd_ < 0) {
        throw std::runtime_error("无法打开 WAL 文件: " + path_ + " (" + std::strerror(errno) + ")");
    }
}

void WriteAheadLogV3::close() {
    std::lock_guard<std::mutex> lk(mutex_);
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

void WriteAheadLogV3::append(const WALRecordV3& rec) {
    std::lock_guard<std::mutex> lk(mutex_);
    append_locked(rec);
    maybe_sync_locked(rec);
}

void WriteAheadLogV3::append_locked(const WALRecordV3& rec) {
    if (fd_ < 0) {
        // 自动 open (兼容 v1 行为: append 前可省略 open)
        fd_ = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd_ < 0) {
            throw std::runtime_error(std::string("WAL append 自动打开失败: ") + std::strerror(errno));
        }
    }
    seq_ = rec.seq;
    auto bytes = rec.encode();
    const uint8_t* p = bytes.data();
    size_t remaining = bytes.size();
    while (remaining > 0) {
        ssize_t n = ::write(fd_, p, remaining);
        if (n < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error(std::string("WAL append 失败: ") + std::strerror(errno));
        }
        remaining -= static_cast<size_t>(n);
        p += n;
    }
}

void WriteAheadLogV3::maybe_sync_locked(const WALRecordV3& rec) {
    if (sync_mode_ == SyncMode::kNever) return;
    bool need = (sync_mode_ == SyncMode::kAlways);
    if (sync_mode_ == SyncMode::kOnCommit) {
        need = (rec.op == static_cast<uint16_t>(WalOp::kXactCommit) ||
                rec.op == static_cast<uint16_t>(WalOp::kXactAbort));
    }
    if (need && fd_ >= 0) {
#if defined(__APPLE__)
        ::fsync(fd_);  // macOS 无 fdatasync; F_FULLFSYNC 更强但慢, 此处用 fsync
#else
        ::fdatasync(fd_);
#endif
    }
}

void WriteAheadLogV3::sync() {
    std::lock_guard<std::mutex> lk(mutex_);
    if (fd_ >= 0) {
#if defined(__APPLE__)
        ::fsync(fd_);
#else
        ::fdatasync(fd_);
#endif
    }
}

void WriteAheadLogV3::set_sync_mode(SyncMode m) {
    std::lock_guard<std::mutex> lk(mutex_);
    sync_mode_ = m;
}

// ── replay: 读全文件, 自动 v2/v3 fallback ──
std::vector<WALRecordV3> WriteAheadLogV3::replay(int64_t from_seq) {
    std::vector<WALRecordV3> out;
    if (!std::filesystem::exists(path_)) return out;

    // 读全文件到内存 (WAL 通常 < 100MB, 后续 checkpoint 截断)
    std::ifstream in(path_, std::ios::binary);
    if (!in.is_open()) return out;
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(in)), {});
    if (buf.empty()) return out;

    // 判断 v3 还是 v2
    bool is_v3 = (buf.size() >= 4) &&
                 (buf[0] == 0x4A) && (buf[1] == 0x4D) &&
                 (buf[2] == 0x44) && (buf[3] == 0x42);  // "JMDB"

    if (is_v3) {
        size_t pos = 0;
        while (pos < buf.size()) {
            WALRecordV3 rec;
            size_t consumed = WALRecordV3::decode(buf.data() + pos, buf.size() - pos, &rec);
            if (consumed == 0) {
                // 尾部不完整, 停止
                std::cerr << "[WAL v3] 文件尾部不完整, 在 offset " << pos << " 处停止" << std::endl;
                break;
            }
            if (consumed == WALRecordV3::kCrcFailed) {
                // CRC 失败: 尝试找下一个 magic 跳过坏行
                std::cerr << "[WAL v3] CRC 校验失败在 offset " << pos << ", 尝试跳过" << std::endl;
                size_t skip = pos + 1;
                while (skip + 4 <= buf.size()) {
                    if (buf[skip] == 0x4A && buf[skip + 1] == 0x4D &&
                        buf[skip + 2] == 0x44 && buf[skip + 3] == 0x42) {
                        break;
                    }
                    ++skip;
                }
                if (skip + 4 > buf.size()) break;
                pos = skip;
                continue;
            }
            if (rec.seq > from_seq) {
                seq_ = std::max(seq_, rec.seq);
                out.push_back(std::move(rec));
            } else {
                seq_ = std::max(seq_, rec.seq);
            }
            pos += consumed;
        }
    } else {
        // O-5 后: 没有 v2 WAL 写入, 老 v2 文件无法回放 — 视为空
        std::cerr << "[WAL v3] 文件不含 JMDB magic, 不是 v3 格式 (v2 已被删除), 跳过" << std::endl;
    }

    return out;
}

// ── truncate: 重读 + 过滤 + 重写 (v3 格式) ──
void WriteAheadLogV3::truncate(int64_t cutoff_seq) {
    std::lock_guard<std::mutex> lk(mutex_);

    // 关闭当前 fd
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }

    // 读全部记录 (复用 replay 逻辑, 但避免改 seq_)
    int64_t saved_seq = seq_;
    seq_ = 0;
    std::vector<WALRecordV3> kept;
    {
        std::vector<WALRecordV3> all = replay(cutoff_seq);
        for (auto& r : all) {
            kept.push_back(std::move(r));
        }
    }
    seq_ = saved_seq;

    // 截断重写 (truncate 模式)
    int wfd = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (wfd < 0) {
        throw std::runtime_error("无法重写 WAL: " + path_ + " (" + std::strerror(errno) + ")");
    }
    for (const auto& rec : kept) {
        auto bytes = rec.encode();
        const uint8_t* p = bytes.data();
        size_t remaining = bytes.size();
        while (remaining > 0) {
            ssize_t n = ::write(wfd, p, remaining);
            if (n < 0) {
                if (errno == EINTR) continue;
                ::close(wfd);
                throw std::runtime_error(std::string("WAL truncate 写失败: ") + std::strerror(errno));
            }
            remaining -= static_cast<size_t>(n);
            p += n;
        }
    }
#if defined(__APPLE__)
    ::fsync(wfd);
#else
    ::fdatasync(wfd);
#endif
    ::close(wfd);

    // 重新打开为 append
    fd_ = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd_ < 0) {
        throw std::runtime_error("无法重新打开 WAL: " + path_ + " (" + std::strerror(errno) + ")");
    }

    if (!kept.empty()) {
        seq_ = kept.back().seq;
    }
}

}  // namespace jiamiao
