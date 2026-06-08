#ifndef JIAMIAODB_TYPES_H
#define JIAMIAODB_TYPES_H

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <variant>
#include <optional>
#include <cstdint>

/* ═══════════════════════════════════════════════════════
   Core Types — 所有模块共享的类型定义
   数据结构参考 PostgreSQL 风格，保持扩展性
   ═══════════════════════════════════════════════════════ */

// ── 基础数据类型 ──

enum class DataType : uint8_t {
    INTEGER = 0,
    BIGINT,
    FLOAT,
    DOUBLE,
    TEXT,
    BOOLEAN,
    DATE,
    TIMESTAMP,
    UNKNOWN
};

// 运行时值
using Value = std::variant<std::nullptr_t, int64_t, double, std::string, bool>;

// 行 = 列名 → 值
using Row = std::map<std::string, Value>;
// 行集
using RowSet = std::vector<Row>;

// ── 辅助函数 ──

inline std::string value_to_string(const Value& v) {
    struct Visitor {
        std::string operator()(std::nullptr_t) { return "NULL"; }
        std::string operator()(int64_t i) { return std::to_string(i); }
        std::string operator()(double d) { return std::to_string(d); }
        std::string operator()(const std::string& s) { return "'" + s + "'"; }
        std::string operator()(bool b) { return b ? "true" : "false"; }
    };
    return std::visit(Visitor{}, v);
}

inline std::string data_type_name(DataType t) {
    switch (t) {
        case DataType::INTEGER: return "INTEGER";
        case DataType::BIGINT: return "BIGINT";
        case DataType::FLOAT: return "FLOAT";
        case DataType::DOUBLE: return "DOUBLE";
        case DataType::TEXT: return "TEXT";
        case DataType::BOOLEAN: return "BOOLEAN";
        case DataType::DATE: return "DATE";
        case DataType::TIMESTAMP: return "TIMESTAMP";
        default: return "UNKNOWN";
    }
}

inline DataType data_type_from_name(const std::string& s) {
    auto up = s;
    for (auto& c : up) c = toupper(c);
    if (up == "INT" || up == "INTEGER") return DataType::INTEGER;
    if (up == "BIGINT") return DataType::BIGINT;
    if (up == "FLOAT") return DataType::FLOAT;
    if (up == "DOUBLE") return DataType::DOUBLE;
    if (up == "TEXT" || up == "VARCHAR" || up == "STRING") return DataType::TEXT;
    if (up == "BOOLEAN" || up == "BOOL") return DataType::BOOLEAN;
    if (up == "DATE") return DataType::DATE;
    if (up == "TIMESTAMP") return DataType::TIMESTAMP;
    return DataType::TEXT; // default
}

// ── 列定义 ──

struct ColumnDef {
    std::string name;
    DataType type = DataType::TEXT;
    bool nullable = true;
    bool primary_key = false;
    std::optional<Value> default_value;
};

// ── 表 Schema ──

struct TableSchema {
    std::string name;
    std::vector<ColumnDef> columns;
    int64_t row_count = 0;
};

// ── 表达式 (AST) ──

struct Expression;
using ExprPtr = std::unique_ptr<Expression>;

enum class ExprType {
    LITERAL,
    COLUMN_REF,
    BINARY_OP,
    UNARY_OP
};

struct Expression {
    ExprType type;
    Value literal;              // for LITERAL
    std::string column_name;    // for COLUMN_REF
    std::string op;             // for BINARY_OP / UNARY_OP
    ExprPtr left;               // for BINARY_OP
    ExprPtr right;              // for BINARY_OP
    ExprPtr expr;               // for UNARY_OP

    static ExprPtr make_literal(const Value& v) {
        auto e = std::make_unique<Expression>();
        e->type = ExprType::LITERAL;
        e->literal = v;
        return e;
    }

    static ExprPtr make_column_ref(const std::string& name) {
        auto e = std::make_unique<Expression>();
        e->type = ExprType::COLUMN_REF;
        e->column_name = name;
        return e;
    }

