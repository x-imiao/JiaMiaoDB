/* ═══════════════════════════════════════════════════════════════════════
   wal_v3.h — Phase 2: 二进制 WAL (length-prefix + CRC32 + 可选 fsync)

   替换 wal.{h,cpp} 的 JSON Lines 实现.

   Record 二进制布局 (小端字节序):
   ┌─────────┬─────────┬──────┬──────┬──────────┬──────┬────────────┬──────────┬────────┐
   │ magic   │ version │ op   │ tlen │ table    │ xid  │ data_len   │ data     │ crc32  │
   │ 4B      │ 2B      │ 2B   │ 2B   │ var      │ 4B   │ 4B         │ var      │ 4B     │
   │ "JMDB"  │ =3      │ enum │      │ qualified│      │            │ json txt │        │
   └─────────┴─────────┴──────┴──────┴──────────┴──────┴────────────┴──────────┴────────┘

   还有一个 8B 的 seq 和 8B 的 timestamp 放在 magic 之后:
   实际布局: magic(4) + version(2) + op(2) + seq(8) + timestamp(8) + tlen(2) +
            table(tlen) + xid(4) + data_len(4) + data(data_len) + crc32(4)
   总最小开销 ≈ 38 字节 (无 table, 无 data)

   crc32 覆盖 magic..data 全部字节 (不含自身).

   v2 fallback: replay() 时, 若文件首 4 字节非 "JMDB", 整文件按 v2 JSON 解析.
   ═══════════════════════════════════════════════════════════════════════ */

#ifndef JIAMIAODB_WAL_V3_H
#define JIAMIAODB_WAL_V3_H

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "../vendor/json.h"
#include "common/jm_alloc.h"

using json = Json;

namespace jiamiao {

// WAL v3 格式常量
constexpr uint32_t kWalV3Magic   = 0x42444D4AU;   // "JMDB" 小端: 'J'=0x4A, 'M'=0x4D, 'D'=0x44, 'B'=0x42
constexpr uint16_t kWalV3Version = 3;

// op enum — 与 WAL v3 写入的 op 字段对应, replay 时 switch 派发
enum class WalOp : uint16_t {
    kUnknown        = 0,
    kInsert         = 1,
    kUpdate         = 2,
    kDelete         = 3,
    kXactCommit     = 4,
    kXactAbort      = 5,
    kCreateTable    = 6,
    kDropTable      = 7,
    kCreateDatabase = 8,
    kDropDatabase   = 9,
    kCreateSchema   = 10,
    kCreateUser     = 11,
    kDropUser       = 12,
};

// op string ↔ enum 映射 (供 engine.cpp 与 replay 用)
uint16_t op_to_enum(const std::string& op_str);
std::string enum_to_op(uint16_t op);

// CRC32 (Ethernet/zlib polynomial 0xEDB88320)
uint32_t crc32_compute(const uint8_t* data, size_t len);

// ── WAL v3 记录 (内存中) ──
struct WALRecordV3 {
    int64_t      seq        = 0;
    int64_t      timestamp  = 0;
    uint16_t     op         = 0;     // WalOp
    std::string  table;
    uint32_t     xid        = 0;     // 0 = non-tx
    std::string  data;               // JSON 文本 (内容仍 JSON, 框架 binary)

    // 编码: 整条记录 (含 magic, version, ..., crc32) → bytes.
    // crc32 内部计算, 调用方不需填.
    // 返回 vector 走 jmalloc (WALContext); 调用方应尽快消费 (.data() 拷到 fd / mmap 后即丢).
    std::vector<uint8_t, JMAlloc<uint8_t>> encode() const;

    // 解码: 从 buf 起始处尝试解析一条记录. 返回消费字节数 (>0).
    // 返回 0 表示数据不完整 (缓冲不够). 返回 SIZE_MAX 表示 CRC 失败 (调用方决定跳过策略).
    // 解析成功时 *out 被填充.
    static size_t decode(const uint8_t* buf, size_t len, WALRecordV3* out);

    static constexpr size_t kCrcFailed = SIZE_MAX;
};

// ── WriteAheadLogV3 ──
//   单文件 append-only, 可选 fdatasync 策略.
//   replay() 自动判断 v2/v3 并 fallback.
class WriteAheadLogV3 {
public:
    enum class SyncMode {
        kNever     = 0,  // 无 fsync (测试 / 极限性能)
        kOnCommit  = 1,  // xact_commit 时 fsync (默认)
        kAlways    = 2,  // 每条 append 都 fsync (最强持久性)
    };

    explicit WriteAheadLogV3(const std::string& path, SyncMode mode = SyncMode::kOnCommit);
    ~WriteAheadLogV3();

    WriteAheadLogV3(const WriteAheadLogV3&) = delete;
    WriteAheadLogV3& operator=(const WriteAheadLogV3&) = delete;

    void open();
    void close();

    // 写一条记录. 若 mode == kAlways 或 op == kXactCommit/kXactAbort (kOnCommit), 自动 fsync.
    void append(const WALRecordV3& rec);

    // 强制 fdatasync (落盘).
    void sync();

    void set_sync_mode(SyncMode m);

    int64_t current_seq() const { return seq_; }
    void    set_seq(int64_t s)  { seq_ = s; }

    // 回放: 返回所有 seq > from_seq 的记录, 按文件顺序.
    //   自动 fallback: 若文件首 4 字节非 magic, 按 v2 JSON 解析.
    std::vector<WALRecordV3> replay(int64_t from_seq);

    // 截断: 重写文件, 仅保留 seq > cutoff_seq 的记录.
    //   保留原始格式 (v3 写 v3, v2 文件升级到 v3).
    void truncate(int64_t cutoff_seq);

private:
    void append_locked(const WALRecordV3& rec);
    void maybe_sync_locked(const WALRecordV3& rec);

    std::string  path_;
    int          fd_       = -1;    // POSIX fd, fdatasync 用
    int64_t      seq_      = 0;
    SyncMode     sync_mode_;
    std::mutex   mutex_;
};

}  // namespace jiamiao

#endif  // JIAMIAODB_WAL_V3_H
