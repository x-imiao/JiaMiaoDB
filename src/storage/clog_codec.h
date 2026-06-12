/* ═══════════════════════════════════════════════════════════════════════
   clog_codec.h — CLog 二进制 codec (O-4)

   CLog 在 Checkpoint 内的持久化段, 把
       [{ "xid": 10, "status": 1 }, ...]   ← 旧 JSON 数组
   压成
       [4B magic "CLGB"][u32 fmt=1][u32 count][(u32 xid, u8 status) × N]

   每条 5 字节, 1M 事务 ~5MB (vs JSON ~80MB).

   Magic 检测: 旧 v3 Checkpoint 嵌入的是 JSON 数组 (无 magic), 新 v3 嵌入二进制
   (开头 4B "CLGB"). reader 先 peek 4B, 命中走 binary, 否则当 JSON 字符串回退.
   ═══════════════════════════════════════════════════════════════════════ */

#ifndef JIAMIAODB_CLOG_CODEC_H
#define JIAMIAODB_CLOG_CODEC_H

#include <cstdint>
#include <string>

#include "transaction.h"  // CLog, TransactionId, TransactionStatus

namespace jiamiao {

// 4B magic: CLoG Binary
inline constexpr char kCLogMagic[5] = "CLGB";
inline constexpr size_t kCLogMagicLen = 4;
inline constexpr uint32_t kCLogFormatVersion = 1;

// 序列化 CLog → 二进制 bytes (含 4B magic + u32 fmt)
//   布局 (小端):
//     [4B  "CLGB"]
//     [u32 format_version = 1]
//     [u32 entry_count]
//     for each:
//       [u32 xid]
//       [u8  status]
// 空 CLog 返 9B (magic + fmt + count=0)
std::string clog_encode(const CLog& clog);

// 反序列化 → CLog. 失败返 false (truncation / 未知 fmt)
//   期望 bytes 已通过 magic 嗅探确认是 binary (caller 负责嗅探)
bool clog_decode(const std::string& bytes, CLog* clog);

}  // namespace jiamiao

#endif  // JIAMIAODB_CLOG_CODEC_H
