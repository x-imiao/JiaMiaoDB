/* ═══════════════════════════════════════════════════════════════════════
   catalog_codec.h — Catalog 二进制 codec (O-3)

   Binary layout (小端):
     u32  format_version = 1
     u16  cur_db_len, current_db

     u32  db_count
     for each db:
       u16  name_len, name
       u32  schema_count
       for each schema: u16 name_len, name

     u32  user_count
     for each user:
       u16  name_len, name
       u16  pw_len, password_hash      (salted SHA-256 hex, 64 chars)
       u16  salt_len, salt             (hex, 32 chars)

   注: passwords 不入 JSON/WAL (安全), 但 Catalog 自己的持久化包含
   用户的 password_hash + salt (用于登录验证). 这是 Catalog 独有的.

   Snapshot 通过 Catalog::internal_snapshot() 拿, 不暴露内部 map.
   ═══════════════════════════════════════════════════════════════════════ */

#ifndef JIAMIAODB_CATALOG_CODEC_H
#define JIAMIAODB_CATALOG_CODEC_H

#include <cstdint>
#include <string>

#include "catalog.h"

namespace jiamiao {

inline constexpr uint32_t kCatalogFormatVersion = 1;

// 序列化 → 二进制 bytes
std::string catalog_encode(const Catalog& cat);

// 反序列化 → Catalog; 失败返 false
bool catalog_decode(const std::string& bytes, Catalog* cat);

}  // namespace jiamiao

#endif  // JIAMIAODB_CATALOG_CODEC_H
