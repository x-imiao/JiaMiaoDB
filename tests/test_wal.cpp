#include "doctest.h"
#include "storage/wal.h"
#include "json.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unistd.h>

using json = Json;

namespace {

// RAII helper: temporary directory for WAL test files
struct TempWalDir {
    std::filesystem::path dir;
    TempWalDir() {
        dir = std::filesystem::temp_directory_path() /
              ("jmdb_wal_test_" + std::to_string(::getpid()) + "_" +
               std::to_string(reinterpret_cast<uintptr_t>(this)));
        std::filesystem::create_directories(dir);
    }
    ~TempWalDir() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    std::string path(const std::string& name) const {
        return (dir / name).string();
    }
};

WALRecord make_rec(int64_t seq, const std::string& op, const std::string& table,
                   const std::string& key) {
    WALRecord r;
    r.seq = seq;
    r.timestamp = 1700000000 + seq;
    r.op = op;
    r.table = table;
    json d;
    d["key"] = key;
    r.data = d;
    return r;
}

} // namespace

TEST_CASE("WAL: open/append/close appends a JSON line per record") {
    TempWalDir tmp;
    std::string p = tmp.path("wal.log");

    WriteAheadLog wal(p);
    wal.open();
    wal.append(make_rec(1, "insert", "users", "u1"));
    wal.append(make_rec(2, "insert", "users", "u2"));
    wal.sync();
    wal.close();

    // File exists, contains 2 lines
    std::ifstream in(p);
    std::string line;
    int count = 0;
    while (std::getline(in, line)) {
        if (!line.empty()) ++count;
    }
    CHECK(count == 2);
}

TEST_CASE("WAL: replay returns records in file order, filtered by from_seq") {
    TempWalDir tmp;
    std::string p = tmp.path("wal.log");

    WriteAheadLog wal(p);
    wal.open();
    wal.append(make_rec(10, "insert", "t", "a"));
    wal.append(make_rec(20, "insert", "t", "b"));
    wal.append(make_rec(30, "insert", "t", "c"));
    wal.close();

    auto recs = wal.replay(15);  // skip seq <= 15
    CHECK(recs.size() == 2);
    CHECK(recs[0].seq == 20);
    CHECK(recs[1].seq == 30);
}

TEST_CASE("WAL: to_json includes WAL_FORMAT_VERSION field 'v'") {
    WALRecord r = make_rec(1, "insert", "t", "k");
    json j = r.to_json();
    CHECK(j.contains("v"));
    CHECK(j["v"].get_int() == WAL_FORMAT_VERSION);
    CHECK(j["v"].get_int() == 2);
    CHECK(j.contains("seq"));
    CHECK(j.contains("op"));
    CHECK(j.contains("table"));
    CHECK(j.contains("data"));
}

TEST_CASE("WAL: from_json tolerates missing 'v' field (backward compat with v1)") {
    // 模拟旧格式: 不包含 "v" 字段
    json j;
    j["seq"] = 42;
    j["ts"] = 1234;
    j["op"] = "insert";
    j["table"] = "users";
    json data;
    data["key"] = "u1";
    j["data"] = data;

    // 不应抛异常
    WALRecord r = WALRecord::from_json(j);
    CHECK(r.seq == 42);
    CHECK(r.timestamp == 1234);
    CHECK(r.op == "insert");
    CHECK(r.table == "users");
}

TEST_CASE("WAL: from_json tolerates missing 'ts' field (older records)") {
    json j;
    j["seq"] = 1;
    j["op"] = "insert";
    j["table"] = "t";
    j["data"] = json::object();

    WALRecord r = WALRecord::from_json(j);
    CHECK(r.seq == 1);
    CHECK(r.timestamp == 0);  // 默认值
}

