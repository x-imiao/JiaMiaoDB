#include "parser.h"
#include <sstream>

Parser::Parser(const std::vector<Token>& tokens) : tokens_(tokens) {}

const Token& Parser::peek() const {
    if (pos_ >= tokens_.size()) return tokens_.back();
    return tokens_[pos_];
}

const Token& Parser::previous() const {
    if (pos_ == 0) return tokens_[0];
    return tokens_[pos_ - 1];
}

Token Parser::advance() {
    if (!is_at_end()) pos_++;
    return previous();
}

Token Parser::expect(TokenType type, const std::string& error_msg) {
    if (peek().type != type) {
        throw ParseError(error_msg + "，但在第" + std::to_string(peek().line) + "行遇到: " + peek().text);
    }
    return advance();
}

bool Parser::match(TokenType type) {
    if (check(type)) { advance(); return true; }
    return false;
}

bool Parser::check(TokenType type) const {
    if (is_at_end()) return false;
    return peek().type == type;
}

bool Parser::is_at_end() const {
    return peek().type == TokenType::END;
}

/* ─── 入口 ─── */

Statement Parser::parse_statement() {
    switch (peek().type) {
        case TokenType::SELECT: return parse_select();
        case TokenType::INSERT: return parse_insert();
        case TokenType::UPDATE: return parse_update();
        case TokenType::DELETE: return parse_delete();
        case TokenType::CREATE: return parse_create();
        case TokenType::DROP: return parse_drop_table();
        case TokenType::BEGIN: return parse_begin();
        case TokenType::COMMIT: return parse_commit();
        case TokenType::ROLLBACK: return parse_rollback();
        case TokenType::USE: return parse_use();
        case TokenType::SHOW: return parse_show();
        default:
            throw ParseError("不支持的语句，以 '" + peek().text + "' 开头");
    }
}

std::vector<Statement> Parser::parse_all() {
    std::vector<Statement> stmts;
    while (!is_at_end()) {
        if (peek().type == TokenType::END) break;
        if (peek().type == TokenType::SEMICOLON) { advance(); continue; }
        stmts.push_back(parse_statement());
        match(TokenType::SEMICOLON);
    }
    return stmts;
}

/* ─── SELECT ─── */

Statement Parser::parse_select() {
    auto stmt = std::make_unique<SelectStmt>();
    expect(TokenType::SELECT, "期望 SELECT");

    // columns
    if (match(TokenType::STAR)) {
        stmt->columns = {}; // empty = *
    } else {
        stmt->columns.push_back(expect(TokenType::IDENTIFIER, "期望列名").text);
        while (match(TokenType::COMMA)) {
            stmt->columns.push_back(expect(TokenType::IDENTIFIER, "期望列名").text);
        }
    }

    expect(TokenType::FROM, "期望 FROM");
    stmt->from_table = expect(TokenType::IDENTIFIER, "期望表名").text;

    if (match(TokenType::WHERE)) {
        stmt->where = parse_expression();
    }

    if (match(TokenType::ORDER)) {
        expect(TokenType::BY, "期望 BY");
        // 简单 ORDER BY，忽略具体实现
        advance(); // column
        if (match(TokenType::ASC) || match(TokenType::DESC)) {}
    }

    if (match(TokenType::LIMIT)) {
        // 简单 LIMIT，忽略具体实现
        if (peek().type == TokenType::INTEGER_LIT) advance();
    }

    Statement s;
    s.type = StatementType::SELECT;
    s.select = std::move(stmt);
    return s;
}

/* ─── INSERT ─── */

