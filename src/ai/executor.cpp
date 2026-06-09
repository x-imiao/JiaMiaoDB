#include "executor.h"
#include "planner.h"
#include "memory.h"
#include "../storage/engine.h"
#include <functional>
#include <iostream>
#include <algorithm>
#include <cmath>

PipelineExecutor::PipelineExecutor(StorageEngine* storage, WorkloadMemory* memory)
    : storage_(storage), memory_(memory) {}

ExecutorResult PipelineExecutor::execute(UnifiedIntent intent) {
    ExecutorResult result;
    result.intent = std::move(intent);

    // 1. AI Planner 规划
    AIPlanner planner(memory_, storage_);
    result.plan = planner.plan(result.intent);

    // 2. 执行
    int64_t start = time(nullptr) * 1000; // ms
    ResultSet rs;
    try {
        rs = execute_pipeline(result.plan.pipeline);
    } catch (const std::exception& e) {
        rs.message = "执行错误: ";
        rs.message += e.what();
    }
    int64_t duration = (time(nullptr) * 1000) - start;

    rs.duration_ms = duration;
    result.results.push_back(rs);

    // 3. 记录
    record_execution(result.intent, result.plan, duration, rs, {});

    // 4. 更新画像
    update_profiles(result.intent);

    return result;
}

ResultSet PipelineExecutor::execute_pipeline(const Pipeline& pipeline) {
    if (pipeline.empty()) return {{}, {}, 0, "空流水线"};

    const auto& first = pipeline[0];

    // DDL / 写入 — 独立执行
    if (first.type == PipelineOpType::CREATE_TABLE ||
        first.type == PipelineOpType::DROP_TABLE ||
        first.type == PipelineOpType::CREATE_DATABASE ||
        first.type == PipelineOpType::DROP_DATABASE ||
        first.type == PipelineOpType::CREATE_SCHEMA ||
        first.type == PipelineOpType::CREATE_USER ||
        first.type == PipelineOpType::DROP_USER ||
        first.type == PipelineOpType::USE_DATABASE ||
        first.type == PipelineOpType::INSERT ||
        first.type == PipelineOpType::UPDATE ||
        first.type == PipelineOpType::DELETE) {
        return execute_standalone(first);
    }

    // 数据流
    RowSet rows;

    // Read Committed: 每条 SQL 语句获取新快照
    auto& txn = storage_->txn_mgr();
    auto snap = txn.get_snapshot();
    auto xid = txn.get_current_xid();
    auto cid = txn.get_current_command_id();

    for (const auto& op : pipeline) {
        switch (op.type) {
            case PipelineOpType::SCAN: {
                rows = storage_->scan_with_snapshot(op.table, snap, xid, cid);
                break;
            }
            case PipelineOpType::INDEX_SCAN: {
                rows = storage_->index_lookup_with_snapshot(
                    op.table, op.column, op.index_value, snap, xid, cid);
                break;
            }
            case PipelineOpType::FILTER: {
                if (op.filter) {
                    RowSet filtered;
                    for (const auto& row : rows) {
                        if (eval_expression(op.filter.get(), row).index() != 0) { // not nullptr
                            Value v = eval_expression(op.filter.get(), row);
                            bool keep = false;
                            if (auto* b = std::get_if<bool>(&v)) keep = *b;
                            if (keep) filtered.push_back(row);
                        }
                    }
                    rows = std::move(filtered);
                }
                break;
            }
            case PipelineOpType::PROJECT: {
                RowSet projected;
                for (const auto& row : rows) {
                    Row r;
                    for (const auto& col : op.project_columns) {
                        auto it = row.find(col);
                        if (it != row.end()) r[col] = it->second;
                    }
                    projected.push_back(std::move(r));
                }
                rows = std::move(projected);
                break;
            }
            case PipelineOpType::LIMIT: {
                if (op.limit_count > 0 && (int64_t)rows.size() > op.limit_count) {
                    rows.resize(op.limit_count);
                }
                break;
            }
            case PipelineOpType::SORT: {
                stable_sort_rows(rows, op.column, op.sort_desc);
                break;
            }
            default:
                break;
        }
    }

    // 提取列名
    std::vector<std::string> columns;
    if (!rows.empty()) {
        for (const auto& [k, _] : rows[0]) {
            if (!k.empty() && k[0] != '_') columns.push_back(k);
        }
    }

    return {columns, std::move(rows), 0, ""};
}

