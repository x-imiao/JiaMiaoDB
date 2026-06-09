#ifndef JIAMIAODB_LEXER_H
#define JIAMIAODB_LEXER_H

#include <string>
#include <vector>
#include <cctype>
#include <stdexcept>

/* ═══════════════════════════════════════════════════════
   SQL Lexer — 词法分析器
   将 SQL 文本拆分为 Token 流
   支持 PostgreSQL 风格语法
   ═══════════════════════════════════════════════════════ */

enum class TokenType {
    // 关键字
    SELECT, INSERT, INTO, VALUES, UPDATE, DELETE, CREATE, DROP, TABLE,
    FROM, WHERE, SET, AND, OR, NOT, NULL_TOKEN, TRUE_TOKEN, FALSE_TOKEN,
    PRIMARY, KEY, INDEX, ON, DEFAULT, ORDER, BY, ASC, DESC, LIMIT,
    AS, DISTINCT, EXISTS, IN, BETWEEN, LIKE, IS,
    // 事务关键字
    BEGIN, COMMIT, ROLLBACK,
    SAVEPOINT, RELEASE,
    TRANSACTION, ABORT, START, TO,

    // 标识符 / 字面量
    IDENTIFIER,
    INTEGER_LIT,
    FLOAT_LIT,
    STRING_LIT,
    BOOL_LIT,

    // 符号
    SEMICOLON,
    COMMA,
    DOT,
    LPAREN,
    RPAREN,
    STAR,

    // 操作符
    EQ, NEQ, LT, GT, LE, GE,
    PLUS, MINUS, MUL, DIV,

    // 特殊
    END,
    UNKNOWN
};

struct Token {
    TokenType type;
    std::string text;
    int line;
    int col;

    std::string to_string() const;
};

class Lexer {
public:
    explicit Lexer(const std::string& input);

    Token next();
    Token peek();
    std::vector<Token> tokenize();

private:
    std::string input_;
    size_t pos_ = 0;
    int line_ = 1;
    int col_ = 1;
    Token peeked_;
    bool has_peeked_ = false;

    char peek_char() const;
    char advance();
    void skip_whitespace();
    void skip_line_comment();
    void skip_block_comment();

    Token read_identifier();
    Token read_number();
    Token read_string();

    Token make_token(TokenType type, const std::string& text);
    Token make_token(TokenType type, char c);
};

#endif // JIAMIAODB_LEXER_H