Statement Parser::parse_insert() {
    auto stmt = std::make_unique<InsertStmt>();
    expect(TokenType::INSERT, "期望 INSERT");
    expect(TokenType::INTO, "期望 INTO");
    stmt->table = expect(TokenType::IDENTIFIER, "期望表名").text;

    if (peek().type == TokenType::LPAREN && tokens_[pos_ + 1].type != TokenType::SELECT) {
        expect(TokenType::LPAREN, "期望 (");
        stmt->columns.push_back(expect(TokenType::IDENTIFIER, "期望列名").text);
        while (match(TokenType::COMMA)) {
            stmt->columns.push_back(expect(TokenType::IDENTIFIER, "期望列名").text);
        }
        expect(TokenType::RPAREN, "期望 )");
    }

    // VALUES 或 Value 关键字
    if (peek().type == TokenType::VALUES || peek().text == "VALUE" || peek().text == "value") {
        advance(); // VALUES
    }

    expect(TokenType::LPAREN, "期望 ( 开始值列表");
    std::vector<Value> row;
    row.push_back(parse_primary()->literal);
    while (match(TokenType::COMMA)) {
        row.push_back(parse_primary()->literal);
    }
    stmt->values.push_back(std::move(row));
    expect(TokenType::RPAREN, "期望 ) 结束值列表");

    while (peek().type == TokenType::COMMA) {
        advance();
        expect(TokenType::LPAREN, "期望 (");
        std::vector<Value> next_row;
        next_row.push_back(parse_primary()->literal);
        while (match(TokenType::COMMA)) {
            next_row.push_back(parse_primary()->literal);
        }
        stmt->values.push_back(std::move(next_row));
        expect(TokenType::RPAREN, "期望 )");
    }

    Statement s;
    s.type = StatementType::INSERT;
    s.insert = std::move(stmt);
    return s;
}

/* ─── UPDATE ─── */

Statement Parser::parse_update() {
    auto stmt = std::make_unique<UpdateStmt>();
    expect(TokenType::UPDATE, "期望 UPDATE");
    stmt->table = expect(TokenType::IDENTIFIER, "期望表名").text;
    expect(TokenType::SET, "期望 SET");

    std::string col = expect(TokenType::IDENTIFIER, "期望列名").text;
    expect(TokenType::EQ, "期望 =");
    stmt->sets[col] = parse_expression();

    while (match(TokenType::COMMA)) {
        std::string c = expect(TokenType::IDENTIFIER, "期望列名").text;
        expect(TokenType::EQ, "期望 =");
        stmt->sets[c] = parse_expression();
    }

    if (match(TokenType::WHERE)) {
        stmt->where = parse_expression();
    }

    Statement s;
    s.type = StatementType::UPDATE;
    s.update = std::move(stmt);
    return s;
}

/* ─── DELETE ─── */

Statement Parser::parse_delete() {
    auto stmt = std::make_unique<DeleteStmt>();
    expect(TokenType::DELETE, "期望 DELETE");
    expect(TokenType::FROM, "期望 FROM");
    stmt->table = expect(TokenType::IDENTIFIER, "期望表名").text;

    if (match(TokenType::WHERE)) {
        stmt->where = parse_expression();
    }

    Statement s;
    s.type = StatementType::DELETE;
    s.delete_stmt = std::move(stmt);
    return s;
}

/* ─── CREATE ─── */

Statement Parser::parse_create() {
    expect(TokenType::CREATE, "期望 CREATE");

    // CREATE INDEX ...
    if (match(TokenType::INDEX)) {
        return parse_create_index();
    }

    // CREATE DATABASE ...
    if (match(TokenType::DATABASE)) {
        return parse_create_database();
    }

    // CREATE SCHEMA ...
    if (match(TokenType::SCHEMA)) {
        return parse_create_schema();
    }

    // CREATE USER ...
    if (match(TokenType::USER)) {
        return parse_create_user();
    }

    // CREATE TABLE ... (default)
    expect(TokenType::TABLE, "期望 TABLE / DATABASE / SCHEMA / USER");
    return parse_create_table();
}

/* ─── CREATE TABLE ─── */

Statement Parser::parse_create_table() {
    auto stmt = std::make_unique<CreateTableStmt>();
    stmt->name = expect(TokenType::IDENTIFIER, "期望表名").text;
    expect(TokenType::LPAREN, "期望 (");
    stmt->columns.push_back(parse_column_def());
    while (match(TokenType::COMMA)) {
        // 跳过 PRIMARY KEY, FOREIGN KEY 等约束
        if (peek().type == TokenType::PRIMARY) {
            advance(); // PRIMARY
            advance(); // KEY
            expect(TokenType::LPAREN, "期望 (");
            advance(); // column
            expect(TokenType::RPAREN, "期望 )");
            continue;
        }
        stmt->columns.push_back(parse_column_def());
    }
    expect(TokenType::RPAREN, "期望 )");

    Statement s;
    s.type = StatementType::CREATE_TABLE;
    s.create_table = std::move(stmt);
    return s;
}

