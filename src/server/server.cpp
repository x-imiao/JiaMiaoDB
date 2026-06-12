#include "server.h"
#include "../storage/engine.h"
#include "../storage/catalog.h"
#include "../ai/memory.h"
#include "../ai/executor.h"
#include "../ai/nl_parser.h"
#include "../ai/planner.h"
#include "../supervisor/rules.h"
#include "../sql/lexer.h"
#include "../sql/parser.h"

#include <iostream>
#include <sstream>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include "common/json.h"

using json = Json;

JiamiaoDBServer::JiamiaoDBServer(const DBConfig& config) : config_(config) {
    // 初始化核心组件
    storage_ = new StorageEngine(config.storage.data_dir, config.storage.checkpoint_interval);
    memory_ = new WorkloadMemory();
    executor_ = new PipelineExecutor(storage_, memory_);
    nl_parser_ = new NLParser(config.ai.api_key, config.ai.api_base, config.ai.model);
    supervisor_ = new Supervisor(storage_);
}

JiamiaoDBServer::~JiamiaoDBServer() {
    stop();
    delete executor_;
    delete memory_;
    delete storage_;
    delete nl_parser_;
    delete supervisor_;
}

void JiamiaoDBServer::start() {
    // 加载存储和记忆
    storage_->load();
    memory_->load(config_.storage.data_dir + "/memory.json");

    // 创建 socket
    server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        std::cerr << "[服务器] 创建 socket 失败\n";
        return;
    }

    int opt = 1;
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(config_.server.host.c_str());
    addr.sin_port = htons(config_.server.port);

    if (bind(server_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "[服务器] 绑定地址 " << config_.server.host
                  << ":" << config_.server.port << " 失败\n";
        close(server_fd_);
        server_fd_ = -1;
        return;
    }

    if (listen(server_fd_, config_.server.max_connections) < 0) {
        std::cerr << "[服务器] 监听失败\n";
        close(server_fd_);
        server_fd_ = -1;
        return;
    }

    running_ = true;

    std::cout << "\n  ╔══════════════════════════════════════════════╗\n";
    std::cout << "  ║     JiamiaoDB — AI-Native Database          ║\n";
    std::cout << "  ║     版本 0.1.0 (C++17)                       ║\n";
    std::cout << "  ╚══════════════════════════════════════════════╝\n";
    std::cout << "  监听: " << config_.server.host << ":" << config_.server.port << "\n";
    std::cout << "  数据: " << config_.storage.data_dir << "\n";
    std::cout << "  节点: " << config_.node.id << " (" << config_.node.role << ")\n";
    if (!config_.ai.api_key.empty()) {
        std::cout << "  AI:   " << config_.ai.provider << " (" << config_.ai.model << ")\n";
    } else {
        std::cout << "  AI:   未配置（jiamiao.config.json 设置 api_key）\n";
    }
    std::cout << "  PID:  " << getpid() << "\n\n";

    // 启动 worker 线程
    int worker_count = config_.server.worker_threads;
    for (int i = 0; i < worker_count; i++) {
        workers_.emplace_back(&JiamiaoDBServer::run_worker, this);
    }

    for (auto& w : workers_) w.join();
}

void JiamiaoDBServer::stop() {
    running_ = false;
    if (server_fd_ >= 0) {
        close(server_fd_);
        server_fd_ = -1;
    }
    storage_->save();
    memory_->save(config_.storage.data_dir + "/memory.json");
}

void JiamiaoDBServer::run_worker() {
    while (running_) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(server_fd_, (struct sockaddr*)&client_addr, &addr_len);
        if (client_fd < 0) {
            if (running_) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
        std::cout << "[连接] " << client_ip << ":" << ntohs(client_addr.sin_port) << "\n";

        handle_client(client_fd);
        close(client_fd);
    }
}