TEST_CASE("WAL: old-format file (no 'v' field) replays correctly") {
    TempWalDir tmp;
    std::string p = tmp.path("wal_old.log");

    // 手动写入旧格式记录
    {
        std::ofstream out(p, std::ios::trunc);
        json r1;
        r1["seq"] = 1;
        r1["ts"] = 100;
        r1["op"] = "insert";
        r1["table"] = "t";
        r1["data"] = json::object();
        out << r1.dump() << "\n";

        json r2;
        r2["seq"] = 2;
        r2["ts"] = 200;
        r2["op"] = "update";
        r2["table"] = "t";
        r2["data"] = json::object();
        out << r2.dump() << "\n";
    }

    WriteAheadLog wal(p);
    auto recs = wal.replay(0);
    CHECK(recs.size() == 2);
    CHECK(recs[0].seq == 1);
    CHECK(recs[0].op == "insert");
    CHECK(recs[1].seq == 2);
    CHECK(recs[1].op == "update");
}

TEST_CASE("WAL: truncate removes records with seq <= cutoff and preserves later ones") {
    TempWalDir tmp;
    std::string p = tmp.path("wal.log");

    WriteAheadLog wal(p);
    wal.open();
    wal.append(make_rec(1, "insert", "t", "a"));
    wal.append(make_rec(2, "insert", "t", "b"));
    wal.append(make_rec(3, "insert", "t", "c"));
    wal.append(make_rec(4, "insert", "t", "d"));
    wal.close();

    // 截断到 seq <= 2
    wal.open();
    wal.truncate(2);
    wal.close();

    // 重放: 应当只剩 seq=3, seq=4
    auto recs = wal.replay(0);
    CHECK(recs.size() == 2);
    CHECK(recs[0].seq == 3);
    CHECK(recs[1].seq == 4);
}

TEST_CASE("WAL: truncate with cutoff >= max_seq empties the file") {
    TempWalDir tmp;
    std::string p = tmp.path("wal.log");

    WriteAheadLog wal(p);
    wal.open();
    wal.append(make_rec(1, "insert", "t", "a"));
    wal.append(make_rec(2, "insert", "t", "b"));
    wal.close();

    wal.open();
    wal.truncate(100);  // 截断比所有 seq 都大
    wal.close();

    auto recs = wal.replay(0);
    CHECK(recs.empty());
}

TEST_CASE("WAL: truncate with cutoff < min_seq keeps everything") {
    TempWalDir tmp;
    std::string p = tmp.path("wal.log");

    WriteAheadLog wal(p);
    wal.open();
    wal.append(make_rec(10, "insert", "t", "a"));
    wal.append(make_rec(20, "insert", "t", "b"));
    wal.close();

    wal.open();
    wal.truncate(5);  // 比最小 seq 还小
    wal.close();

    auto recs = wal.replay(0);
    CHECK(recs.size() == 2);
    CHECK(recs[0].seq == 10);
    CHECK(recs[1].seq == 20);
}

TEST_CASE("WAL: truncate is idempotent on already-empty result") {
    TempWalDir tmp;
    std::string p = tmp.path("wal.log");

    WriteAheadLog wal(p);
    wal.open();
    wal.append(make_rec(1, "insert", "t", "a"));
    wal.append(make_rec(2, "insert", "t", "b"));
    wal.append(make_rec(3, "insert", "t", "c"));
    wal.close();

    wal.open();
    wal.truncate(1);  // 保留 seq=2,3
    CHECK(wal.replay(0).size() == 2);

    // 多次截断到同一值: 结果不变
    wal.truncate(1);
    wal.truncate(1);
    CHECK(wal.replay(0).size() == 2);

    // 截断到更大值: 全部移除
    wal.truncate(100);
    wal.truncate(100);  // 重复一次也无副作用
    wal.close();

    auto recs = wal.replay(0);
    CHECK(recs.empty());
}

