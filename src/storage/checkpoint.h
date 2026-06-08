#ifndef JIAMIAODB_CHECKPOINT_H
#define JIAMIAODB_CHECKPOINT_H

#include <string>
#include <vector>
#include "../types.h"
#include "json.h"

using json = Json;

/* ═══════════════════════════════════════════════════════
   Checkpoint — 定期快照

   将内存状态持久化到磁盘，标记 WAL 截断点。
   恢复时：加载最新 Checkpoint → 重放 WAL。
   ═══════════════════════════════════════════════════════ */

struct Checkpoint {
    int64_t version = 1;
    int64_t last_seq = 0;
    int64_t timestamp;
    std::vector<TableSchema> tables;
    // table_name → rows (rows serialized as JSON array)
    std::map<std::string, json> table_data;
    std::map<std::string, int64_t> row_id_counters;
    std::map<std::string, json> indexes;

    json to_json() const;
    static Checkpoint from_json(const json& j);
};

class CheckpointManager {
public:
    explicit CheckpointManager(const std::string& dir);

    void save(const Checkpoint& ckp);
    Checkpoint load();
    int64_t get_last_seq();
    std::string checkpoint_path() const { return dir_ + "/checkpoint.json"; }

private:
    std::string dir_;
};

#endif // JIAMIAODB_CHECKPOINT_H
