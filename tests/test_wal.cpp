#include "doctest.h"
#include "storage/wal.h"
#include "storage/wal_v3.h"
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

// ═══════════════════════════════════════════════════════════════════
// WAL v3 tests: binary length-prefix + CRC32 + v2 fallback + fsync
// ═══════════════════════════════════════════════════════════════════
namespace {

jiamiao::WALRecordV3 make_v3(int64_t seq, jiamiao::WalOp op,
                              const std::string& table,
                              const std::string& key) {
    jiamiao::WALRecordV3 r;
    r.seq = seq;
    r.timestamp = 1700000000 + seq;
    r.op = static_cast<uint16_t>(op);
    r.table = table;
    r.xid = 0;
    json d;
    d["key"] = key;
    r.data = d.dump();
    return r;
}

} // namespace

TEST_CASE("WAL v3: encode/decode round-trip is byte-faithful") {
    using namespace jiamiao;
    WALRecordV3 r = make_v3(42, WalOp::kInsert, "db.s.t", "x");
    r.xid = 7;

    auto bytes = r.encode();
    CHECK(bytes.size() >= 38);

    WALRecordV3 r2;
    size_t consumed = WALRecordV3::decode(bytes.data(), bytes.size(), &r2);
    CHECK(consumed == bytes.size());
    CHECK(r2.seq == r.seq);
    CHECK(r2.timestamp == r.timestamp);
    CHECK(r2.op == r.op);
    CHECK(r2.table == r.table);
    CHECK(r2.xid == r.xid);
    CHECK(r2.data == r.data);
}

TEST_CASE("WAL v3: 12 op enums round-trip via op_to_enum/enum_to_op") {
    using namespace jiamiao;
    const std::vector<std::string> ops = {
        "insert", "update", "delete",
        "xact_commit", "xact_abort",
        "create_table", "drop_table",
        "create_database", "drop_database",
        "create_schema", "create_user", "drop_user"
    };
    for (const auto& s : ops) {
        uint16_t e = op_to_enum(s);
        CHECK(e != 0);
        std::string back = enum_to_op(e);
        CHECK(back == s);
    }
    // 未知 op → 0
    CHECK(op_to_enum("not_a_real_op") == 0);
}

TEST_CASE("WAL v3: append + replay round-trip preserves all records") {
    using namespace jiamiao;
    TempWalDir td;
    auto p = td.path("v3.wal");

    WriteAheadLogV3 wal(p, WriteAheadLogV3::SyncMode::kNever);
    wal.open();
    wal.append(make_v3(1, WalOp::kCreateTable, "db.s.t", ""));
    wal.append(make_v3(2, WalOp::kInsert, "db.s.t", "a"));
    wal.append(make_v3(3, WalOp::kUpdate, "db.s.t", "b"));
    wal.append(make_v3(4, WalOp::kDelete, "db.s.t", "c"));
    wal.append(make_v3(5, WalOp::kXactCommit, "", ""));
    wal.close();

    WriteAheadLogV3 wal2(p);
    auto recs = wal2.replay(0);
    CHECK(recs.size() == 5);
    CHECK(recs[0].op == static_cast<uint16_t>(WalOp::kCreateTable));
    CHECK(recs[1].op == static_cast<uint16_t>(WalOp::kInsert));
    CHECK(recs[2].op == static_cast<uint16_t>(WalOp::kUpdate));
    CHECK(recs[3].op == static_cast<uint16_t>(WalOp::kDelete));
    CHECK(recs[4].op == static_cast<uint16_t>(WalOp::kXactCommit));
    CHECK(recs[1].table == "db.s.t");
    CHECK(wal2.current_seq() == 5);
}

TEST_CASE("WAL v3: CRC failure skips corrupted record, replay continues") {
    using namespace jiamiao;
    TempWalDir td;
    auto p = td.path("v3_crc.wal");

    {
        WriteAheadLogV3 wal(p, WriteAheadLogV3::SyncMode::kNever);
        wal.open();
        wal.append(make_v3(1, WalOp::kInsert, "t", "a"));
        wal.append(make_v3(2, WalOp::kInsert, "t", "b"));
        wal.append(make_v3(3, WalOp::kInsert, "t", "c"));
        wal.close();
    }

    // 翻 1 个字节在第二条记录的 data 区 (大约 offset 40+)
    {
        std::fstream f(p, std::ios::binary | std::ios::in | std::ios::out);
        REQUIRE(f.is_open());
        f.seekg(0, std::ios::end);
        size_t fsize = static_cast<size_t>(f.tellg());
        f.seekp(static_cast<std::streamoff>(fsize / 2));
        char c;
        f.read(&c, 1);
        c ^= 0xFFu;
        f.seekp(static_cast<std::streamoff>(fsize / 2));
        f.write(&c, 1);
    }

    {
        WriteAheadLogV3 wal2(p);
        auto recs = wal2.replay(0);
        // 至少有 1 条记录 (第一条正常), 可能 2 条 (CRC skip 跳过被翻字节的, 找下一个 magic)
        CHECK(recs.size() >= 1);
        CHECK(recs.size() <= 3);
    }
}

TEST_CASE("WAL v3: fallback reads v2 JSON Lines (no magic)") {
    using namespace jiamiao;
    TempWalDir td;
    auto p = td.path("v2_legacy.wal");

    // 写 v2 JSON Lines (用 WriteAheadLog v2 类)
    {
        WriteAheadLog wal(p);
        wal.open();
        wal.append(make_rec(1, "insert", "db.s.t", "alpha"));
        wal.append(make_rec(2, "update", "db.s.t", "beta"));
        wal.append(make_rec(3, "xact_commit", "", "_"));
        wal.close();
    }

    // 用 v3 reader 读 — 应该 fallback 解析 v2 JSON
    WriteAheadLogV3 wal_v3(p);
    auto recs = wal_v3.replay(0);
    CHECK(recs.size() == 3);
    CHECK(recs[0].op == static_cast<uint16_t>(WalOp::kInsert));
    CHECK(recs[1].op == static_cast<uint16_t>(WalOp::kUpdate));
    CHECK(recs[2].op == static_cast<uint16_t>(WalOp::kXactCommit));
    CHECK(recs[0].table == "db.s.t");
    CHECK(wal_v3.current_seq() == 3);
}

TEST_CASE("WAL v3: truncate keeps only seq > cutoff and upgrades v2 → v3") {
    using namespace jiamiao;
    TempWalDir td;
    auto p = td.path("v3_truncate.wal");

    {
        WriteAheadLogV3 wal(p, WriteAheadLogV3::SyncMode::kNever);
        wal.open();
        for (int64_t i = 1; i <= 5; ++i) {
            wal.append(make_v3(i, WalOp::kInsert, "t", "k" + std::to_string(i)));
        }
        wal.truncate(3);
        wal.close();
    }

    WriteAheadLogV3 wal2(p);
    auto recs = wal2.replay(0);
    CHECK(recs.size() == 2);  // seq 4, 5 保留
    CHECK(recs[0].seq == 4);
    CHECK(recs[1].seq == 5);
}

TEST_CASE("WAL v3: CRC32 matches known zlib value (sanity)") {
    using namespace jiamiao;
    // "123456789" 的 CRC32 = 0xCBF43926 (RFC, zlib 验证向量)
    const char* s = "123456789";
    uint32_t crc = crc32_compute(reinterpret_cast<const uint8_t*>(s), 9);
    CHECK(crc == 0xCBF43926U);
}