TEST_CASE("WAL: truncate preserves 'v' field in kept records (v2 format)") {
    TempWalDir tmp;
    std::string p = tmp.path("wal.log");

    WriteAheadLog wal(p);
    wal.open();
    wal.append(make_rec(1, "insert", "t", "a"));
    wal.append(make_rec(2, "insert", "t", "b"));
    wal.close();

    wal.open();
    wal.truncate(1);
    wal.close();

    // 读回原始字节, 验证 v2 格式保留
    std::ifstream in(p);
    std::string line;
    std::getline(in, line);
    auto j = json::parse(line);
    CHECK(j["v"].get_int() == WAL_FORMAT_VERSION);
    CHECK(j["seq"].get_int() == 2);
}

TEST_CASE("WAL: append after truncate continues seq from kept records") {
    TempWalDir tmp;
    std::string p = tmp.path("wal.log");

    WriteAheadLog wal(p);
    wal.open();
    wal.append(make_rec(1, "insert", "t", "a"));
    wal.append(make_rec(2, "insert", "t", "b"));
    wal.append(make_rec(3, "insert", "t", "c"));
    wal.close();

    wal.open();
    wal.truncate(2);  // 只保留 seq=3
    // 重新打开后, 截断后 append 新记录
    wal.append(make_rec(4, "insert", "t", "d"));
    wal.close();

    auto recs = wal.replay(0);
    CHECK(recs.size() == 2);
    CHECK(recs[0].seq == 3);
    CHECK(recs[1].seq == 4);
}

TEST_CASE("WAL: mixed format file (old + new) replays in order") {
    TempWalDir tmp;
    std::string p = tmp.path("mixed.log");

    // 旧格式记录 (无 v 字段)
    {
        std::ofstream out(p, std::ios::trunc);
        json r1;
        r1["seq"] = 1;
        r1["op"] = "insert";
        r1["table"] = "t";
        r1["data"] = json::object();
        out << r1.dump() << "\n";
    }

    // 新格式记录 (有 v 字段) — 追加
    WriteAheadLog wal(p);
    wal.open();
    wal.append(make_rec(2, "update", "t", "b"));
    wal.close();

    auto recs = wal.replay(0);
    CHECK(recs.size() == 2);
    CHECK(recs[0].seq == 1);
    CHECK(recs[0].op == "insert");
    CHECK(recs[1].seq == 2);
    CHECK(recs[1].op == "update");
}

TEST_CASE("WAL: corrupt lines are silently skipped during replay") {
    TempWalDir tmp;
    std::string p = tmp.path("corrupt.log");

    {
        std::ofstream out(p, std::ios::trunc);
        out << "{not valid json\n";
        json r1;
        r1["seq"] = 1;
        r1["op"] = "insert";
        r1["table"] = "t";
        r1["data"] = json::object();
        out << r1.dump() << "\n";
        out << "garbage line\n";
        json r2;
        r2["seq"] = 2;
        r2["op"] = "insert";
        r2["table"] = "t";
        r2["data"] = json::object();
        out << r2.dump() << "\n";
    }

    WriteAheadLog wal(p);
    auto recs = wal.replay(0);
    CHECK(recs.size() == 2);
    CHECK(recs[0].seq == 1);
    CHECK(recs[1].seq == 2);
}

TEST_CASE("WAL: replay of non-existent file returns empty") {
    TempWalDir tmp;
    std::string p = tmp.path("missing.log");

    WriteAheadLog wal(p);
    auto recs = wal.replay(0);
    CHECK(recs.empty());
}

TEST_CASE("WAL: current_seq tracks the last appended record's seq") {
    TempWalDir tmp;
    std::string p = tmp.path("wal.log");

    WriteAheadLog wal(p);
    wal.open();
    wal.append(make_rec(5, "insert", "t", "a"));
    CHECK(wal.current_seq() == 5);
    wal.append(make_rec(10, "insert", "t", "b"));
    CHECK(wal.current_seq() == 10);
    // 注: 当前实现直接覆盖 seq_ (不取 max), 调用方应保证单调递增
    wal.append(make_rec(7, "insert", "t", "c"));
    CHECK(wal.current_seq() == 7);
    wal.close();
}
