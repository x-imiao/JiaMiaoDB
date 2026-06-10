#ifndef JIAMIAODB_WAL_H
#define JIAMIAODB_WAL_H

#include <string>
#include <vector>
#include <fstream>
#include <mutex>
#include "../types.h"
#include "json.h"

using json = Json;

/* ═══════════════════════════════════════════════════════
   Write-Ahead Log — 预写日志

   崩溃恢复的核心：
   1. 每次写操作先追加 WAL，再更新内存
   2. 崩溃后重放 WAL 恢复数据
   3. Checkpoint 定期截断 WAL
   4. JSON Lines 格式，每行一条记录
   ═══════════════════════════════════════════════════════ */

// WAL 记录格式版本: 后续如果格式变更, 可以区分旧/新格式
constexpr int64_t WAL_FORMAT_VERSION = 2;

struct WALRecord {
    int64_t seq;
    int64_t timestamp;
    std::string op;  // create_table, drop_table, insert, update, delete
    std::string table;
    json data;       // 操作相关的数据

    json to_json() const {
        json j;
        j["v"] = WAL_FORMAT_VERSION;  // 格式版本
        j["seq"] = seq;
        j["ts"] = timestamp;
        j["op"] = op;
        j["table"] = table;
        j["data"] = data;
        return j;
    }

    static WALRecord from_json(const json& j) {
        WALRecord rec;
        // 旧格式 (v1) 没有 v 字段, 默认为 1
        rec.seq = j["seq"].get_int();
        rec.timestamp = j.value("ts", (int64_t)0);
        rec.op = j["op"].get_string();
        rec.table = j["table"].get_string();
        rec.data = j["data"];
        return rec;
    }
};

class WriteAheadLog {
public:
    explicit WriteAheadLog(const std::string& path);
    ~WriteAheadLog();

    void open();
    void close();
    void append(const WALRecord& rec);
    void sync();
    // 截断 WAL: 删除 seq <= cutoff_seq 的记录
    // 由 checkpoint 在持久化完成后调用
    void truncate(int64_t cutoff_seq);

    int64_t current_seq() const { return seq_; }
    void set_seq(int64_t seq) { seq_ = seq; }

    std::vector<WALRecord> replay(int64_t from_seq);

private:
    std::string path_;
    int64_t seq_ = 0;
    std::ofstream file_;
    std::mutex mutex_;
};

#endif // JIAMIAODB_WAL_H
