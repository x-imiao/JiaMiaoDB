#include "wal.h"
#include <filesystem>
#include <iostream>

WriteAheadLog::WriteAheadLog(const std::string& path) : path_(path) {}

WriteAheadLog::~WriteAheadLog() { close(); }

void WriteAheadLog::open() {
    file_.open(path_, std::ios::app | std::ios::binary);
    if (!file_.is_open()) {
        throw std::runtime_error("无法打开 WAL 文件: " + path_);
    }
}

void WriteAheadLog::close() {
    if (file_.is_open()) file_.close();
}

void WriteAheadLog::append(const WALRecord& rec) {
    std::lock_guard<std::mutex> lock(mutex_);
    seq_ = rec.seq;
    file_ << rec.to_json().dump() << "\n";
}

void WriteAheadLog::sync() {
    std::lock_guard<std::mutex> lock(mutex_);
    file_.flush();
}

void WriteAheadLog::truncate(int64_t cutoff_seq) {
    // 截断语义: 丢弃所有 seq <= cutoff_seq 的记录
    // 调用方负责: 已确保 cutoff_seq 之前的修改已落盘 (checkpoint 完成)
    std::lock_guard<std::mutex> lock(mutex_);

    // 1. 关闭当前文件
    if (file_.is_open()) file_.close();

    // 2. 读取所有现有记录
    std::vector<WALRecord> kept;
    if (std::filesystem::exists(path_)) {
        std::ifstream in(path_);
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            try {
                auto j = json::parse(line);
                auto rec = WALRecord::from_json(j);
                if (rec.seq > cutoff_seq) {
                    kept.push_back(std::move(rec));
                }
            } catch (...) {
                // 跳过损坏的行
            }
        }
    }

    // 3. 用保留的记录重写文件 (覆盖模式)
    {
        std::ofstream out(path_, std::ios::trunc | std::ios::binary);
        if (!out.is_open()) {
            throw std::runtime_error("无法重写 WAL 文件: " + path_);
        }
        for (const auto& rec : kept) {
            out << rec.to_json().dump() << "\n";
        }
        out.flush();
    }

    // 4. 重新以 append 模式打开
    file_.open(path_, std::ios::app | std::ios::binary);
    if (!file_.is_open()) {
        throw std::runtime_error("无法重新打开 WAL 文件: " + path_);
    }

    // 5. 更新内存中的 seq: 重写后, 当前 seq 应为保留记录中最大的
    if (!kept.empty()) {
        seq_ = kept.back().seq;
    }
}

std::vector<WALRecord> WriteAheadLog::replay(int64_t from_seq) {
    std::vector<WALRecord> records;
    if (!std::filesystem::exists(path_)) return records;

    std::ifstream file(path_);
    if (!file.is_open()) return records;

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        try {
            auto j = json::parse(line);
            auto rec = WALRecord::from_json(j);
            if (rec.seq > from_seq) {
                records.push_back(std::move(rec));
                seq_ = std::max(seq_, rec.seq);
            }
        } catch (...) {
            // 跳过损坏的行
        }
    }
    return records;
}
