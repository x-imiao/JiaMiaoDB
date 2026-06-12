#ifndef JIAMIAODB_CHECKPOINT_H
#define JIAMIAODB_CHECKPOINT_H

#include <string>
#include <vector>
#include <cstdint>
#include "../types.h"  // IndexInfo, ColumnDef, TableSchema, Row
#include "common/json.h"

using json = Json;

/* ═══════════════════════════════════════════════════════
   Checkpoint — 定期快照

   写入路径: 内存状态 → (binary) checkpoint 文件
   恢复路径: 加载最新 Checkpoint → 重放 WAL

   字段 (O-2 起改为强类型, JSON 形式仅保留为 clog/catalog 序列化):
   - tables           : vector<TableSchema>           表 schema
   - table_rows       : map<table, vector<(rowid, Row)>> 行数据
   - row_id_counters  : map<table, int64>             自增 rowid 计数器
   - indexes_data     : map<table, vector<IndexInfo>> 索引
   - next_xid         : uint32                        事务 XID 续点
   - clog_entries     : json                          CLog 状态 (O-3 替换)
   - catalog_data     : json                          Catalog 状态 (O-4 替换)

   O-2 改造要点:
   1. table_data / indexes 改为强类型 table_rows / indexes_data
   2. to_json / from_json 仍保留 (legacy 兼容, 由 CheckpointManager::load JSON fallback 调用)
   3. 新增 to_binary / from_binary, CheckpointManager::save 写二进制
   4. 文件头 8B magic "JMDB\x01CKP\x01\x00" — 不匹配则回退 JSON 解析
   ═══════════════════════════════════════════════════════ */

struct Checkpoint {
    int64_t version = 1;
    int64_t last_seq = 0;
    int64_t timestamp = 0;
    std::vector<TableSchema> tables;
    // O-2: 强类型行数据, 取代旧的 map<string, json>
    std::map<std::string, std::vector<std::pair<int64_t, Row>>> table_rows;
    std::map<std::string, int64_t> row_id_counters;
    std::map<std::string, std::vector<IndexInfo>> indexes_data;
    uint32_t next_xid = 3;          // FirstNormalTransactionId
    json clog_entries;              // CLog 序列化数据 (legacy JSON 兼容)
    json catalog_data;              // Catalog 序列化数据 (legacy JSON 兼容)
    std::string catalog_bytes;      // O-3 起: Catalog 二进制 (catalog_encode 输出)
                                    //   v3 写 binary; v2 仍用 catalog_data (json)
    std::string clog_bytes;         // O-4 起: CLog 二进制 (clog_encode 输出)
                                    //   v3 写 binary; 旧 v3 文件可能仍是 JSON 字符串

    // JSON 形式 (legacy 兼容, 仅供 CheckpointManager::load fallback 使用)
    json to_json() const;
    static Checkpoint from_json(const json& j);

    // 二进制形式 (主路径, O-2 起)
    // 序列化到 bytes (不含 magic, magic 由 CheckpointManager::save 负责)
    std::string to_binary() const;
    // 反序列化; 失败返 false
    //   out_clog_json / out_catalog_json 携带嵌入的 JSON, 供 caller 走现有 from_json
    static bool from_binary(const std::string& bytes,
                            Checkpoint* ckp,
                            std::string* out_clog_json,
                            std::string* out_catalog_json);
};

class CheckpointManager {
public:
    explicit CheckpointManager(const std::string& dir);

    void save(const Checkpoint& ckp);
    Checkpoint load();
    int64_t get_last_seq();
    std::string checkpoint_path() const { return dir_ + "/checkpoint.bin"; }

private:
    std::string dir_;
};

#endif // JIAMIAODB_CHECKPOINT_H
