#ifndef JIAMIAODB_PLANNER_H
#define JIAMIAODB_PLANNER_H

#include <string>
#include <vector>
#include <memory>
#include "../types.h"

class StorageEngine;
class WorkloadMemory;

/* ═══════════════════════════════════════════════════════
   AI Planner — AI 规划器

   1. 查 WorkloadMemory 有没有相似查询的历史
   2. 有 → 选历史上最快的流水线
   3. 没有 → 生成新流水线
   ═══════════════════════════════════════════════════════ */

struct ExecutionPlan {
    std::string intent_hash;
    Pipeline pipeline;
    int64_t rows_to_scan = 0;
    int64_t rows_to_return = 0;
    int64_t estimated_duration_ms = 0;
    int64_t memory_estimate = 0;
    std::string reasoning;
    std::string source; // "from_memory", "new_plan"
};

class AIPlanner {
public:
    AIPlanner(WorkloadMemory* memory, StorageEngine* storage);

    ExecutionPlan plan(const UnifiedIntent& intent);

private:
    WorkloadMemory* memory_;
    StorageEngine* storage_;

    std::string choose_initial_strategy(const UnifiedIntent& intent, const Statement& ast);
    Pipeline build_pipeline(const UnifiedIntent& intent, const Statement& ast, const std::string& strategy);
    void estimate_cost(const Pipeline& pipeline, ExecutionPlan& plan);
    std::string pipeline_signature(const Pipeline& pipeline);

    // 辅助
    std::string extract_eq_value(const Expression* expr, const std::string& column);
    std::vector<std::string> extract_where_columns(const Expression* expr);
    std::string normalize_stmt(const Statement& stmt);
};

#endif // JIAMIAODB_PLANNER_H