/* ─── DROP TABLE ─── */

Statement Parser::parse_drop_table() {
    expect(TokenType::DROP, "期望 DROP");

    if (match(TokenType::DATABASE)) {
        return parse_drop_database();
    }
    if (match(TokenType::USER)) {
        return parse_drop_user();
    }

    expect(TokenType::TABLE, "期望 TABLE / DATABASE / USER");
    std::string name = expect(TokenType::IDENTIFIER, "期望表名").text;

    Statement s;
    s.type = StatementType::DROP_TABLE;
    s.drop_table = std::make_unique<DropTableStmt>();
    s.drop_table->name = name;
    return s;
}

/* ─── CREATE INDEX ─── */

Statement Parser::parse_create_index() {
    auto stmt = std::make_unique<CreateIndexStmt>();
    stmt->index_name = expect(TokenType::IDENTIFIER, "期望索引名").text;
    expect(TokenType::ON, "期望 ON");
    stmt->table = expect(TokenType::IDENTIFIER, "期望表名").text;
    expect(TokenType::LPAREN, "期望 (");
    stmt->column = expect(TokenType::IDENTIFIER, "期望列名").text;
    expect(TokenType::RPAREN, "期望 )");

    Statement s;
    s.type = StatementType::CREATE_INDEX;
    s.create_index = std::move(stmt);
    return s;
}

/* ─── BEGIN ─── */

Statement Parser::parse_begin() {
    expect(TokenType::BEGIN, "期望 BEGIN");
    // 可选 TRANSACTION / WORK
    if (match(TokenType::TRANSACTION)) {}
    else if (check(TokenType::IDENTIFIER)) {
        auto up = peek().text;
        for (auto& c : up) c = toupper(c);
        if (up == "WORK" || up == "TRANSACTION") advance();
    }

    Statement s;
    s.type = StatementType::BEGIN_TRANSACTION;
    return s;
}

/* ─── COMMIT ─── */

Statement Parser::parse_commit() {
    expect(TokenType::COMMIT, "期望 COMMIT");
    // 可选 TRANSACTION / WORK
    if (match(TokenType::TRANSACTION)) {}
    else if (check(TokenType::IDENTIFIER)) {
        auto up = peek().text;
        for (auto& c : up) c = toupper(c);
        if (up == "WORK" || up == "TRANSACTION") advance();
    }

    Statement s;
    s.type = StatementType::COMMIT_TRANSACTION;
    return s;
}

/* ─── ROLLBACK ─── */

Statement Parser::parse_rollback() {
    expect(TokenType::ROLLBACK, "期望 ROLLBACK");
    // 可选 TRANSACTION / WORK
    if (match(TokenType::TRANSACTION)) {}
    else if (check(TokenType::IDENTIFIER)) {
        auto up = peek().text;
        for (auto& c : up) c = toupper(c);
        if (up == "WORK" || up == "TRANSACTION") advance();
    }
    // 可选 TO [SAVEPOINT] savepoint_name (Phase 2)

    Statement s;
    s.type = StatementType::ROLLBACK_TRANSACTION;
    return s;
}

/* ─── CREATE DATABASE ─── */

Statement Parser::parse_create_database() {
    auto stmt = std::make_unique<CreateDatabaseStmt>();
    stmt->name = expect(TokenType::IDENTIFIER, "期望数据库名").text;

    Statement s;
    s.type = StatementType::CREATE_DATABASE;
    s.create_database = std::move(stmt);
    return s;
}

/* ─── CREATE SCHEMA ─── */

Statement Parser::parse_create_schema() {
    auto stmt = std::make_unique<CreateSchemaStmt>();
    stmt->name = expect(TokenType::IDENTIFIER, "期望模式名").text;

    // 可选: CREATE SCHEMA [database.]name 或 AUTHORIZATION
    // 简化: 只支持 CREATE SCHEMA name

    Statement s;
    s.type = StatementType::CREATE_SCHEMA;
    s.create_schema = std::move(stmt);
    return s;
}

/* ─── CREATE USER ─── */

