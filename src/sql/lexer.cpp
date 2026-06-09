#include "lexer.h"
#include <unordered_map>

std::string Token::to_string() const {
    static const std::unordered_map<TokenType, std::string> names = {
        {TokenType::SELECT, "SELECT"}, {TokenType::INSERT, "INSERT"},
        {TokenType::INTO, "INTO"}, {TokenType::VALUES, "VALUES"},
        {TokenType::UPDATE, "UPDATE"}, {TokenType::DELETE, "DELETE"},
        {TokenType::CREATE, "CREATE"}, {TokenType::DROP, "DROP"},
        {TokenType::TABLE, "TABLE"}, {TokenType::FROM, "FROM"},
        {TokenType::WHERE, "WHERE"}, {TokenType::SET, "SET"},
        {TokenType::AND, "AND"}, {TokenType::OR, "OR"}, {TokenType::NOT, "NOT"},
        {TokenType::NULL_TOKEN, "NULL"}, {TokenType::TRUE_TOKEN, "TRUE"},
        {TokenType::FALSE_TOKEN, "FALSE"}, {TokenType::PRIMARY, "PRIMARY"},
        {TokenType::KEY, "KEY"}, {TokenType::INDEX, "INDEX"},
        {TokenType::ON, "ON"}, {TokenType::DEFAULT, "DEFAULT"},
        {TokenType::ORDER, "ORDER"}, {TokenType::BY, "BY"},
        {TokenType::ASC, "ASC"}, {TokenType::DESC, "DESC"},
        {TokenType::LIMIT, "LIMIT"}, {TokenType::AS, "AS"},
        {TokenType::DISTINCT, "DISTINCT"}, {TokenType::EXISTS, "EXISTS"},
        {TokenType::IN, "IN"}, {TokenType::BETWEEN, "BETWEEN"},
        {TokenType::LIKE, "LIKE"}, {TokenType::IS, "IS"},
        {TokenType::BEGIN, "BEGIN"}, {TokenType::COMMIT, "COMMIT"},
        {TokenType::ROLLBACK, "ROLLBACK"}, {TokenType::SAVEPOINT, "SAVEPOINT"},
        {TokenType::RELEASE, "RELEASE"}, {TokenType::TRANSACTION, "TRANSACTION"},
        {TokenType::ABORT, "ABORT"}, {TokenType::START, "START"},
        {TokenType::TO, "TO"},
        {TokenType::IDENTIFIER, "ID"}, {TokenType::INTEGER_LIT, "INT"},
        {TokenType::FLOAT_LIT, "FLOAT"}, {TokenType::STRING_LIT, "STR"},
        {TokenType::BOOL_LIT, "BOOL"},
        {TokenType::SEMICOLON, ";"}, {TokenType::COMMA, ","},
        {TokenType::DOT, "."},
        {TokenType::LPAREN, "("}, {TokenType::RPAREN, ")"},
        {TokenType::STAR, "*"}, {TokenType::EQ, "="},
        {TokenType::NEQ, "!="}, {TokenType::LT, "<"}, {TokenType::GT, ">"},
        {TokenType::LE, "<="}, {TokenType::GE, ">="},
        {TokenType::PLUS, "+"}, {TokenType::MINUS, "-"},
        {TokenType::MUL, "*"}, {TokenType::DIV, "/"},
        {TokenType::END, "EOF"}, {TokenType::UNKNOWN, "?"},
    };
    auto it = names.find(type);
    std::string name = (it != names.end()) ? it->second : "?";
    if (text.empty()) return name;
    return name + "(" + text + ")";
}

Lexer::Lexer(const std::string& input) : input_(input) {}

char Lexer::peek_char() const {
    if (pos_ >= input_.size()) return '\0';
    return input_[pos_];
}

char Lexer::advance() {
    char c = input_[pos_++];
    if (c == '\n') { line_++; col_ = 1; }
    else { col_++; }
    return c;
}

void Lexer::skip_whitespace() {
    while (isspace(peek_char())) advance();
}

void Lexer::skip_line_comment() {
    while (peek_char() != '\n' && peek_char() != '\0') advance();
}

void Lexer::skip_block_comment() {
    while (pos_ + 1 < input_.size()) {
        if (input_[pos_] == '*' && input_[pos_ + 1] == '/') {
            advance(); advance();
            return;
        }
        advance();
    }
    throw std::runtime_error("未闭合的块注释");
}

Token Lexer::make_token(TokenType type, const std::string& text) {
    return {type, text, line_, col_ - (int)text.size()};
}

Token Lexer::make_token(TokenType type, char c) {
    return {type, std::string(1, c), line_, col_ - 1};
}

