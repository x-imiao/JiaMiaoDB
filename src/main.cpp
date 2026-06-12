/* ═══════════════════════════════════════════════════════
   JiamiaoDB — AI-Native Database

   入口点：加载配置 → 初始化组件 → 启动服务
   ═══════════════════════════════════════════════════════ */

#include <iostream>
#include <csignal>
#include <cstring>
#include <unistd.h>
#include "config.h"
#include "server/server.h"
#include "common/memcontext.h"

JiamiaoDBServer* g_server = nullptr;

void signal_handler(int sig) {
    std::cout << "\n[服务器] 收到信号 " << sig << "，正在关闭...\n";
    if (g_server) g_server->stop();
    exit(0);
}

void print_usage(const char* prog) {
    std::cout << "用法: " << prog << " [选项]\n";
    std::cout << "选项:\n";
    std::cout << "  -c, --config <文件>   配置文件路径 (默认: ./jiamiao.config.json)\n";
    std::cout << "  -p, --port <端口>     监听端口 (默认: 9615)\n";
    std::cout << "  -h, --host <地址>     监听地址 (默认: 127.0.0.1)\n";
    std::cout << "  -d, --data <目录>     数据存储目录 (默认: ./data)\n";
    std::cout << "  -w, --workers <线程数> Worker 线程数 (默认: 4)\n";
    std::cout << "  -v, --verbose         详细日志\n";
    std::cout << "  --help                显示此帮助\n";
}

int main(int argc, char* argv[]) {
    // 必须最先初始化 MemoryContext: WAL replay 在 StorageEngine ctor 内, 那时已用 jmalloc
    jiamiao::MemoryContextInit();

    // 默认配置路径
    std::string config_path = "./jiamiao.config.json";

    // 解析命令行参数（第一遍：只收集配置路径）
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if ((arg == "-c" || arg == "--config") && i + 1 < argc) {
            config_path = argv[++i];
        }
    }

    // 加载配置
    DBConfig config = DBConfig::load(config_path);

    // 命令行参数覆盖配置（第二遍：覆盖已加载的配置）
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if ((arg == "-p" || arg == "--port") && i + 1 < argc) {
            config.server.port = std::stoi(argv[++i]);
        } else if ((arg == "-h" || arg == "--host") && i + 1 < argc) {
            config.server.host = argv[++i];
        } else if ((arg == "-d" || arg == "--data") && i + 1 < argc) {
            config.storage.data_dir = argv[++i];
        } else if ((arg == "-w" || arg == "--workers") && i + 1 < argc) {
            config.server.worker_threads = std::stoi(argv[++i]);
        } else if (arg == "-v" || arg == "--verbose") {
            config.verbose = true;
        }
    }

    // 注册信号处理
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // 启动服务
    JiamiaoDBServer server(config);
    g_server = &server;
    server.start();

    return 0;
}