void JiamiaoDBServer::handle_client(int client_fd) {
    char buffer[65536];
    std::string input;

    // Read until newline or buffer full
    ssize_t n;
    while ((n = read(client_fd, buffer, sizeof(buffer) - 1)) > 0) {
        buffer[n] = '\0';
        input += buffer;

        // Check for newline (end of query)
        if (buffer[n - 1] == '\n') break;
    }

    // Strip trailing newline/carriage return
    while (!input.empty() && (input.back() == '\n' || input.back() == '\r')) {
        input.pop_back();
    }

    if (input.empty()) {
        std::string resp = "{\"error\": \"空输入\"}\n";
        write(client_fd, resp.c_str(), resp.size());
        return;
    }

    // 处理特殊命令
    if (input == ".exit" || input == ".quit") {
        std::string resp = "{\"message\": \"再见\"}\n";
        write(client_fd, resp.c_str(), resp.size());
        stop();
        return;
    }

    if (input == ".tables") {
        auto tables = storage_->list_tables();
        json j;
        j["type"] = "table_list";
        j["database"] = storage_->current_db();
        { json arr = json::array(); for (const auto& t : tables) arr.push_back(t); j["tables"] = arr; }
        std::string resp = j.dump() + "\n";
        write(client_fd, resp.c_str(), resp.size());
        return;
    }

    if (input == ".databases") {
        auto dbs = storage_->list_databases();
        json j;
        j["type"] = "database_list";
        j["current"] = storage_->current_db();
        { json arr = json::array(); for (const auto& d : dbs) arr.push_back(d); j["databases"] = arr; }
        std::string resp = j.dump() + "\n";
        write(client_fd, resp.c_str(), resp.size());
        return;
    }

    if (input == ".users") {
        auto users = storage_->list_users();
        json j;
        j["type"] = "user_list";
        { json arr = json::array(); for (const auto& u : users) arr.push_back(u); j["users"] = arr; }
        std::string resp = j.dump() + "\n";
        write(client_fd, resp.c_str(), resp.size());
        return;
    }

    if (input == ".schemas") {
        auto schemas = storage_->list_schemas(storage_->current_db());
        json j;
        j["type"] = "schema_list";
        j["database"] = storage_->current_db();
        { json arr = json::array(); for (const auto& s : schemas) arr.push_back(s); j["schemas"] = arr; }
        std::string resp = j.dump() + "\n";
        write(client_fd, resp.c_str(), resp.size());
        return;
    }

    std::string result = process_query(input);
    write(client_fd, result.c_str(), result.size());
}