ResultSet PipelineExecutor::execute_standalone(const PipelineOp& op) {
    switch (op.type) {
        case PipelineOpType::INSERT: {
            int64_t count = 0;
            for (const auto& row : op.insert_rows) {
                storage_->insert_with_txn(op.table, row);
                count++;
            }
            return {{}, {}, count, "插入 " + std::to_string(count) + " 行"};
        }
        case PipelineOpType::UPDATE: {
            auto match_fn = [&](const Row& row) -> bool {
                if (!op.filter) return true;
                Value v = eval_expression(op.filter.get(), row);
                return std::get_if<bool>(&v) && *std::get_if<bool>(&v);
            };
            int64_t count = storage_->update_with_txn(op.table, match_fn, op.update_values);
            return {{}, {}, count, "更新 " + std::to_string(count) + " 行"};
        }
        case PipelineOpType::DELETE: {
            auto match_fn = [&](const Row& row) -> bool {
                if (!op.filter) return true;
                Value v = eval_expression(op.filter.get(), row);
                return std::get_if<bool>(&v) && *std::get_if<bool>(&v);
            };
            int64_t count = storage_->remove_with_txn(op.table, match_fn);
            return {{}, {}, count, "删除 " + std::to_string(count) + " 行"};
        }
        case PipelineOpType::CREATE_TABLE: {
            // Table creation needs schema info, handled via SQL parser
            return {{}, {}, 0, "已创建表 " + op.table};
        }
        case PipelineOpType::DROP_TABLE: {
            storage_->drop_table(op.table);
            return {{}, {}, 0, "已删除表 " + op.table};
        }
        case PipelineOpType::CREATE_DATABASE: {
            storage_->create_database(op.table);
            return {{}, {}, 0, "已创建数据库 " + op.table};
        }
        case PipelineOpType::DROP_DATABASE: {
            storage_->drop_database(op.table);
            return {{}, {}, 0, "已删除数据库 " + op.table};
        }
        case PipelineOpType::CREATE_SCHEMA: {
            storage_->create_schema(storage_->current_db(), op.table);
            return {{}, {}, 0, "已创建模式 " + op.table};
        }
        case PipelineOpType::CREATE_USER: {
            // CREATE_USER needs password from pipeline or handled via SQL
            return {{}, {}, 0, "已创建用户 " + op.table};
        }
        case PipelineOpType::DROP_USER: {
            storage_->drop_user(op.table);
            return {{}, {}, 0, "已删除用户 " + op.table};
        }
        case PipelineOpType::USE_DATABASE: {
            storage_->set_current_db(op.table);
            return {{}, {}, 0, "已切换到数据库 " + op.table};
        }
        default:
            return {{}, {}, 0, "未知操作"};
    }
}

