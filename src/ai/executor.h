#ifndef JIAMIAODB_EXECUTOR_H
#define JIAMIAODB_EXECUTOR_H

#include <string>
#include <vector>
#include "../types.h"
#include "planner.h"

class StorageEngine;
class WorkloadMemory;

/* ═══════════════════════════════════════════════════════
   Pipeline Executor — 流水线解释器

   通用解释器，逐个执行 PipelineOp。
   数据在操作符之间流动（scan → filter → project）。
   加新操作符只需在 PipelineOpType 加枚举 + 这里加 case。
   ═══════════════════════════════════════════════════════ */

struct ExecutorResult {
    std::vector<ResultSet> results;
    ExecutionPlan plan;
    UnifiedIntent intent;
};

class AIPlanner;

class PipelineExecutor {
public:
    PipelineExecutor(StorageEngine* storage, WorkloadMemory* memory);

    ExecutorResult execute(UnifiedIntent intent);

private:
    StorageEngine* storage_;
    WorkloadMemory* memory_;
    int record_counter_ = 0;

    ResultSet execute_pipeline(const Pipeline& pipeline);
    ResultSet execute_standalone(const PipelineOp& op);
    void record_execution(const UnifiedIntent& intent, const ExecutionPlan& plan,
                          int64_t duration_ms, const ResultSet& result,
                          const std::vector<std::string>& warnings);
    void update_profiles(const UnifiedIntent& intent);
};

// 表达式求值
Value eval_expression(const Expression* expr, const Row& row);

// 稳定排序
void stable_sort_rows(RowSet& rows, const std::string& column, bool desc);

#endif // JIAMIAODB_EXECUTOR_H
