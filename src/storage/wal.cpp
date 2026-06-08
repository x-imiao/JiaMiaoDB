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