void PipelineExecutor::record_execution(const UnifiedIntent& intent, const ExecutionPlan& plan,
                                         int64_t duration_ms, const ResultSet& result,
                                         const std::vector<std::string>& warnings) {
    // Build pipeline signature
    std::string sig;
    for (size_t i = 0; i < plan.pipeline.size(); i++) {
        if (i > 0) sig += "|";
        switch (plan.pipeline[i].type) {
            case PipelineOpType::SCAN: sig += "scan"; break;
            case PipelineOpType::INDEX_SCAN: sig += "indexScan"; break;
            case PipelineOpType::FILTER: sig += "filter"; break;
            case PipelineOpType::PROJECT: sig += "project"; break;
            case PipelineOpType::INSERT: sig += "insert"; break;
            case PipelineOpType::UPDATE: sig += "update"; break;
            case PipelineOpType::DELETE: sig += "delete"; break;
            case PipelineOpType::CREATE_TABLE: sig += "createTable"; break;
            case PipelineOpType::DROP_TABLE: sig += "dropTable"; break;
            case PipelineOpType::CREATE_DATABASE: sig += "createDb"; break;
            case PipelineOpType::DROP_DATABASE: sig += "dropDb"; break;
            case PipelineOpType::CREATE_SCHEMA: sig += "createSchema"; break;
            case PipelineOpType::CREATE_USER: sig += "createUser"; break;
            case PipelineOpType::DROP_USER: sig += "dropUser"; break;
            case PipelineOpType::USE_DATABASE: sig += "useDb"; break;
            default: sig += "?"; break;
        }
    }

    ExecutionRecord rec;
    rec.id = "exec-" + std::to_string(++record_counter_);
    rec.timestamp = time(nullptr) * 1000;
    rec.intent_hash = plan.intent_hash;
    rec.description = intent.description;
    rec.category = intent.category == IntentCategory::QUERY ? "query" :
                   intent.category == IntentCategory::MUTATION ? "mutation" : "schema";
    rec.tables = intent.tables;
    rec.planned_strategy = sig;
    rec.estimated_duration_ms = plan.estimated_duration_ms;
    rec.actual_duration_ms = duration_ms;
    rec.rows_scanned = plan.rows_to_scan;
    rec.rows_returned = result.rows.size();
    rec.affected_rows = result.affected_rows;
    rec.issues = warnings;
    rec.accuracy = plan.estimated_duration_ms > 0
        ? (double)duration_ms / plan.estimated_duration_ms
        : 1.0;

    memory_->record(rec);

    // 偏差日志
    if (duration_ms > 5 && plan.estimated_duration_ms > 5) {
        if (rec.accuracy > 2.0 || rec.accuracy < 0.3) {
            std::string dir = rec.accuracy > 2.0 ? "慢" : "快";
            std::cout << "[AI 学习] \"" << intent.description
                      << "\" 实际比预估" << dir << "了 "
                      << std::abs(1 - rec.accuracy) * 100 << "%\n";
        }
    }
}

void PipelineExecutor::update_profiles(const UnifiedIntent& intent) {
    // 简化的画像更新 — 后续实现详细统计
}

/* ─── 表达式求值 ─── */