Token Lexer::read_identifier() {
    std::string text;
    while (isalnum(peek_char()) || peek_char() == '_') text += advance();

    // 关键字匹配
    auto up = text;
    for (auto& c : up) c = toupper(c);

    static const std::unordered_map<std::string, TokenType> keywords = {
        {"SELECT", TokenType::SELECT}, {"INSERT", TokenType::INSERT},
        {"INTO", TokenType::INTO}, {"VALUES", TokenType::VALUES},
        {"UPDATE", TokenType::UPDATE}, {"DELETE", TokenType::DELETE},
        {"CREATE", TokenType::CREATE}, {"DROP", TokenType::DROP},
        {"TABLE", TokenType::TABLE}, {"FROM", TokenType::FROM},
        {"WHERE", TokenType::WHERE}, {"SET", TokenType::SET},
        {"AND", TokenType::AND}, {"OR", TokenType::OR}, {"NOT", TokenType::NOT},
        {"NULL", TokenType::NULL_TOKEN}, {"TRUE", TokenType::TRUE_TOKEN},
        {"FALSE", TokenType::FALSE_TOKEN}, {"PRIMARY", TokenType::PRIMARY},
        {"KEY", TokenType::KEY}, {"INDEX", TokenType::INDEX},
        {"ON", TokenType::ON}, {"DEFAULT", TokenType::DEFAULT},
        {"ORDER", TokenType::ORDER}, {"BY", TokenType::BY},
        {"ASC", TokenType::ASC}, {"DESC", TokenType::DESC},
        {"LIMIT", TokenType::LIMIT}, {"AS", TokenType::AS},
        {"DISTINCT", TokenType::DISTINCT}, {"EXISTS", TokenType::EXISTS},
        {"IN", TokenType::IN}, {"BETWEEN", TokenType::BETWEEN},
        {"LIKE", TokenType::LIKE}, {"IS", TokenType::IS},
        {"BEGIN", TokenType::BEGIN}, {"COMMIT", TokenType::COMMIT},
        {"ROLLBACK", TokenType::ROLLBACK}, {"SAVEPOINT", TokenType::SAVEPOINT},
        {"RELEASE", TokenType::RELEASE}, {"TRANSACTION", TokenType::TRANSACTION},
        {"ABORT", TokenType::ABORT}, {"START", TokenType::START},
        {"TO", TokenType::TO},
    };

    auto it = keywords.find(up);
    if (it != keywords.end()) {
        return make_token(it->second, text);
    }
    return make_token(TokenType::IDENTIFIER, text);
}

Token Lexer::read_number() {
    std::string text;
    bool is_float = false;
    while (isdigit(peek_char())) text += advance();
    if (peek_char() == '.') {
        is_float = true;
        text += advance();
        while (isdigit(peek_char())) text += advance();
    }
    return make_token(is_float ? TokenType::FLOAT_LIT : TokenType::INTEGER_LIT, text);
}

Token Lexer::read_string() {
    advance(); // skip opening '
    std::string text;
    while (peek_char() != '\'' && peek_char() != '\0') {
        if (peek_char() == '\\') { advance(); text += advance(); }
        else { text += advance(); }
    }
    if (peek_char() == '\'') advance(); // skip closing '
    return make_token(TokenType::STRING_LIT, text);
}

Token Lexer::next() {
    if (has_peeked_) {
        has_peeked_ = false;
        return peeked_;
    }

    skip_whitespace();
    char c = peek_char();
    int line = line_, col = col_;

    if (c == '\0') return {TokenType::END, "", line, col};

    // 注释
    if (c == '-' && pos_ + 1 < input_.size() && input_[pos_ + 1] == '-') {
        skip_line_comment();
        return next();
    }
    if (c == '/' && pos_ + 1 < input_.size() && input_[pos_ + 1] == '*') {
        advance(); advance();
        skip_block_comment();
        return next();
    }

    // 标识符 / 关键字
    if (isalpha(c) || c == '_') return read_identifier();

    // 数字
    if (isdigit(c)) return read_number();

    // 字符串
    if (c == '\'') return read_string();

    // 符号
    advance();
    switch (c) {
        case ';': return make_token(TokenType::SEMICOLON, c);
        case ',': return make_token(TokenType::COMMA, c);
        case '.': return make_token(TokenType::DOT, c);
        case '(': return make_token(TokenType::LPAREN, c);
        case ')': return make_token(TokenType::RPAREN, c);
        case '*': return make_token(TokenType::STAR, c);
        case '+': return make_token(TokenType::PLUS, c);
        case '-': return make_token(TokenType::MINUS, c);
        case '/': return make_token(TokenType::DIV, c);
        case '=': return make_token(TokenType::EQ, c);
        case '!':
            if (peek_char() == '=') { advance(); return make_token(TokenType::NEQ, "!="); }
            return make_token(TokenType::UNKNOWN, c);
        case '<':
            if (peek_char() == '=') { advance(); return make_token(TokenType::LE, "<="); }
            return make_token(TokenType::LT, c);
        case '>':
            if (peek_char() == '=') { advance(); return make_token(TokenType::GE, ">="); }
            return make_token(TokenType::GT, c);
        default:
            return make_token(TokenType::UNKNOWN, c);
    }
}

Token Lexer::peek() {
    if (!has_peeked_) {
        peeked_ = next();
        has_peeked_ = true;
    }
    return peeked_;
}

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> tokens;
    while (true) {
        Token t = next();
        tokens.push_back(t);
        if (t.type == TokenType::END) break;
    }
    return tokens;
}