Statement Parser::parse_create_user() {
    auto stmt = std::make_unique<CreateUserStmt>();
    stmt->name = expect(TokenType::IDENTIFIER, "期望用户名").text;

    // 必须有 WITH PASSWORD 'xxx'
    if (!match(TokenType::WITH)) {
        throw ParseError("CREATE USER 必须指定 WITH PASSWORD");
    }
    // PASSWORD 可能是关键字或标识符
    std::string pwd_kw = peek().text;
    auto up = pwd_kw;
    for (auto& c : up) c = toupper(c);
    if (up == "PASSWORD") {
        advance(); // PASSWORD
    } else {
        throw ParseError("CREATE USER 必须指定 PASSWORD");
    }
    stmt->password = expect(TokenType::STRING_LIT, "期望密码字符串").text;

    Statement s;
    s.type = StatementType::CREATE_USER;
    s.create_user = std::move(stmt);
    return s;
}

/* ─── DROP DATABASE ─── */

Statement Parser::parse_drop_database() {
    auto stmt = std::make_unique<DropDatabaseStmt>();
    stmt->name = expect(TokenType::IDENTIFIER, "期望数据库名").text;

    Statement s;
    s.type = StatementType::DROP_DATABASE;
    s.drop_database = std::move(stmt);
    return s;
}

/* ─── DROP USER ─── */

Statement Parser::parse_drop_user() {
    auto stmt = std::make_unique<DropUserStmt>();
    stmt->name = expect(TokenType::IDENTIFIER, "期望用户名").text;

    Statement s;
    s.type = StatementType::DROP_USER;
    s.drop_user = std::move(stmt);
    return s;
}

/* ─── USE ─── */

Statement Parser::parse_use() {
    expect(TokenType::USE, "期望 USE");
    auto stmt = std::make_unique<UseDatabaseStmt>();
    stmt->name = expect(TokenType::IDENTIFIER, "期望数据库名").text;

    Statement s;
    s.type = StatementType::USE_DATABASE;
    s.use_database = std::move(stmt);
    return s;
}

/* ─── SHOW ─── */

Statement Parser::parse_show() {
    expect(TokenType::SHOW, "期望 SHOW");

    if (match(TokenType::DATABASES)) {
        Statement s;
        s.type = StatementType::SHOW_DATABASES;
        return s;
    }
    if (match(TokenType::USERS)) {
        Statement s;
        s.type = StatementType::SHOW_USERS;
        return s;
    }
    if (match(TokenType::SCHEMAS)) {
        Statement s;
        s.type = StatementType::SHOW_SCHEMAS;
        return s;
    }

    throw ParseError("SHOW 后期望 DATABASES / USERS / SCHEMAS，但遇到: " + peek().text);
}

/* ─── 表达式解析 ─── */

// 优先级: OR < AND < 比较 < 加减 < 乘除 < 一元 < 基本

ExprPtr Parser::parse_expression() {
    return parse_or();
}

ExprPtr Parser::parse_or() {
    auto expr = parse_and();
    while (match(TokenType::OR)) {
        auto right = parse_and();
        expr = Expression::make_binary_op("OR", std::move(expr), std::move(right));
    }
    return expr;
}

ExprPtr Parser::parse_and() {
    auto expr = parse_comparison();
    while (match(TokenType::AND)) {
        auto right = parse_comparison();
        expr = Expression::make_binary_op("AND", std::move(expr), std::move(right));
    }
    return expr;
}

ExprPtr Parser::parse_comparison() {
    auto expr = parse_addition();

    if (match(TokenType::EQ)) {
        auto right = parse_addition();
        return Expression::make_binary_op("=", std::move(expr), std::move(right));
    }
    if (match(TokenType::NEQ)) {
        auto right = parse_addition();
        return Expression::make_binary_op("!=", std::move(expr), std::move(right));
    }
    if (match(TokenType::LT)) {
        auto right = parse_addition();
        return Expression::make_binary_op("<", std::move(expr), std::move(right));
    }
    if (match(TokenType::GT)) {
        auto right = parse_addition();
        return Expression::make_binary_op(">", std::move(expr), std::move(right));
    }
    if (match(TokenType::LE)) {
        auto right = parse_addition();
        return Expression::make_binary_op("<=", std::move(expr), std::move(right));
    }
    if (match(TokenType::GE)) {
        auto right = parse_addition();
        return Expression::make_binary_op(">=", std::move(expr), std::move(right));
    }

    return expr;
}

