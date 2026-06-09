#ifndef JIAMIAODB_CONFIG_H
#define JIAMIAODB_CONFIG_H

#include <string>
#include <vector>
#include <fstream>
#include <iostream>
#include "json.h"

using json = Json;

/* ═══════════════════════════════════════════════════════
   配置管理 — 集中管理所有配置项，支持后续扩展
   从 JSON 配置文件加载，提供默认值
   ═══════════════════════════════════════════════════════ */

struct AIConfig {
    std::string provider = "minimax";
    std::string api_key;
    std::string api_base = "https://api.minimaxi.com/v1";
    std::string model = "MiniMax-M2.7";
};

struct ServerConfig {
    std::string host = "127.0.0.1";
    int port = 9615;
    int max_connections = 128;
    int worker_threads = 4;
    std::string default_database = "defaultdb";
};

struct StorageConfig {
    std::string data_dir = "./data";
    std::string wal_dir = "./data/wal";
    int buffer_size_mb = 64;
    int checkpoint_interval = 5000; // records between checkpoints
};

struct NodeConfig {
    std::string id = "node-1";
    std::string role = "standalone"; // standalone | leader | follower
    std::vector<std::string> peers;
};

struct DBConfig {
    ServerConfig server;
    StorageConfig storage;
    AIConfig ai;
    NodeConfig node;
    bool verbose = false;

    static DBConfig load(const std::string& path) {
        DBConfig cfg;
        std::ifstream f(path);
        if (!f.good()) {
            std::cerr << "[配置] 未找到配置文件 " << path << "，使用默认配置\n";
            return cfg;
        }
        try {
            std::stringstream ss;
            ss << f.rdbuf();
            json j = json::parse(ss.str());

            if (j.contains("server")) {
                auto& s = j["server"];
                if (s.contains("host")) cfg.server.host = s["host"].get_string();
                if (s.contains("port")) cfg.server.port = (int)s["port"].get_int();
                if (s.contains("max_connections")) cfg.server.max_connections = (int)s["max_connections"].get_int();
                if (s.contains("worker_threads")) cfg.server.worker_threads = (int)s["worker_threads"].get_int();
                if (s.contains("default_database")) cfg.server.default_database = s["default_database"].get_string();
            }
            if (j.contains("storage")) {
                auto& s = j["storage"];
                if (s.contains("data_dir")) cfg.storage.data_dir = s["data_dir"].get_string();
                if (s.contains("wal_dir")) cfg.storage.wal_dir = s["wal_dir"].get_string();
                if (s.contains("buffer_size_mb")) cfg.storage.buffer_size_mb = (int)s["buffer_size_mb"].get_int();
                if (s.contains("checkpoint_interval")) cfg.storage.checkpoint_interval = (int)s["checkpoint_interval"].get_int();
            }
            if (j.contains("ai")) {
                auto& a = j["ai"];
                if (a.contains("provider")) cfg.ai.provider = a["provider"].get_string();
                if (a.contains("api_key")) cfg.ai.api_key = a["api_key"].get_string();
                if (a.contains("api_base")) cfg.ai.api_base = a["api_base"].get_string();
                if (a.contains("model")) cfg.ai.model = a["model"].get_string();
            }
            if (j.contains("node")) {
                auto& n = j["node"];
                if (n.contains("id")) cfg.node.id = n["id"].get_string();
                if (n.contains("role")) cfg.node.role = n["role"].get_string();
                if (n.contains("peers")) {
                    for (const auto& p : n["peers"]) {
                        cfg.node.peers.push_back(p.get_string());
                    }
                }
            }
            if (j.contains("verbose")) cfg.verbose = j["verbose"].get_bool();
        } catch (const std::exception& e) {
            std::cerr << "[配置] 配置文件解析失败: " << e.what() << "，使用默认配置\n";
        }
        return cfg;
    }
};

#endif // JIAMIAODB_CONFIG_H
