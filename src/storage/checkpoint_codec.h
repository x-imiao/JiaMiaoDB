/* ═══════════════════════════════════════════════════════════════════════
   checkpoint_codec.h — Checkpoint 二进制 codec (O-2)

   镜像 wal_payload 的设计: 显式 per-section 序列化, 没有 JSON 词法/转义成本.

   顶层 Checkpoint 文件布局:
     [8B  magic "JMDB\x01CKP\x01\x00"]    (CheckpointManager::save 负责写 magic)
     [binary payload]                       (本文件 encode/decode)

   Binary payload 布局 (小端):
     u32  format_version = 2
     i64  version
     i64  last_seq
     i64  timestamp
     u32  next_xid

     ── tables ──
     u32  table_count
     for each table:
       u16 nlen, name
       u32 col_count
       for each col: u16 nlen, name; u8 type_id; u8 nullable; u8 primary_key
       i64 row_count

     ── table_data (rows) ──
     u32  table_count
     for each table:
       u16 nlen, name
       u32 row_count
       for each row: i64 rowid, Row row    (Row 复用 wal_encode_row)

     ── row_id_counters ──
     u32  entry_count
     for each: u16 klen, key, i64 value

     ── indexes ──
     u32  table_count
     for each table:
       u16 nlen, name
       u32 index_count
       for each index:
         u16 nlen, column
         u32 entry_count
         for each entry: u16 klen, key; u32 idx_count; i64 × idx_count

     ── clog_entries (still JSON, O-3 将替换) ──
     u32  json_len, json_bytes

     ── catalog_data (still JSON, O-4 将替换) ──
     u32  json_len, json_bytes
   ═══════════════════════════════════════════════════════════════════════ */

#ifndef JIAMIAODB_CHECKPOINT_CODEC_H
#define JIAMIAODB_CHECKPOINT_CODEC_H

#include <cstdint>
#include <string>

#include "checkpoint.h"

namespace jiamiao {

// Magic 8B: "JMDB" + \x01 (binary) + "CKP" + \x01 \x00 (format version 2)
//   (末尾 \x00 不计入 magic 长度, 仅为字符串终止符)
//   注: v3 起 catalog 段改为二进制 (catalog_encode); v2 仍以 JSON 字符串嵌入.
//   load 时按 format_version 自动选 v2 / v3 decoder.
inline constexpr char kCheckpointMagic[9] = "JMDB\x01CKP\x01";
inline constexpr size_t    kCheckpointMagicLen = 8;
inline constexpr uint32_t kCheckpointFormatVersionV2 = 2;
inline constexpr uint32_t kCheckpointFormatVersionV3 = 3;
inline constexpr uint32_t kCheckpointFormatVersion = kCheckpointFormatVersionV3;  // 当前

// 序列化 Checkpoint → 二进制 bytes (不含 magic)
//   O-2 范围: tables / table_data / row_id_counters / indexes / next_xid 二进制化
//   clog_entries + catalog_data 仍以原始 JSON 字符串嵌入 (O-3/O-4 替换)
std::string checkpoint_encode(const Checkpoint& ckp);

// 反序列化 → Checkpoint; 失败返 false (payload 截断 / 字段越界)
//   out_clog_json / out_catalog_json 携带嵌入的 JSON, 供 caller 直接走现有 from_json
bool checkpoint_decode(const std::string& bytes,
                       Checkpoint* ckp,
                       std::string* out_clog_json,
                       std::string* out_catalog_json);

}  // namespace jiamiao

#endif  // JIAMIAODB_CHECKPOINT_CODEC_H
