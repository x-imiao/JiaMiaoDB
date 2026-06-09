#ifndef JIAMIAODB_PARSER_H
#define JIAMIAODB_PARSER_H

#include "lexer.h"
#include "../types.h"
#include <memory>
#include <vector>

/* ═══════════════════════════════════════════════════════
   SQL Parser — 递归下降语法分析器
   支持 PostgreSQL 风格 SQL 子集
   ═══════════════════════════════════════════════════════ */

class ParseError : public std::runtime_error {
public:
    explicit ParseError(const std::string& msg) : std::runtime_error(msg) {}
};

class Parser {
public:
    explicit Parser(const std::vector<Token>& tokens);

    Statement parse_statement();
    std::vector<Statement> parse_all();

private:
    std::vector<Token> tokens_;
    size_t pos_ = 0;

    const Token& peek() const;
    const Token& previous() const;
    Token advance();
    Token expect(TokenType type, const std::string& error_msg);
    bool match(TokenType type);
    bool check(TokenType type) const;
    bool is_at_end() const;

    // 语法规则
    Statement parse_select();
    Statement parse_insert();
    Statement parse_update();
    Statement parse_delete();
    Statement parse_create();
    Statement parse_create_table();
    Statement parse_drop_table();
    Statement parse_create_index();
    Statement parse_create_database();
    Statement parse_create_schema();
    Statement parse_create_user();
    Statement parse_drop_database();
    Statement parse_drop_user();
    Statement parse_use();
    Statement parse_show();
    Statement parse_begin();
    Statement parse_commit();
    Statement parse_rollback();

    // 表达式
    ExprPtr parse_expression();
    ExprPtr parse_or();
    ExprPtr parse_and();
    ExprPtr parse_comparison();
    ExprPtr parse_addition();
    ExprPtr parse_multiplication();
    ExprPtr parse_unary();
    ExprPtr parse_primary();

    // 辅助
    ColumnDef parse_column_def();
    std::vector<std::string> parse_ident_list();
    DataType parse_type();
};

#endif // JIAMIAODB_PARSER_H
