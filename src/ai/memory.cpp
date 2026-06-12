#include "memory.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cmath>
#include "common/json.h"
#include <iostream>

using json = Json;

WorkloadMemory::WorkloadMemory() {}

void WorkloadMemory::record(const ExecutionRecord& exec) {
    std::lock_guard<std::mutex> lock(mutex_);
    records_.push_back(exec);
    learn_patterns(exec);
}

void WorkloadMemory::save(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    json j;
    j["records"] = json::array();
    // 只保存最近 1000 条
    size_t start = records_.size() > 1000 ? records_.size() - 1000 : 0;
    for (size_t i = start; i < records_.size(); i++) {
        const auto& r = records_[i];
        json rec;
        rec["id"] = r.id;
        rec["timestamp"] = r.timestamp;
        rec["intent_hash"] = r.intent_hash;
        rec["description"] = r.description;
        rec["category"] = r.category;
        { json arr = json::array(); for (const auto& t : r.tables) arr.push_back(t); rec["tables"] = arr; }
        rec["planned_strategy"] = r.planned_strategy;
        rec["estimated_duration_ms"] = r.estimated_duration_ms;
        rec["actual_duration_ms"] = r.actual_duration_ms;
        rec["rows_scanned"] = r.rows_scanned;
        rec["rows_returned"] = r.rows_returned;
        rec["affected_rows"] = r.affected_rows;
        { json arr = json::array(); for (const auto& i : r.issues) arr.push_back(i); rec["issues"] = arr; }
        rec["accuracy"] = r.accuracy;
        j["records"].push_back(rec);
    }

    j["patterns"] = json::array();
    for (const auto& p : patterns_) {
        json pj;
        pj["id"] = p.id;
        pj["type"] = p.type;
        pj["description"] = p.description;
        { json arr = json::array(); for (const auto& h : p.related_intent_hashes) arr.push_back(h); pj["related_intent_hashes"] = arr; }
        pj["suggested_strategy"] = p.suggested_strategy;
        pj["suggestion_reason"] = p.suggestion_reason;
        pj["confidence"] = p.confidence;
        pj["first_seen"] = p.first_seen;
        pj["last_updated"] = p.last_updated;
        j["patterns"].push_back(pj);
    }

    std::ofstream file(path);
    if (file.is_open()) file << j.dump(2);
}

void WorkloadMemory::load(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ifstream file(path);
    if (!file.is_open()) return;

    try {
        std::stringstream ss;
        ss << file.rdbuf();
        json j = json::parse(ss.str());

        for (const auto& r : j["records"]) {
            ExecutionRecord rec;
            rec.id = r["id"].get_string();
            rec.timestamp = r.value("timestamp", (int64_t)0);
            rec.intent_hash = r.value("intent_hash", std::string(""));
            rec.description = r.value("description", std::string(""));
            rec.category = r.value("category", std::string(""));
            if (r.contains("tables")) {
                for (const auto& t : r["tables"]) rec.tables.push_back(t.get_string());
            }
            rec.planned_strategy = r.value("planned_strategy", std::string(""));
            rec.estimated_duration_ms = r.value("estimated_duration_ms", (int64_t)0);
            rec.actual_duration_ms = r.value("actual_duration_ms", (int64_t)0);
            rec.rows_scanned = r.value("rows_scanned", (int64_t)0);
            rec.rows_returned = r.value("rows_returned", (int64_t)0);
            rec.affected_rows = r.value("affected_rows", (int64_t)0);
            if (r.contains("issues")) {
                for (const auto& i : r["issues"]) rec.issues.push_back(i.get_string());
            }
            rec.accuracy = r.value("accuracy", 1.0);
            records_.push_back(rec);
        }

        for (const auto& p : j["patterns"]) {
            Pattern pat;
            pat.id = p.value("id", std::string(""));
            pat.type = p.value("type", std::string(""));
            pat.description = p.value("description", std::string(""));
            if (p.contains("related_intent_hashes")) {
                for (const auto& h : p["related_intent_hashes"]) pat.related_intent_hashes.push_back(h.get_string());
            }
            pat.suggested_strategy = p.value("suggested_strategy", std::string(""));
            pat.suggestion_reason = p.value("suggestion_reason", std::string(""));
            pat.confidence = p.value("confidence", 0.0);
            pat.first_seen = p.value("first_seen", (int64_t)0);
            pat.last_updated = p.value("last_updated", (int64_t)0);
            patterns_.push_back(pat);
        }
    } catch (...) {}
}

std::vector<ExecutionRecord> WorkloadMemory::find_similar(const std::string& intent_hash,
                                                           const std::string& category,
                                                           const std::vector<std::string>& tables) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ExecutionRecord> result;
    for (const auto& r : records_) {
        if (r.intent_hash == intent_hash && r.category == category) {
            // Check table overlap
            for (const auto& t : tables) {
                if (std::find(r.tables.begin(), r.tables.end(), t) != r.tables.end()) {
                    result.push_back(r);
                    break;
                }
            }
        }
    }
    return result;
}

