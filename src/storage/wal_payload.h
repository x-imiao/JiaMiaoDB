/* ═══════════════════════════════════════════════════════════════════════
   wal_payload.h — WAL record `data` 字段的二进制 codec (O-1 优化)

   WALRecordV3 的 `data` 字段从"JSON 文本"改为"二进制 payload",每个 op
   有自己的结构化布局,长度前缀 + 直接 raw bytes,无字段名,无引号转义.

   每个 op 的 payload 不重复 WALRecordV3 header 已有的字段 (table, xid, seq, op).
   xid 已在 WALRecordV3.xid; table/db/user name 已在 WALRecordV3.table.

   Payload 布局 (小端字节序):
   ─────────────────────────────────────────────────────────────────────────
   OP=kInsert (1):
     i64 rowid
     Row row                          // u32 count + entries (见下)

   OP=kUpdate (2):
     i64 rowid
     Row updates                       // 仅修改列
     Row new_row                       // 完整新版本

   OP=kDelete (3):
     i64 rowid
     u8  has_xmax                     // 0 = 物理删除; 1 = MVCC tombstone
     u32 xmax                         // 仅 has_xmax=1 时存在

   OP=kXactCommit / kXactAbort (4 / 5):
     (空 — xid 在 WALRecordV3.xid)

   OP=kCreateTable (6):
     u32 col_count
     for each col: u16 nlen, name, u8 type_id, u8 nullable, u8 primary_key

   OP=kDropTable (7):
     (空 — table 在 WALRecordV3.table)

   OP=kCreateDatabase (8):
     (空 — name = WALRecordV3.table)

   OP=kDropDatabase (9):
     (空 — name = WALRecordV3.table)

   OP=kCreateSchema (10):
     u16 schema_name_len, schema_name
     // database name = WALRecordV3.table

   OP=kCreateUser (11):
     (空 — name = WALRecordV3.table, 密码不入 WAL)

   OP=kDropUser (12):
     (空 — name = WALRecordV3.table)
   ─────────────────────────────────────────────────────────────────────────

   Row binary format:
     u32 count
     for each entry:
       u16 keylen, key_bytes
       u8  vtag                         // ValTag enum 见下
       value bytes (depend on vtag):
         Null:    (无)
         Int64:   i64
         Float64: f64 (IEEE 754)
         Bool:    u8 (0/1)
         String:  u32 len + bytes
   ═══════════════════════════════════════════════════════════════════════ */

#ifndef JIAMIAODB_WAL_PAYLOAD_H
#define JIAMIAODB_WAL_PAYLOAD_H

#include <cstdint>
#include <string>
#include <vector>

#include "../types.h"
#include "wal_v3.h"

namespace jiamiao {

// Row 值的二进制 tag (1B)
enum class WALValTag : uint8_t {
    Null    = 0,
    Int64   = 1,
    Float64 = 2,
    Bool    = 3,
    String  = 4,
};

// ═══════════════════════════════════════════════════════════
// 每个 op 的 payload 结构 (内存中)
// ═══════════════════════════════════════════════════════════

struct InsertPayload {
    int64_t rowid = 0;
    Row     row;
};

struct UpdatePayload {
    int64_t rowid = 0;
    Row     updates;
    Row     new_row;
};

struct DeletePayload {
    int64_t  rowid = 0;
    bool     has_xmax = false;     // MVCC tombstone: true; physical delete: false
    uint32_t xmax = 0;
};

struct CreateTablePayload {
    std::vector<ColumnDef> columns;
};

struct CreateSchemaPayload {
    std::string schema;  // database 已在 WALRecordV3.table
};

// ═══════════════════════════════════════════════════════════
// Encode / Decode API
//   encode_*: 序列化到 std::string (binary bytes, 不是 JSON 文本)
//   decode_*: 从 std::string 反序列化, 返回 true 成功
//
// 用法:
//   InsertPayload p; p.rowid = 42; p.row["name"] = std::string("Alice");
//   std::string bytes = wal_encode_insert(p);
//   WALRecordV3 rec; rec.op = WalOp::kInsert; rec.data = std::move(bytes);
//
// 反向:
//   InsertPayload p;
//   if (wal_decode_insert(v3rec.data, &p)) { /* 用 p */ }
// ═══════════════════════════════════════════════════════════

// Row 编解码 (供其他 op 复用)
std::string wal_encode_row(const Row& row);
bool        wal_decode_row(const std::string& bytes, Row* out);

// ─── 共享位置的内部 Reader (暴露给 checkpoint_codec 等其它 codec 复用) ───
//   读 Value 不会重新分配: 共用 reader 的 pos
struct PayloadReader {
    const char* data = nullptr;
    size_t      len = 0;
    size_t      pos = 0;

    PayloadReader() = default;
    PayloadReader(const char* d, size_t l) : data(d), len(l), pos(0) {}
    explicit PayloadReader(const std::string& b) : data(b.data()), len(b.size()), pos(0) {}

    bool eof() const { return pos >= len; }
    bool ok() const { return pos <= len; }
    size_t remaining() const { return len - pos; }

    bool read_u8(uint8_t* out);
    bool read_u16(uint16_t* out);
    bool read_u32(uint32_t* out);
    bool read_u64(uint64_t* out);
    bool read_i64(int64_t* out);
    bool read_f64(double* out);
    bool read_string_u16(std::string* out);
    bool read_string_u32(std::string* out);
};

// 共享位置的 decode (供其它 codec 在同一个 reader 上连续读多个 row)
bool wal_decode_value(PayloadReader& r, Value* out);
bool wal_decode_row  (PayloadReader& r, Row*   out);

// op-specific
std::string wal_encode_insert(const InsertPayload& p);
bool        wal_decode_insert(const std::string& bytes, InsertPayload* out);

std::string wal_encode_update(const UpdatePayload& p);
bool        wal_decode_update(const std::string& bytes, UpdatePayload* out);

std::string wal_encode_delete(const DeletePayload& p);
bool        wal_decode_delete(const std::string& bytes, DeletePayload* out);

std::string wal_encode_create_table(const CreateTablePayload& p);
bool        wal_decode_create_table(const std::string& bytes, CreateTablePayload* out);

std::string wal_encode_create_schema(const CreateSchemaPayload& p);
bool        wal_decode_create_schema(const std::string& bytes, CreateSchemaPayload* out);

// 用于 commit/abort/drop_table/drop_user/create_database/drop_database/create_user:
// 这些 op 的 payload 为空 (所需信息已在 WALRecordV3.table 或 WALRecordV3.xid)
inline std::string wal_encode_empty() { return {}; }

}  // namespace jiamiao

#endif  // JIAMIAODB_WAL_PAYLOAD_H