    static ExprPtr make_binary_op(const std::string& op, ExprPtr l, ExprPtr r) {
        auto e = std::make_unique<Expression>();
        e->type = ExprType::BINARY_OP;
        e->op = op;
        e->left = std::move(l);
        e->right = std::move(r);
        return e;
    }

    static ExprPtr make_unary_op(const std::string& op, ExprPtr inner) {
        auto e = std::make_unique<Expression>();
        e->type = ExprType::UNARY_OP;
        e->op = op;
        e->expr = std::move(inner);
        return e;
    }
};

// ── clone_expr declaration ──
ExprPtr clone_expr(const Expression* expr);

// ── SQL 语句 AST ──

enum class StatementType {
    SELECT,
    INSERT,
    UPDATE,
    DELETE,
    CREATE_TABLE,
    DROP_TABLE,
    CREATE_INDEX,
    UNKNOWN
};

struct SelectStmt {
    std::vector<std::string> columns; // empty = *
    std::string from_table;
    ExprPtr where;
};

struct InsertStmt {
    std::string table;
    std::vector<std::string> columns; // empty = all
    std::vector<std::vector<Value>> values; // multi-row
};

struct UpdateStmt {
    std::string table;
    std::map<std::string, ExprPtr> sets; // column → expr
    ExprPtr where;
};

struct DeleteStmt {
    std::string table;
    ExprPtr where;
};

struct CreateTableStmt {
    std::string name;
    std::vector<ColumnDef> columns;
};

struct DropTableStmt {
    std::string name;
};

struct CreateIndexStmt {
    std::string table;
    std::string column;
    std::string index_name;
};

struct Statement {
    StatementType type = StatementType::UNKNOWN;
    std::unique_ptr<SelectStmt> select;
    std::unique_ptr<InsertStmt> insert;
    std::unique_ptr<UpdateStmt> update;
    std::unique_ptr<DeleteStmt> delete_stmt;
    std::unique_ptr<CreateTableStmt> create_table;
    std::unique_ptr<DropTableStmt> drop_table;
    std::unique_ptr<CreateIndexStmt> create_index;
};

// ── UnifiedIntent ──

enum class IntentCategory { QUERY, MUTATION, SCHEMA, TASK, CONVERSATION };

struct UnifiedIntent {
    IntentCategory category = IntentCategory::QUERY;
    std::string raw_input;
    std::vector<std::string> tables;
    std::string description;
    Statement statement;
    bool is_write = false;
};

// ── Pipeline Operator ──

enum class PipelineOpType {
    SCAN,
    INDEX_SCAN,
    FILTER,
    PROJECT,
    LIMIT,
    SORT,
    INSERT,
    UPDATE,
    DELETE,
    CREATE_TABLE,
    DROP_TABLE,
    CREATE_INDEX
};

struct PipelineOp {
    PipelineOpType type;
    std::string table;
    std::string column;        // for INDEX_SCAN, SORT
    Value index_value;         // for INDEX_SCAN
    ExprPtr filter;            // for FILTER, UPDATE, DELETE
    std::vector<std::string> project_columns; // for PROJECT
    int64_t limit_count = 0;   // for LIMIT
    bool sort_desc = false;    // for SORT
    std::vector<Row> insert_rows; // for INSERT
    std::map<std::string, Value> update_values; // for UPDATE
};

using Pipeline = std::vector<PipelineOp>;

// ── Result ──

struct ResultSet {
    std::vector<std::string> columns;
    RowSet rows;
    int64_t affected_rows = 0;
    std::string message;
    int64_t duration_ms = 0;
};

// ── ExecuteResult ──

struct ExecutionRecord {
    std::string id;
    int64_t timestamp;
    std::string intent_hash;
    std::string description;
    std::string category;
    std::vector<std::string> tables;
    std::string planned_strategy;
    int64_t estimated_duration_ms;
    int64_t actual_duration_ms;
    int64_t rows_scanned;
    int64_t rows_returned;
    int64_t affected_rows;
    std::vector<std::string> issues;
    double accuracy;
};

#endif // JIAMIAODB_TYPES_H