StrategyStat* WorkloadMemory::get_best_strategy(const std::string& intent_hash) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::map<std::string, StrategyStat> stats;
    for (const auto& r : records_) {
        if (r.intent_hash == intent_hash) {
            stats[r.planned_strategy].strategy = r.planned_strategy;
            stats[r.planned_strategy].total_duration += r.actual_duration_ms;
            stats[r.planned_strategy].count++;
        }
    }

    StrategyStat* best = nullptr;
    double best_avg = 0;
    for (auto& [name, stat] : stats) {
        double avg = stat.avg_duration();
        if (!best || avg < best_avg) {
            best = &stat;
            best_avg = avg;
        }
    }
    return best;
}

std::vector<Pattern> WorkloadMemory::get_patterns() {
    std::lock_guard<std::mutex> lock(mutex_);
    return patterns_;
}

void WorkloadMemory::learn_patterns(const ExecutionRecord& rec) {
    detect_periodic_job(rec);
    detect_frequent_query(rec);
    detect_strategy_deviation(rec);
}

void WorkloadMemory::detect_periodic_job(const ExecutionRecord& rec) {
    // 找同样查询的历史
    std::vector<ExecutionRecord> same;
    for (const auto& r : records_) {
        if (r.intent_hash == rec.intent_hash) {
            same.push_back(r);
        }
    }
    if (same.size() < 3) return;

    // 检查时间间隔
    std::vector<int64_t> intervals;
    for (size_t i = 1; i < same.size(); i++) {
        intervals.push_back(same[i].timestamp - same[i-1].timestamp);
    }

    if (intervals.size() < 2) return;

    double avg = 0;
    for (auto d : intervals) avg += d;
    avg /= intervals.size();

    double max_dev = 0;
    for (auto d : intervals) max_dev = std::max(max_dev, std::abs(d - avg) / avg);

    if (max_dev > 0.2) return;

    int64_t day_ms = 86400000;
    int64_t hour_ms = 3600000;

    std::string period;
    if (avg > day_ms * 0.9 && avg < day_ms * 1.1) period = "daily";
    else if (avg > hour_ms * 0.9 && avg < hour_ms * 1.1) period = "hourly";
    else return;

    // 检查模式是否已存在
    for (auto& p : patterns_) {
        if (p.type == "periodic_job") {
            for (const auto& h : p.related_intent_hashes) {
                if (h == rec.intent_hash) {
                    p.last_updated = time(nullptr);
                    p.confidence = std::min(p.confidence + 0.05, 0.95);
                    return;
                }
            }
        }
    }

    Pattern pat;
    pat.id = "pattern-" + std::to_string(time(nullptr));
    pat.type = "periodic_job";
    pat.description = "查询 \"" + rec.description + "\" 每" + (period == "daily" ? "天" : "小时") + "定期执行";
    pat.related_intent_hashes = {rec.intent_hash};
    pat.suggested_strategy = rec.planned_strategy;
    pat.suggestion_reason = "检测到" + period + "周期模式，已执行" + std::to_string(same.size()) + "次";
    pat.confidence = std::min(0.5 + same.size() * 0.1, 0.95);
    pat.first_seen = same[0].timestamp;
    pat.last_updated = time(nullptr);
    patterns_.push_back(pat);
}

void WorkloadMemory::detect_frequent_query(const ExecutionRecord& rec) {
    int64_t window = 3600000; // 1 hour
    int64_t now = rec.timestamp;

    int count = 0;
    for (const auto& r : records_) {
        if (r.intent_hash == rec.intent_hash && now - r.timestamp < window) {
            count++;
        }
    }
    if (count < 5) return;

    for (auto& p : patterns_) {
        if (p.type == "frequent_query") {
            for (const auto& h : p.related_intent_hashes) {
                if (h == rec.intent_hash) return;
            }
        }
    }

    Pattern pat;
    pat.id = "pattern-hot-" + std::to_string(time(nullptr));
    pat.type = "frequent_query";
    pat.description = "查询 \"" + rec.description + "\" 是热点查询（1小时内执行" + std::to_string(count) + "次）";
    pat.related_intent_hashes = {rec.intent_hash};
    pat.suggested_strategy = rec.planned_strategy;
    pat.suggestion_reason = "高频查询，建议缓存或预执行";
    pat.confidence = std::min(0.6 + count * 0.05, 0.9);
    pat.first_seen = time(nullptr);
    pat.last_updated = time(nullptr);
    patterns_.push_back(pat);
}

void WorkloadMemory::detect_strategy_deviation(const ExecutionRecord& rec) {
    if (rec.accuracy > 2.0 || rec.accuracy < 0.3) {
        for (auto& p : patterns_) {
            if (p.type == "data_skew") {
                for (const auto& h : p.related_intent_hashes) {
                    if (h == rec.intent_hash) {
                        p.last_updated = time(nullptr);
                        p.confidence = std::min(p.confidence + 0.1, 0.95);
                        return;
                    }
                }
            }
        }

        Pattern pat;
        pat.id = "pattern-skew-" + std::to_string(time(nullptr));
        pat.type = "data_skew";
        pat.description = "查询 \"" + rec.description + "\" 的策略偏差: 预估" +
                         std::to_string(rec.estimated_duration_ms) + "ms, 实际" +
                         std::to_string(rec.actual_duration_ms) + "ms";
        pat.related_intent_hashes = {rec.intent_hash};
        pat.suggested_strategy = "re-evaluate";
        pat.suggestion_reason = "数据分布可能发生了变化，之前的最优策略不再适用";
        pat.confidence = 0.7;
        pat.first_seen = time(nullptr);
        pat.last_updated = time(nullptr);
        patterns_.push_back(pat);
    }
}