ExprPtr Parser::parse_addition() {
    auto expr = parse_multiplication();
    while (true) {
        if (match(TokenType::PLUS)) {
            auto right = parse_multiplication();
            expr = Expression::make_binary_op("+", std::move(expr), std::move(right));
        } else if (match(TokenType::MINUS)) {
            auto right = parse_multiplication();
            expr = Expression::make_binary_op("-", std::move(expr), std::move(right));
        } else break;
    }
    return expr;
}

ExprPtr Parser::parse_multiplication() {
    auto expr = parse_unary();
    while (true) {
        if (match(TokenType::MUL) || match(TokenType::STAR)) {
            auto right = parse_unary();
            expr = Expression::make_binary_op("*", std::move(expr), std::move(right));
        } else if (match(TokenType::DIV)) {
            auto right = parse_unary();
            expr = Expression::make_binary_op("/", std::move(expr), std::move(right));
        } else break;
    }
    return expr;
}

ExprPtr Parser::parse_unary() {
    if (match(TokenType::NOT)) {
        auto inner = parse_unary();
        return Expression::make_unary_op("NOT", std::move(inner));
    }
    if (match(TokenType::MINUS)) {
        auto inner = parse_unary();
        // -x = 0 - x
        auto zero = Expression::make_literal((int64_t)0);
        return Expression::make_binary_op("-", std::move(zero), std::move(inner));
    }
    return parse_primary();
}

ExprPtr Parser::parse_primary() {
    if (match(TokenType::INTEGER_LIT)) {
        int64_t v = std::stoll(previous().text);
        return Expression::make_literal(v);
    }
    if (match(TokenType::FLOAT_LIT)) {
        double v = std::stod(previous().text);
        return Expression::make_literal(v);
    }
    if (match(TokenType::STRING_LIT)) {
        return Expression::make_literal(std::string(previous().text));
    }
    if (match(TokenType::TRUE_TOKEN)) {
        return Expression::make_literal(true);
    }
    if (match(TokenType::FALSE_TOKEN)) {
        return Expression::make_literal(false);
    }
    if (match(TokenType::NULL_TOKEN)) {
        return Expression::make_literal(nullptr);
    }
    if (match(TokenType::IDENTIFIER)) {
        return Expression::make_column_ref(previous().text);
    }
    if (match(TokenType::LPAREN)) {
        auto expr = parse_expression();
        expect(TokenType::RPAREN, "期望 )");
        return expr;
    }

    throw ParseError("期望表达式，但遇到: " + peek().text);
}

/* ─── 辅助 ─── */

ColumnDef Parser::parse_column_def() {
    ColumnDef col;
    col.name = expect(TokenType::IDENTIFIER, "期望列名").text;
    col.type = parse_type();

    if (peek().type == TokenType::IDENTIFIER) {
        std::string mod = advance().text;
        auto up = mod;
        for (auto& c : up) c = toupper(c);
        if (up == "NOT") {
            expect(TokenType::NULL_TOKEN, "期望 NULL (NOT NULL)");
            col.nullable = false;
        } else if (up == "PRIMARY") {
            expect(TokenType::KEY, "期望 KEY (PRIMARY KEY)");
            col.primary_key = true;
        }
        // second modifier
        if (peek().type == TokenType::IDENTIFIER) {
            std::string mod2 = advance().text;
            auto up2 = mod2;
            for (auto& c : up2) c = toupper(c);
            if (up2 == "PRIMARY") {
                expect(TokenType::KEY, "期望 KEY");
                col.primary_key = true;
            }
        }
    }

    if (match(TokenType::PRIMARY)) {
        expect(TokenType::KEY, "期望 KEY");
        col.primary_key = true;
    }

    return col;
}

DataType Parser::parse_type() {
    std::string type_name = expect(TokenType::IDENTIFIER, "期望数据类型").text;
    return data_type_from_name(type_name);
}

std::vector<std::string> Parser::parse_ident_list() {
    std::vector<std::string> list;
    list.push_back(expect(TokenType::IDENTIFIER, "期望标识符").text);
    while (match(TokenType::COMMA)) {
        list.push_back(expect(TokenType::IDENTIFIER, "期望标识符").text);
    }
    return list;
}
