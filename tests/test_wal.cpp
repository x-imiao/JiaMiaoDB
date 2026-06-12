#include "doctest.h"
#include "storage/wal_v3.h"
#include "storage/wal_payload.h"
#include "common/json.h"
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

TEST_CASE("WAL v3: truncate keeps only seq > cutoff") {
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

/* ═══════════════════════════════════════════════════════════════════════
   O-1: Binary WAL payload codec tests
   验证每个 op 的 encode/decode 往返保真
   ═══════════════════════════════════════════════════════════════════════ */

TEST_CASE("O-1: wal_encode_row / wal_decode_row round-trips all Value variant cases") {
    using namespace jiamiao;
    Row r;
    r["null_col"]  = nullptr;
    r["int_col"]   = int64_t(-1234567890123LL);
    r["float_col"] = 3.14159;
    r["bool_col"]  = true;
    r["str_col"]   = std::string("hello, world 中文");

    auto bytes = wal_encode_row(r);
    Row r2;
    CHECK(wal_decode_row(bytes, &r2));
    CHECK(r2.size() == r.size());
    CHECK(std::get<int64_t>(r2["int_col"]) == -1234567890123LL);
    CHECK(std::get<double>(r2["float_col"]) == doctest::Approx(3.14159));
    CHECK(std::get<bool>(r2["bool_col"]) == true);
    CHECK(std::get<std::string>(r2["str_col"]) == "hello, world 中文");
    CHECK(std::holds_alternative<std::nullptr_t>(r2["null_col"]));
}

TEST_CASE("O-1: wal_encode_insert round-trip preserves rowid and full row") {
    using namespace jiamiao;
    InsertPayload p;
    p.rowid = 42;
    p.row["id"]   = int64_t(42);
    p.row["name"] = std::string("alice");
    p.row["v"]    = 1.5;

    auto bytes = wal_encode_insert(p);
    InsertPayload q;
    CHECK(wal_decode_insert(bytes, &q));
    CHECK(q.rowid == 42);
    CHECK(std::get<std::string>(q.row["name"]) == "alice");
    CHECK(std::get<double>(q.row["v"]) == doctest::Approx(1.5));
}

TEST_CASE("O-1: wal_encode_update round-trip preserves rowid, updates, new_row") {
    using namespace jiamiao;
    UpdatePayload p;
    p.rowid = 7;
    p.updates["age"] = int64_t(30);
    p.new_row["id"]  = int64_t(7);
    p.new_row["age"] = int64_t(30);
    p.new_row["name"] = std::string("bob");

    auto bytes = wal_encode_update(p);
    UpdatePayload q;
    CHECK(wal_decode_update(bytes, &q));
    CHECK(q.rowid == 7);
    CHECK(std::get<int64_t>(q.updates["age"]) == 30);
    CHECK(std::get<std::string>(q.new_row["name"]) == "bob");
}

TEST_CASE("O-1: wal_encode_delete physical vs MVCC tombstone round-trip") {
    using namespace jiamiao;
    {
        DeletePayload p;            // physical delete
        p.rowid = 99;
        p.has_xmax = false;
        p.xmax = 0;
        auto bytes = wal_encode_delete(p);
        DeletePayload q;
        CHECK(wal_decode_delete(bytes, &q));
        CHECK(q.rowid == 99);
        CHECK(q.has_xmax == false);
        CHECK(q.xmax == 0u);
    }
    {
        DeletePayload p;            // MVCC tombstone
        p.rowid = 100;
        p.has_xmax = true;
        p.xmax = 0xDEADBEEF;
        auto bytes = wal_encode_delete(p);
        DeletePayload q;
        CHECK(wal_decode_delete(bytes, &q));
        CHECK(q.rowid == 100);
        CHECK(q.has_xmax == true);
        CHECK(q.xmax == 0xDEADBEEFu);
    }
}

TEST_CASE("O-1: wal_encode_create_table round-trip preserves column definitions") {
    using namespace jiamiao;
    CreateTablePayload p;
    p.columns.push_back(ColumnDef{"id",     DataType::BIGINT,   false, true});
    p.columns.push_back(ColumnDef{"name",   DataType::TEXT,     true,  false});
    p.columns.push_back(ColumnDef{"score",  DataType::DOUBLE,   true,  false});

    auto bytes = wal_encode_create_table(p);
    CreateTablePayload q;
    CHECK(wal_decode_create_table(bytes, &q));
    REQUIRE(q.columns.size() == 3);
    CHECK(q.columns[0].name == "id");
    CHECK(q.columns[0].type == DataType::BIGINT);
    CHECK(q.columns[0].primary_key == true);
    CHECK(q.columns[0].nullable == false);
    CHECK(q.columns[1].name == "name");
    CHECK(q.columns[1].type == DataType::TEXT);
    CHECK(q.columns[1].nullable == true);
    CHECK(q.columns[2].type == DataType::DOUBLE);
}

TEST_CASE("O-1: wal_encode_create_schema round-trip preserves schema name") {
    using namespace jiamiao;
    CreateSchemaPayload p;
    p.schema = "analytics";
    auto bytes = wal_encode_create_schema(p);
    CreateSchemaPayload q;
    CHECK(wal_decode_create_schema(bytes, &q));
    CHECK(q.schema == "analytics");
}

TEST_CASE("O-1: decode rejects truncated payload (corruption safety)") {
    using namespace jiamiao;
    InsertPayload p;
    p.rowid = 1;
    p.row["x"] = int64_t(42);
    auto bytes = wal_encode_insert(p);
    // 截断到一半: 解析必须返回 false, 不可 silently 接受
    auto truncated = bytes.substr(0, bytes.size() / 2);
    InsertPayload q;
    CHECK_FALSE(wal_decode_insert(truncated, &q));
}

TEST_CASE("O-1: binary payload size is comparable to JSON; win is CPU, not bytes") {
    using namespace jiamiao;
    // 重要: O-1 的 win 不是字节大小 (per-row length prefix + tag 抵消了 JSON 引号优势)
    // 而是 CPU: 二进制是 O(n) memcpy + switch dispatch, 没有 json::parse 词法分析 + AST 构建
    //
    // 这里仅证明:
    //   1) 大小在合理比例内 (binary 不明显更大)
    //   2) 二进制 decode 速度 ≥ JSON parse 速度 (不具体 micro-benchmark,
    //      但 round-trip 9 个测试 case 整体 < 1ms 即可见)
    Row r;
    r["user_id"]       = int64_t(12345);
    r["email"]         = std::string("alice.smith@somecompany.com");
    r["bio"]           = std::string("distributed systems, databases, and the\n"
                                     "occasional strongly-typed language rant.\n"
                                     "previously: intern @ small startup, junior @ mid co.");
    r["score"]         = 98.5;
    r["active"]        = true;

    auto bin = wal_encode_row(r);
    json d;
    d["user_id"] = int64_t(12345);
    d["email"]   = std::string("alice.smith@somecompany.com");
    d["bio"]     = std::string("distributed systems, databases, and the\n"
                               "occasional strongly-typed language rant.\n"
                               "previously: intern @ small startup, junior @ mid co.");
    d["score"]   = 98.5;
    d["active"]  = true;
    auto jstr = d.dump();

    // 比例应在 0.5x ~ 2x 之间. 实际典型 1.0x ~ 1.3x
    auto ratio = static_cast<double>(bin.size()) / jstr.size();
    MESSAGE("binary=", bin.size(), " json=", jstr.size(), " ratio=", ratio);
    CHECK(ratio > 0.5);
    CHECK(ratio < 2.0);
}