Value eval_expression(const Expression* expr, const Row& row) {
    if (!expr) return nullptr;

    switch (expr->type) {
        case ExprType::LITERAL:
            return expr->literal;

        case ExprType::COLUMN_REF: {
            auto it = row.find(expr->column_name);
            if (it != row.end()) return it->second;
            return nullptr;
        }

        case ExprType::UNARY_OP: {
            if (expr->op == "NOT") {
                Value v = eval_expression(expr->expr.get(), row);
                if (auto* b = std::get_if<bool>(&v)) return !*b;
                return nullptr;
            }
            return nullptr;
        }

        case ExprType::BINARY_OP: {
            Value l = eval_expression(expr->left.get(), row);
            Value r = eval_expression(expr->right.get(), row);

            auto cmp = [&](auto op) -> bool {
                // String comparison
                auto ls = std::get_if<std::string>(&l);
                auto rs = std::get_if<std::string>(&r);
                if (ls && rs) return op(*ls, *rs);

                auto li = std::get_if<int64_t>(&l);
                auto ri = std::get_if<int64_t>(&r);
                if (li && ri) return op(*li, *ri);

                auto ld = std::get_if<double>(&l);
                auto rd = std::get_if<double>(&r);
                if (ld && rd) return op(*ld, *rd);

                // Mixed int/double
                if (li && rd) return op((double)*li, *rd);
                if (ld && ri) return op(*ld, (double)*ri);

                return false;
            };

            if (expr->op == "=") {
                if (l.index() != r.index()) return false;
                return l == r;
            }
            if (expr->op == "!=") return l != r;
            if (expr->op == "<") return cmp(std::less<>());
            if (expr->op == ">") return cmp(std::greater<>());
            if (expr->op == "<=") return cmp(std::less_equal<>());
            if (expr->op == ">=") return cmp(std::greater_equal<>());
            if (expr->op == "AND") return (std::get_if<bool>(&l) && *std::get_if<bool>(&l)) &&
                                         (std::get_if<bool>(&r) && *std::get_if<bool>(&r));
            if (expr->op == "OR") return (std::get_if<bool>(&l) && *std::get_if<bool>(&l)) ||
                                        (std::get_if<bool>(&r) && *std::get_if<bool>(&r));
            if (expr->op == "+") {
                if (auto li = std::get_if<int64_t>(&l)) {
                    if (auto ri = std::get_if<int64_t>(&r)) return *li + *ri;
                    if (auto rd = std::get_if<double>(&r)) return (double)*li + *rd;
                }
                if (auto ld = std::get_if<double>(&l)) {
                    if (auto ri = std::get_if<int64_t>(&r)) return *ld + (double)*ri;
                    if (auto rd = std::get_if<double>(&r)) return *ld + *rd;
                }
                return nullptr;
            }
            if (expr->op == "-") {
                if (auto li = std::get_if<int64_t>(&l)) {
                    if (auto ri = std::get_if<int64_t>(&r)) return *li - *ri;
                    if (auto rd = std::get_if<double>(&r)) return (double)*li - *rd;
                }
                if (auto ld = std::get_if<double>(&l)) {
                    if (auto ri = std::get_if<int64_t>(&r)) return *ld - (double)*ri;
                    if (auto rd = std::get_if<double>(&r)) return *ld - *rd;
                }
                return nullptr;
            }
            if (expr->op == "*") {
                if (auto li = std::get_if<int64_t>(&l)) {
                    if (auto ri = std::get_if<int64_t>(&r)) return *li * *ri;
                    if (auto rd = std::get_if<double>(&r)) return (double)*li * *rd;
                }
                if (auto ld = std::get_if<double>(&l)) {
                    if (auto ri = std::get_if<int64_t>(&r)) return *ld * (double)*ri;
                    if (auto rd = std::get_if<double>(&r)) return *ld * *rd;
                }
                return nullptr;
            }
            if (expr->op == "/") {
                if (auto li = std::get_if<int64_t>(&l)) {
                    if (auto ri = std::get_if<int64_t>(&r)) {
                        if (*ri == 0) return nullptr;
                        return *li / *ri;
                    }
                }
                auto ld = std::get_if<double>(&l);
                auto rd = std::get_if<double>(&r);
                if (ld && rd && *rd != 0) return *ld / *rd;
                return nullptr;
            }
            return nullptr;
        }
    }
    return nullptr;
}

void stable_sort_rows(RowSet& rows, const std::string& column, bool desc) {
    std::stable_sort(rows.begin(), rows.end(), [&](const Row& a, const Row& b) {
        auto it_a = a.find(column);
        auto it_b = b.find(column);
        if (it_a == a.end() && it_b == b.end()) return false;
        if (it_a == a.end()) return desc ? false : true;
        if (it_b == b.end()) return desc ? true : false;

        const Value& va = it_a->second;
        const Value& vb = it_b->second;

        auto cmp = [&]() -> int {
            auto sa = std::get_if<std::string>(&va);
            auto sb = std::get_if<std::string>(&vb);
            if (sa && sb) return sa->compare(*sb);

            auto ia = std::get_if<int64_t>(&va);
            auto ib = std::get_if<int64_t>(&vb);
            if (ia && ib) return *ia < *ib ? -1 : (*ia > *ib ? 1 : 0);

            auto da = std::get_if<double>(&va);
            auto db = std::get_if<double>(&vb);
            if (da && db) return *da < *db ? -1 : (*da > *db ? 1 : 0);

            return 0;
        }();

        return desc ? cmp > 0 : cmp < 0;
    });
}