std::string JiamiaoDBServer::process_query(const std::string& input) {
    json response;
    response["input"] = input;

    try {
        // 判断是 SQL 还是 NL
        if (is_likely_sql(input)) {
            // SQL 路径
            try {
                Lexer lexer(input);
                auto tokens = lexer.tokenize();
                Parser parser(tokens);
                auto stmts = parser.parse_all();

                json results_arr = json::array();
                for (auto& stmt : stmts) {
                    UnifiedIntent intent;
                    intent.raw_input = input;
                    intent.statement = std::move(stmt);

                    // 设置 category 和 tables
                    switch (intent.statement.type) {
                        case StatementType::SELECT:
                            intent.category = IntentCategory::QUERY;
                            if (intent.statement.select)
                                intent.tables = {intent.statement.select->from_table};
                            intent.is_write = false;
                            break;
                        case StatementType::INSERT:
                            intent.category = IntentCategory::MUTATION;
                            if (intent.statement.insert)
                                intent.tables = {intent.statement.insert->table};
                            intent.is_write = true;
                            break;
                        case StatementType::UPDATE:
                            intent.category = IntentCategory::MUTATION;
                            if (intent.statement.update)
                                intent.tables = {intent.statement.update->table};
                            intent.is_write = true;
                            break;
                        case StatementType::DELETE:
                            intent.category = IntentCategory::MUTATION;
                            if (intent.statement.delete_stmt)
                                intent.tables = {intent.statement.delete_stmt->table};
                            intent.is_write = true;
                            break;
                        case StatementType::CREATE_TABLE:
                            intent.category = IntentCategory::SCHEMA;
                            if (intent.statement.create_table)
                                intent.tables = {intent.statement.create_table->name};
                            intent.is_write = true;
                            break;
                        case StatementType::DROP_TABLE:
                            intent.category = IntentCategory::SCHEMA;
                            if (intent.statement.drop_table)
                                intent.tables = {intent.statement.drop_table->name};
                            intent.is_write = true;
                            break;
                        case StatementType::CREATE_DATABASE:
                            intent.category = IntentCategory::SCHEMA;
                            if (intent.statement.create_database)
                                intent.tables = {intent.statement.create_database->name};
                            intent.is_write = true;
                            break;
                        case StatementType::DROP_DATABASE:
                            intent.category = IntentCategory::SCHEMA;
                            if (intent.statement.drop_database)
                                intent.tables = {intent.statement.drop_database->name};
                            intent.is_write = true;
                            break;
                        case StatementType::CREATE_SCHEMA:
                            intent.category = IntentCategory::SCHEMA;
                            if (intent.statement.create_schema)
                                intent.tables = {intent.statement.create_schema->name};
                            intent.is_write = true;
                            break;
                        case StatementType::CREATE_USER:
                            intent.category = IntentCategory::SCHEMA;
                            if (intent.statement.create_user)
                                intent.tables = {intent.statement.create_user->name};
                            intent.is_write = true;
                            break;
                        case StatementType::DROP_USER:
                            intent.category = IntentCategory::SCHEMA;
                            if (intent.statement.drop_user)
                                intent.tables = {intent.statement.drop_user->name};
                            intent.is_write = true;
                            break;
                        case StatementType::USE_DATABASE:
                            intent.category = IntentCategory::TASK;
                            intent.is_write = false;
                            break;
                        case StatementType::SHOW_DATABASES:
                            intent.category = IntentCategory::QUERY;
                            intent.is_write = false;
                            break;
                        case StatementType::SHOW_USERS:
                            intent.category = IntentCategory::QUERY;
                            intent.is_write = false;
                            break;
                        case StatementType::SHOW_SCHEMAS:
                            intent.category = IntentCategory::QUERY;
                            intent.is_write = false;
                            break;
                        default:
                            break;
                    }

                    intent.description = "SQL: " + input;

                    // 事务控制语句
                    if (intent.statement.type == StatementType::BEGIN_TRANSACTION) {
                        storage_->txn_mgr().begin_transaction_block();
                        json r;
                        r["type"] = "txn_result";
                        r["message"] = "BEGIN";
                        results_arr.push_back(r);
                        continue;
                    }
                    if (intent.statement.type == StatementType::COMMIT_TRANSACTION) {
                        auto xid = storage_->txn_mgr().get_current_xid();
                        if (xid != 0) storage_->write_xact_commit(xid);
                        storage_->txn_mgr().commit_transaction();
                        storage_->txn_mgr().reset_context();
                        json r;
                        r["type"] = "txn_result";
                        r["message"] = "COMMIT";
                        results_arr.push_back(r);
                        continue;
                    }
                    if (intent.statement.type == StatementType::ROLLBACK_TRANSACTION) {
                        auto xid = storage_->txn_mgr().get_current_xid();
                        if (xid != 0) storage_->write_xact_abort(xid);
                        storage_->apply_undo();
                        storage_->txn_mgr().abort_transaction();
                        storage_->txn_mgr().reset_context();
                        json r;
                        r["type"] = "txn_result";
                        r["message"] = "ROLLBACK";
                        results_arr.push_back(r);
                        continue;
                    }

                    // 如果 DDL，直接通过 storage 执行
                    if (intent.statement.type == StatementType::CREATE_TABLE && intent.statement.create_table) {
                        storage_->create_table(intent.statement.create_table->name,
                                               intent.statement.create_table->columns);
                        json r;
                        r["type"] = "ddl_result";
                        r["message"] = "已创建表 " + intent.statement.create_table->name;
                        results_arr.push_back(r);
                        continue;
                    }

                    if (intent.statement.type == StatementType::DROP_TABLE && intent.statement.drop_table) {
                        storage_->drop_table(intent.statement.drop_table->name);
                        json r;
                        r["type"] = "ddl_result";
                        r["message"] = "已删除表 " + intent.statement.drop_table->name;
                        results_arr.push_back(r);
                        continue;
                    }
                    if (intent.statement.type == StatementType::CREATE_DATABASE && intent.statement.create_database) {
                        storage_->create_database(intent.statement.create_database->name);
                        json r;
                        r["type"] = "ddl_result";
                        r["message"] = "已创建数据库 " + intent.statement.create_database->name;
                        results_arr.push_back(r);
                        continue;
                    }
                    if (intent.statement.type == StatementType::DROP_DATABASE && intent.statement.drop_database) {
                        storage_->drop_database(intent.statement.drop_database->name);
                        json r;
                        r["type"] = "ddl_result";
                        r["message"] = "已删除数据库 " + intent.statement.drop_database->name;
                        results_arr.push_back(r);
                        continue;
                    }
                    if (intent.statement.type == StatementType::CREATE_SCHEMA && intent.statement.create_schema) {
                        storage_->create_schema(storage_->current_db(), intent.statement.create_schema->name);
                        json r;
                        r["type"] = "ddl_result";
                        r["message"] = "已创建模式 " + intent.statement.create_schema->name;
                        results_arr.push_back(r);
                        continue;
                    }
                    if (intent.statement.type == StatementType::CREATE_USER && intent.statement.create_user) {
                        storage_->create_user(intent.statement.create_user->name,
                                             intent.statement.create_user->password);
                        json r;
                        r["type"] = "ddl_result";
                        r["message"] = "已创建用户 " + intent.statement.create_user->name;
                        results_arr.push_back(r);
                        continue;
                    }
                    if (intent.statement.type == StatementType::DROP_USER && intent.statement.drop_user) {
                        storage_->drop_user(intent.statement.drop_user->name);
                        json r;
                        r["type"] = "ddl_result";
                        r["message"] = "已删除用户 " + intent.statement.drop_user->name;
                        results_arr.push_back(r);
                        continue;
                    }
                    if (intent.statement.type == StatementType::USE_DATABASE && intent.statement.use_database) {
                        storage_->set_current_db(intent.statement.use_database->name);
                        json r;
                        r["type"] = "ddl_result";
                        r["message"] = "已切换到数据库 " + intent.statement.use_database->name;
                        results_arr.push_back(r);
                        continue;
                    }
                    if (intent.statement.type == StatementType::SHOW_DATABASES) {
                        auto dbs = storage_->list_databases();
                        json r;
                        r["type"] = "database_list";
                        r["current"] = storage_->current_db();
                        { json arr = json::array(); for (const auto& d : dbs) arr.push_back(d); r["databases"] = arr; }
                        results_arr.push_back(r);
                        continue;
                    }
                    if (intent.statement.type == StatementType::SHOW_USERS) {
                        auto users = storage_->list_users();
                        json r;
                        r["type"] = "user_list";
                        { json arr = json::array(); for (const auto& u : users) arr.push_back(u); r["users"] = arr; }
                        results_arr.push_back(r);
                        continue;
                    }
                    if (intent.statement.type == StatementType::SHOW_SCHEMAS) {
                        auto schemas = storage_->list_schemas(storage_->current_db());
                        json r;
                        r["type"] = "schema_list";
                        r["database"] = storage_->current_db();
                        { json arr = json::array(); for (const auto& s : schemas) arr.push_back(s); r["schemas"] = arr; }
                        results_arr.push_back(r);
                        continue;
                    }

                    // 隐式事务包装
                    storage_->txn_mgr().start_transaction_command();

                    // 通过 AI Executor 执行
                    auto exec_result = executor_->execute(std::move(intent));

                    storage_->txn_mgr().commit_transaction_command();

                    for (const auto& rs : exec_result.results) {
                        json r;
                        r["type"] = "query_result";
                        if (!rs.message.empty()) {
                            r["message"] = rs.message;
                        }
                        if (!rs.columns.empty()) {
                            { json arr = json::array(); for (const auto& c : rs.columns) arr.push_back(c); r["columns"] = arr; }
                            r["rows"] = json::array();
                            for (const auto& row : rs.rows) {
                                json jrow;
                                for (const auto& [k, v] : row) {
                                    if (k.empty() || k[0] == '_') continue;
                                    std::visit([&](auto&& val) {
                                        using T = std::decay_t<decltype(val)>;
                                        if constexpr (std::is_same_v<T, std::nullptr_t>) jrow[k] = nullptr;
                                        else if constexpr (std::is_same_v<T, int64_t>) jrow[k] = val;
                                        else if constexpr (std::is_same_v<T, double>) jrow[k] = val;
                                        else if constexpr (std::is_same_v<T, bool>) jrow[k] = val;
                                        else jrow[k] = val;
                                    }, v);
                                }
                                r["rows"].push_back(jrow);
                            }
                        }
                        r["affected_rows"] = rs.affected_rows;
                        r["duration_ms"] = rs.duration_ms;
                        r["plan_source"] = exec_result.plan.source;
                        r["reasoning"] = exec_result.plan.reasoning;
                        results_arr.push_back(r);
                    }
                }

                response["type"] = "sql_result";
                response["results"] = results_arr;

            } catch (const ParseError& e) {
                // SQL 解析失败，给提示
                response["type"] = "sql_syntax_error";
                response["message"] = std::string("不支持的 SQL 语法: ") + e.what();
                response["suggestion"] = "试试 .tables / .schema <表名>，或直接用自然语言描述";
            }
        } else {
            // NL 路径
            if (!nl_parser_->is_configured()) {
                response["type"] = "nl_unconfigured";
                response["message"] = "AI API 未配置。编辑 jiamiao.config.json 填入 api_key 启用自然语言查询";
            } else {
                auto nl_result = nl_parser_->parse(input, build_schema_context());
                if (nl_result.success && nl_result.is_conversation) {
                    response["type"] = "conversation";
                    response["reply"] = nl_result.reply;
                } else if (nl_result.success) {
                    storage_->txn_mgr().start_transaction_command();
                    auto exec_result = executor_->execute(std::move(nl_result.intent));
                    storage_->txn_mgr().commit_transaction_command();
                    json results_arr = json::array();
                    for (const auto& rs : exec_result.results) {
                        json r;
                        r["type"] = "query_result";
                        if (!rs.message.empty()) r["message"] = rs.message;
                        if (!rs.columns.empty()) {
                            { json arr = json::array(); for (const auto& c : rs.columns) arr.push_back(c); r["columns"] = arr; }
                            r["rows"] = json::array();
                            for (const auto& row : rs.rows) {
                                json jrow;
                                for (const auto& [k, v] : row) {
                                    if (k.empty() || k[0] == '_') continue;
                                    std::visit([&](auto&& val) {
                                        using T = std::decay_t<decltype(val)>;
                                        if constexpr (std::is_same_v<T, std::nullptr_t>) jrow[k] = nullptr;
                                        else if constexpr (std::is_same_v<T, int64_t>) jrow[k] = val;
                                        else if constexpr (std::is_same_v<T, double>) jrow[k] = val;
                                        else if constexpr (std::is_same_v<T, bool>) jrow[k] = val;
                                        else jrow[k] = val;
                                    }, v);
                                }
                                r["rows"].push_back(jrow);
                            }
                        }
                        r["affected_rows"] = rs.affected_rows;
                        r["duration_ms"] = rs.duration_ms;
                        r["plan_source"] = exec_result.plan.source;
                        r["reasoning"] = exec_result.plan.reasoning;
                        results_arr.push_back(r);
                    }
                    response["type"] = "nl_result";
                    response["results"] = results_arr;
                } else {
                    response["type"] = "nl_error";
                    response["message"] = nl_result.error;
                }
            }
        }
    } catch (const std::exception& e) {
        response["type"] = "error";
        response["message"] = std::string("执行异常: ") + e.what();
    }

    response["timestamp"] = (int64_t)time(nullptr);
    return response.dump() + "\n";
}

