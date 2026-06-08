#ifndef JIAMIAODB_SERVER_H
#define JIAMIAODB_SERVER_H

#include <string>
#include <thread>
#include <vector>
#include <atomic>
#include <netinet/in.h>
#include "../config.h"

class StorageEngine;
class WorkloadMemory;
class PipelineExecutor;
class NLParser;
class Supervisor;

/* ═══════════════════════════════════════════════════════
   Server — 数据库服务

   TCP 监听端口，接收 SQL/NL 输入，返回 JSON 结果。
   支持单节点和分布式启动。
   ═══════════════════════════════════════════════════════ */

class JiamiaoDBServer {
public:
    explicit JiamiaoDBServer(const DBConfig& config);
    ~JiamiaoDBServer();

    void start();
    void stop();

private:
    DBConfig config_;
    std::atomic<bool> running_{false};

    // Core components
    StorageEngine* storage_;
    WorkloadMemory* memory_;
    PipelineExecutor* executor_;
    NLParser* nl_parser_;
    Supervisor* supervisor_;

    // Server
    int server_fd_ = -1;
    std::vector<std::thread> workers_;

    void handle_client(int client_fd);
    std::string process_query(const std::string& input);
    std::string build_schema_context();
    std::string format_result(const std::string& result);
    void run_worker();

    // SQL keyword check
    static bool is_likely_sql(const std::string& input);
};

#endif // JIAMIAODB_SERVER_H
