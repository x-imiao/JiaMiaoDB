#include "planner.h"
#include "../storage/engine.h"
#include "memory.h"
#include <sstream>
#include <cmath>

// Forward declarations for expression cloning
std::unique_ptr<Expression> clone_expr(const Expression* expr);

AIPlanner::AIPlanner(WorkloadMemory* memory, StorageEngine* storage)
    : memory_(memory), storage_(storage) {}

ExecutionPlan AIPlanner::plan(const UnifiedIntent& intent) {
    ExecutionPlan plan;
    plan.intent_hash = intent.description; // simplified hash

    // 查记忆
    auto history = memory_->find_similar(plan.intent_hash,
        intent.category == IntentCategory::QUERY ? "query" : "mutation",
        intent.tables);
    auto* best = memory_->get_best_strategy(plan.intent_hash);

    if (best && history.size() >= 2) {
        // 从记忆恢复
        plan.pipeline = build_pipeline(intent, intent.statement, best->strategy);
        estimate_cost(plan.pipeline, plan);

        int64_t total_dur = 0;
        for (const auto& h : history) total_dur += h.actual_duration_ms;
        int64_t avg_dur = total_dur / (int64_t)history.size();

        plan.estimated_duration_ms = avg_dur;
        plan.source = "from_memory";
        plan.reasoning = "基于 " + std::to_string(history.size()) +
            " 次历史，流水线 \"" + pipeline_signature(plan.pipeline) +
            "\" 平均 " + std::to_string(avg_dur) + "ms（最优）";
    } else {
        // 新查询
        std::string strategy = choose_initial_strategy(intent, intent.statement);
        plan.pipeline = build_pipeline(intent, intent.statement, strategy);
        estimate_cost(plan.pipeline, plan);
        plan.source = "new_plan";

        std::string table_name = intent.tables.empty() ? "?" : intent.tables[0];
        plan.reasoning = "首次执行 " + intent.description +
            "（表 " + table_name + "），选择策略 \"" + strategy + "\"";
    }

    return plan;
}

std::string AIPlanner::choose_initial_strategy(const UnifiedIntent& intent, const Statement& ast) {
    switch (ast.type) {
        case StatementType::SELECT: {
            if (ast.select && ast.select->where) {
                auto cols = extract_where_columns(ast.select->where.get());
                for (const auto& col : cols) {
                    if (storage_->has_index(ast.select->from_table, col)) {
                        return "index_scan:" + col;
                    }
                }
            }
            return "full_scan";
        }
        case StatementType::INSERT: return "insert";
        case StatementType::UPDATE: return ast.update && ast.update->where ? "update_with_where" : "update";
        case StatementType::DELETE: return ast.delete_stmt && ast.delete_stmt->where ? "delete_with_where" : "delete";
        case StatementType::CREATE_TABLE: return "ddl_create";
        case StatementType::DROP_TABLE: return "ddl_drop";
        default: return "full_scan";
    }
}

Pipeline AIPlanner::build_pipeline(const UnifiedIntent& intent, const Statement& ast, const std::string& strategy) {
    Pipeline pipeline;

    switch (ast.type) {
        case StatementType::SELECT: {
            const auto& s = ast.select;
            if (!s) return pipeline;

            // 数据源
            if (strategy.find("index_scan:") == 0) {
                auto col = strategy.substr(11);
                PipelineOp op;
                op.type = PipelineOpType::INDEX_SCAN;
                op.table = s->from_table;
                op.column = col;
                // Try to extract equality value from WHERE
                std::string eq_val = extract_eq_value(s->where.get(), col);
                if (!eq_val.empty()) {
                    op.index_value = std::string(eq_val);
                } else {
                    op.type = PipelineOpType::SCAN; // fallback
                }
                pipeline.push_back(std::move(op));
            } else {
                PipelineOp op;
                op.type = PipelineOpType::SCAN;
                op.table = s->from_table;
                pipeline.push_back(std::move(op));
            }

            // Filter
            if (s->where) {
                PipelineOp op;
                op.type = PipelineOpType::FILTER;
                // Clone the expression
                op.filter = std::move(clone_expr(s->where.get()));
                pipeline.push_back(std::move(op));
            }

            // Project
            if (!s->columns.empty()) {
                PipelineOp op;
                op.type = PipelineOpType::PROJECT;
                op.project_columns = s->columns;
                pipeline.push_back(std::move(op));
            }
            break;
        }
        case StatementType::INSERT: {
            if (!ast.insert) break;
            PipelineOp op;
            op.type = PipelineOpType::INSERT;
            op.table = ast.insert->table;
            // Convert values to RowSet
            auto schema = storage_->get_schema(ast.insert->table);
            std::vector<std::string> col_names = ast.insert->columns;
            if (col_names.empty() && schema) {
                for (const auto& c : schema->columns) col_names.push_back(c.name);
            }
            for (const auto& vals : ast.insert->values) {
                Row row;
                for (size_t i = 0; i < vals.size() && i < col_names.size(); i++) {
                    row[col_names[i]] = vals[i];
                }
                op.insert_rows.push_back(std::move(row));
            }
            pipeline.push_back(std::move(op));
            break;
        }
        case StatementType::UPDATE: {
            if (!ast.update) break;
            PipelineOp op;
            op.type = PipelineOpType::UPDATE;
            op.table = ast.update->table;
            for (const auto& [col, expr] : ast.update->sets) {
                if (expr->type == ExprType::LITERAL) {
                    op.update_values[col] = expr->literal;
                }
            }
            if (ast.update->where) {
                op.filter = std::move(clone_expr(ast.update->where.get()));
            }
            pipeline.push_back(std::move(op));
            break;
        }
        case StatementType::DELETE: {
            if (!ast.delete_stmt) break;
            PipelineOp op;
            op.type = PipelineOpType::DELETE;
            op.table = ast.delete_stmt->table;
            if (ast.delete_stmt->where) {
                op.filter = std::move(clone_expr(ast.delete_stmt->where.get()));
            }
            pipeline.push_back(std::move(op));
            break;
        }
        case StatementType::CREATE_TABLE: {
            if (!ast.create_table) break;
            PipelineOp op;
            op.type = PipelineOpType::CREATE_TABLE;
            op.table = ast.create_table->name;
            pipeline.push_back(std::move(op));
            break;
        }
        case StatementType::DROP_TABLE: {
            if (!ast.drop_table) break;
            PipelineOp op;
            op.type = PipelineOpType::DROP_TABLE;
            op.table = ast.drop_table->name;
            pipeline.push_back(std::move(op));
            break;
        }
        default:
            break;
    }

    return pipeline;
}

