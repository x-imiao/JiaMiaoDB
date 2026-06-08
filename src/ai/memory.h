#ifndef JIAMIAODB_MEMORY_H
#define JIAMIAODB_MEMORY_H

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include "../types.h"

/* ═══════════════════════════════════════════════════════
   WorkloadMemory — 工作负载记忆

   记录每次执行的效果，AI 从中学习。
   传统优化器"猜"统计信息，JiamiaoDB "记"执行历史。
   ═══════════════════════════════════════════════════════ */

struct Pattern {
    std::string id;
    std::string type; // periodic_job, frequent_query, hot_data, data_skew
    std::string description;
    std::vector<std::string> related_intent_hashes;
    std::string suggested_strategy;
    std::string suggestion_reason;
    double confidence = 0.0;
    int64_t first_seen;
    int64_t last_updated;
};

struct StrategyStat {
    std::string strategy;
    int64_t total_duration = 0;
    int count = 0;
    double avg_duration() const { return count > 0 ? (double)total_duration / count : 0; }
};

class WorkloadMemory {
public:
    WorkloadMemory();

    void record(const ExecutionRecord& exec);
    void save(const std::string& path);
    void load(const std::string& path);

    // 查询
    std::vector<ExecutionRecord> find_similar(const std::string& intent_hash, const std::string& category,
                                               const std::vector<std::string>& tables);
    StrategyStat* get_best_strategy(const std::string& intent_hash);
    std::vector<Pattern> get_patterns();

private:
    std::vector<ExecutionRecord> records_;
    std::vector<Pattern> patterns_;
    std::mutex mutex_;

    void learn_patterns(const ExecutionRecord& rec);
    void detect_periodic_job(const ExecutionRecord& rec);
    void detect_frequent_query(const ExecutionRecord& rec);
    void detect_strategy_deviation(const ExecutionRecord& rec);
};

#endif // JIAMIAODB_MEMORY_H
