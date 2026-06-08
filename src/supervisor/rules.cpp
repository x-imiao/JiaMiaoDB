#include "rules.h"
#include "../storage/engine.h"
#include <algorithm>

Supervisor::Supervisor(StorageEngine* storage) : storage_(storage) {}

VerificationResult Supervisor::verify(const UnifiedIntent& intent, const ExecutionPlan& plan) {
    VerificationResult result;

    if (intent.category != IntentCategory::SCHEMA) {
        check_schema_exists(intent, result.errors);
    }

    check_write_safety(intent, plan, result.errors, result.warnings);
    check_delete_safety(intent, result.errors);
    check_resource_budget(plan, result.warnings);
    check_index_usage(plan, result.warnings);

    result.passed = result.errors.empty();
    return result;
}

void Supervisor::check_schema_exists(const UnifiedIntent& intent, std::vector<std::string>& errors) {
    for (const auto& table : intent.tables) {
        if (!storage_->get_schema(table)) {
            errors.push_back("表 \"" + table + "\" 不存在");
        }
    }
}

void Supervisor::check_write_safety(const UnifiedIntent& intent, const ExecutionPlan& plan,
                                     std::vector<std::string>& errors, std::vector<std::string>& warnings) {
    if (!intent.is_write) return;

    for (const auto& op : plan.pipeline) {
        if ((op.type == PipelineOpType::UPDATE || op.type == PipelineOpType::DELETE) && !op.filter) {
            const std::string& table = op.table;
            auto* schema = storage_->get_schema(table);
            int64_t row_count = schema ? schema->row_count : 0;
            if (row_count > 100) {
                std::string op_name = op.type == PipelineOpType::UPDATE ? "更新" : "删除";
                errors.push_back("全表 " + op_name + " 操作涉及 " + std::to_string(row_count) +
                                 " 行，需要 WHERE 条件确认");
            }
        }
    }
}

void Supervisor::check_delete_safety(const UnifiedIntent& intent, std::vector<std::string>& errors) {
    std::string upper = intent.raw_input;
    for (auto& c : upper) c = toupper(c);
    if (upper.find("DELETE") != std::string::npos && upper.find("WHERE") == std::string::npos) {
        if (!intent.tables.empty()) {
            auto* schema = storage_->get_schema(intent.tables[0]);
            int64_t row_count = schema ? schema->row_count : 0;
            if (row_count > 0) {
                errors.push_back("DELETE 语句没有 WHERE 条件，将删除表 \"" +
                                 intent.tables[0] + "\" 中全部 " + std::to_string(row_count) + " 行数据");
            }
        }
    }
}

void Supervisor::check_resource_budget(const ExecutionPlan& plan, std::vector<std::string>& warnings) {
    if (plan.rows_to_scan > 10000) {
        warnings.push_back("预计扫描 " + std::to_string(plan.rows_to_scan) + " 行，如果表很大可能会慢");
    }
    if (plan.estimated_duration_ms > 5000) {
        warnings.push_back("预计执行 " + std::to_string(plan.estimated_duration_ms) + "ms，建议检查是否有索引");
    }
}

void Supervisor::check_index_usage(const ExecutionPlan& plan, std::vector<std::string>& warnings) {
    if (plan.pipeline.empty()) return;
    if (plan.pipeline[0].type != PipelineOpType::SCAN) return;

    const std::string& table = plan.pipeline[0].table;
    auto* schema = storage_->get_schema(table);
    if (!schema) return;

    for (const auto& op : plan.pipeline) {
        if (op.type == PipelineOpType::FILTER && op.filter) {
            // Extract column refs and check indexes
            std::vector<std::string> cols;
            // Simple extraction from expression
            std::string expr_str; // simplified
            for (const auto& col : schema->columns) {
                if (storage_->has_index(table, col.name)) {
                    warnings.push_back("表 \"" + table + "\" 的 \"" + col.name +
                                       "\" 列有索引，但 Planner 选择了全表扫描");
                }
            }
        }
    }
}