void AIPlanner::estimate_cost(const Pipeline& pipeline, ExecutionPlan& plan) {
    if (pipeline.empty()) return;

    // 看第一个操作推断扫描范围
    const auto& first = pipeline[0];
    std::string table;
    if (first.type == PipelineOpType::SCAN || first.type == PipelineOpType::INDEX_SCAN) {
        table = first.table;
    } else if (first.type == PipelineOpType::INSERT || first.type == PipelineOpType::UPDATE ||
               first.type == PipelineOpType::DELETE || first.type == PipelineOpType::CREATE_TABLE ||
               first.type == PipelineOpType::DROP_TABLE) {
        plan.rows_to_scan = 0;
        plan.rows_to_return = 0;
        plan.estimated_duration_ms = 1;
        plan.memory_estimate = 256;
        return;
    }

    if (table.empty()) return;
    auto* schema = storage_->get_schema(table);
    int64_t row_count = schema ? schema->row_count : 0;

    if (first.type == PipelineOpType::INDEX_SCAN) {
        double selectivity = 0.1; // default
        plan.rows_to_scan = std::max((int64_t)std::ceil(row_count * selectivity), (int64_t)1);
        plan.rows_to_return = plan.rows_to_scan;
        plan.estimated_duration_ms = std::max((int64_t)(plan.rows_to_scan * 0.005), (int64_t)1);
        plan.memory_estimate = plan.rows_to_scan * 64;
    } else {
        plan.rows_to_scan = row_count;
        plan.rows_to_return = row_count;
        plan.estimated_duration_ms = std::max((int64_t)(row_count * 0.01), (int64_t)1);
        plan.memory_estimate = row_count * 64;
    }
}

std::string AIPlanner::pipeline_signature(const Pipeline& pipeline) {
    std::string sig;
    for (size_t i = 0; i < pipeline.size(); i++) {
        if (i > 0) sig += "|";
        switch (pipeline[i].type) {
            case PipelineOpType::SCAN: sig += "scan"; break;
            case PipelineOpType::INDEX_SCAN: sig += "indexScan"; break;
            case PipelineOpType::FILTER: sig += "filter"; break;
            case PipelineOpType::PROJECT: sig += "project"; break;
            case PipelineOpType::INSERT: sig += "insert"; break;
            case PipelineOpType::UPDATE: sig += "update"; break;
            case PipelineOpType::DELETE: sig += "delete"; break;
            case PipelineOpType::CREATE_TABLE: sig += "createTable"; break;
            case PipelineOpType::DROP_TABLE: sig += "dropTable"; break;
            default: sig += "?";
        }
    }
    return sig;
}

std::string AIPlanner::extract_eq_value(const Expression* expr, const std::string& column) {
    if (!expr) return "";
    if (expr->type == ExprType::BINARY_OP && expr->op == "=") {
        if (expr->left && expr->left->type == ExprType::COLUMN_REF && expr->left->column_name == column) {
            if (expr->right && expr->right->type == ExprType::LITERAL) {
                return value_to_string(expr->right->literal);
            }
        }
    }
    if (expr->type == ExprType::BINARY_OP && expr->op == "AND") {
        std::string left = extract_eq_value(expr->left.get(), column);
        if (!left.empty()) return left;
        return extract_eq_value(expr->right.get(), column);
    }
    return "";
}

std::vector<std::string> AIPlanner::extract_where_columns(const Expression* expr) {
    std::vector<std::string> cols;
    if (!expr) return cols;
    if (expr->type == ExprType::COLUMN_REF) {
        cols.push_back(expr->column_name);
    }
    if (expr->type == ExprType::BINARY_OP) {
        auto left = extract_where_columns(expr->left.get());
        cols.insert(cols.end(), left.begin(), left.end());
        auto right = extract_where_columns(expr->right.get());
        cols.insert(cols.end(), right.begin(), right.end());
    }
    if (expr->type == ExprType::UNARY_OP && expr->expr) {
        auto inner = extract_where_columns(expr->expr.get());
        cols.insert(cols.end(), inner.begin(), inner.end());
    }
    return cols;
}

// Helper — need to define clone_expr somewhere accessible
namespace {
    std::unique_ptr<Expression> clone_expr_impl(const Expression* expr) {
        if (!expr) return nullptr;
        auto e = std::make_unique<Expression>();
        e->type = expr->type;
        e->literal = expr->literal;
        e->column_name = expr->column_name;
        e->op = expr->op;
        if (expr->left) e->left = clone_expr_impl(expr->left.get());
        if (expr->right) e->right = clone_expr_impl(expr->right.get());
        if (expr->expr) e->expr = clone_expr_impl(expr->expr.get());
        return e;
    }
}

// External linkage for clone_expr used in build_pipeline
std::unique_ptr<Expression> clone_expr(const Expression* expr) {
    return clone_expr_impl(expr);
}