std::string JiamiaoDBServer::build_schema_context() {
    std::string ctx;
    ctx += "当前数据库: " + storage_->current_db() + "\n";
    ctx += "可用数据库: ";
    auto dbs = storage_->list_databases();
    for (size_t i = 0; i < dbs.size(); i++) {
        if (i > 0) ctx += ", ";
        ctx += dbs[i];
    }
    ctx += "\n";

    auto tables = storage_->list_tables();
    if (tables.empty()) {
        ctx += "（当前数据库没有表）\n";
    } else {
        for (const auto& name : tables) {
            auto* s = storage_->get_schema(name);
            if (!s) continue;
            ctx += name + " (" + std::to_string(s->row_count) + " 行): ";
            for (size_t i = 0; i < s->columns.size(); i++) {
                if (i > 0) ctx += ", ";
                ctx += s->columns[i].name + " " + data_type_name(s->columns[i].type);
                if (s->columns[i].primary_key) ctx += " PK";
                if (!s->columns[i].nullable) ctx += " NOT NULL";
            }
            ctx += "\n";
        }
    }
    return ctx;
}

bool JiamiaoDBServer::is_likely_sql(const std::string& input) {
    std::string trimmed = input;
    while (!trimmed.empty() && (trimmed[0] == ' ' || trimmed[0] == '\t')) {
        trimmed.erase(0, 1);
    }

    auto starts_with = [&](const std::string& kw) -> bool {
        if (trimmed.size() < kw.size()) return false;
        std::string prefix = trimmed.substr(0, kw.size());
        for (auto& c : prefix) c = toupper(c);
        return prefix == kw;
    };

    return starts_with("SELECT") || starts_with("INSERT") || starts_with("UPDATE") ||
           starts_with("DELETE") || starts_with("CREATE") || starts_with("DROP") ||
           starts_with("ALTER") || starts_with("SHOW") || starts_with("USE") ||
           starts_with("BEGIN") || starts_with("COMMIT") || starts_with("ROLLBACK") ||
           starts_with("TRUNCATE");
}
