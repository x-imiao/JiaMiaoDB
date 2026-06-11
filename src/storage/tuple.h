/* ══════════════════════════════════════════════════════════════════════
   tuple.h — Phase 1 LSM 重构：二进制行格式 (Tuple)

   目的: 把 JMDB 的"JSON 内存 Row"换成"二进制 Tuple + MemTable"。
   本文件定义:
     - TupleHeader   32 字节定长 MVCC 元数据 (packed)
     - Tuple         header + 二进制 payload (业务列)
     - InternalKey   MemTable 排序键 (user_key ASC, seq DESC)
     - BinaryRowCodec  Row ↔ bytes 双向转换
     - make_user_key 构造 user_key

   Phase 2 计划: 将 Tuple::payload 换成 Arena 指针; 升级 BinaryRowCodec
   为带 CRC 校验的格式. 本阶段所有类型为 stable 公开契约.

   不动: TransactionId (uint32_t) 来自 transaction.h, 锁 / MVCC 语义保持.
   ══════════════════════════════════════════════════════════════════════ */

#pragma once

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "../types.h"     // Row, Value
#include "transaction.h"  // TransactionId, Snapshot, CLog, check_tuple_visibility

namespace jiamiao {

// ── 32 字节定长 MVCC header, packed (避免编译器填充) ──
struct __attribute__((packed)) TupleHeader {
    uint64_t row_id;        // _rowid, 单调递增
    uint32_t xmin;          // 插入事务 (0=Invalid, 2=Frozen)
    uint32_t xmax;          // 删除事务 (0=未删)
    uint32_t cid;           // command id (在插入事务内)
    uint16_t schema_hash;   // FNV-1a(qualified_table_name), 防表重名错位
    uint16_t flags;         // bit0: tombstone, bit1: xmin frozen
    uint32_t payload_len;   // 后续 payload 字节数
    uint32_t crc32;         // header 自身 CRC (Phase 2 启用, 本阶段写 0)
};
static_assert(sizeof(TupleHeader) == 32, "TupleHeader must be 32B");

// ── Tuple: 物理存储单元 ──
struct Tuple {
    TupleHeader hdr;
    std::vector<uint8_t> payload;  // BinaryRowCodec::encode(row)

    // 计算 header 的 16-bit FNV-1a hash
    static uint16_t fnv1a_16(const std::string& s);
};

// ── InternalKey: MemTable 排序键 ──
//   user_key 升序, 同一 user_key 内 seq 降序 (最新版本在前, RocksDB 风格)
struct InternalKey {
    std::string user_key;
    uint64_t    seq = 0;

    bool operator<(const InternalKey& o) const {
        if (user_key != o.user_key) return user_key < o.user_key;
        return seq > o.seq;
    }
    bool operator==(const InternalKey& o) const {
        return user_key == o.user_key && seq == o.seq;
    }
};

// 构造 user_key: "<table_qualified>\x1f<row_id_8B_BE>"
inline std::string make_user_key(const std::string& table_qualified, int64_t row_id) {
    std::string k;
    k.reserve(table_qualified.size() + 9);
    k.append(table_qualified);
    k.push_back('\x1f');
    // row_id 用二进制大端, 保证字典序与数值序一致
    uint64_t v = static_cast<uint64_t>(row_id);
    for (int i = 7; i >= 0; --i) {
        k.push_back(static_cast<char>((v >> (i * 8)) & 0xff));
    }
    return k;
}

// ── BinaryRowCodec: Row ↔ bytes ──
//
//   Payload 布局 (小端, 紧凑):
//     u16 ncols
//     for each col:
//       u16  name_len
//       char name[name_len]      UTF-8
//       u8   type_tag            0=null, 1=i64, 2=f64, 3=bool, 4=string
//       <type-specific bytes>    i64:8B  f64:8B  bool:1B  string:u32 len + bytes
//
//   不存系统列 _rowid/_xmin/_xmax/_cid — 这些在 TupleHeader 里.
class BinaryRowCodec {
public:
    static std::vector<uint8_t> encode(const Row& row);
    static Row                  decode(const uint8_t* data, size_t len);

    // 投影: Tuple → Row (含系统列 _rowid/_xmin/_xmax/_cid)
    // 供 check_tuple_visibility / executor / tests 使用.
    static Row tuple_to_row(const Tuple& t);

    // 投影: Row + TupleHeader → Tuple
    static Tuple row_to_tuple(const Row& row, const TupleHeader& hdr);
};

}  // namespace jiamiao
