/* ═══════════════════════════════════════════════════════════════════════
   O-2: Checkpoint 二进制 codec 测试
   ═══════════════════════════════════════════════════════════════════════ */

#include "doctest.h"
#include "storage/checkpoint.h"
#include "storage/checkpoint_codec.h"
#include "common/json.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cstring>
#include <unistd.h>

using json = Json;
using namespace jiamiao;

namespace {

Checkpoint make_sample_checkpoint() {
    Checkpoint ckp;
    ckp.version = 2;
    ckp.last_seq = 12345;
    ckp.timestamp = 1700000000;
    ckp.next_xid = 100;

    // 1 张表
    TableSchema t;
    t.name = "defaultdb.public.users";
    t.row_count = 3;
    ColumnDef c1{"id",   DataType::BIGINT, false, true};
    ColumnDef c2{"name", DataType::TEXT,   true,  false};
    ColumnDef c3{"age",  DataType::BIGINT, true,  false};
    t.columns = {c1, c2, c3};
    ckp.tables.push_back(t);

    // 行数据
    Row r1; r1["id"] = int64_t(1); r1["name"] = std::string("alice"); r1["age"] = int64_t(30);
    Row r2; r2["id"] = int64_t(2); r2["name"] = std::string("bob");   r2["age"] = int64_t(25);
    Row r3; r3["id"] = int64_t(3); r3["name"] = std::string("carol"); r3["age"] = nullptr;
    ckp.table_rows[t.name] = {{1, r1}, {2, r2}, {3, r3}};
    ckp.row_id_counters[t.name] = 3;

    // 索引
    IndexInfo idx;
    idx.column = "name";
    idx.entries["alice"] = {1};
    idx.entries["bob"]   = {2};
    idx.entries["carol"] = {3};
    ckp.indexes_data[t.name] = {idx};

    // 嵌入 JSON (clog/catalog O-3/O-4 替换)
    ckp.clog_entries = json::object();
    ckp.clog_entries["xid_1"] = "COMMITTED";
    ckp.catalog_data = json::object();
    json db_arr = json::array();
    db_arr.push_back(json("defaultdb"));
    ckp.catalog_data["databases"] = db_arr;
    return ckp;
}

}  // namespace

TEST_CASE("O-2: checkpoint binary round-trip preserves all fields") {
    Checkpoint orig = make_sample_checkpoint();
    std::string bin = orig.to_binary();
    REQUIRE(!bin.empty());

    Checkpoint ckp;
    std::string clog_json, catalog_json;
    REQUIRE(Checkpoint::from_binary(bin, &ckp, &clog_json, &catalog_json));

    CHECK(ckp.version   == 2);
    CHECK(ckp.last_seq  == 12345);
    CHECK(ckp.timestamp == 1700000000);
    CHECK(ckp.next_xid  == 100u);

    REQUIRE(ckp.tables.size() == 1);
    CHECK(ckp.tables[0].name == "defaultdb.public.users");
    CHECK(ckp.tables[0].row_count == 3);
    REQUIRE(ckp.tables[0].columns.size() == 3);
    CHECK(ckp.tables[0].columns[0].name == "id");
    CHECK(ckp.tables[0].columns[0].primary_key == true);
    CHECK(ckp.tables[0].columns[2].nullable == true);

    REQUIRE(ckp.table_rows.size() == 1);
    const auto& rows = ckp.table_rows["defaultdb.public.users"];
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].first == 1);
    CHECK(std::get<int64_t>(rows[0].second.at("id")) == 1);
    CHECK(std::get<std::string>(rows[0].second.at("name")) == "alice");
    CHECK(std::holds_alternative<std::nullptr_t>(rows[2].second.at("age")));

    CHECK(ckp.row_id_counters["defaultdb.public.users"] == 3);

    REQUIRE(ckp.indexes_data.size() == 1);
    REQUIRE(ckp.indexes_data["defaultdb.public.users"].size() == 1);
    const auto& idx = ckp.indexes_data["defaultdb.public.users"][0];
    CHECK(idx.column == "name");
    CHECK(idx.entries.at("alice") == std::vector<int64_t>{1});

    // 嵌入 JSON 应能 parse 回原 json
    json clog_p = json::parse(clog_json);
    CHECK(clog_p["xid_1"].get_string() == "COMMITTED");
    json cat_p = json::parse(catalog_json);
    REQUIRE(cat_p["databases"].size() == 1);
    CHECK(cat_p["databases"][0].get_string() == "defaultdb");
}

TEST_CASE("O-2: from_binary rejects truncated payload") {
    Checkpoint orig = make_sample_checkpoint();
    std::string bin = orig.to_binary();
    auto truncated = bin.substr(0, bin.size() / 2);
    Checkpoint ckp;
    std::string a, b;
    CHECK_FALSE(Checkpoint::from_binary(truncated, &ckp, &a, &b));
}

TEST_CASE("O-2: from_binary rejects unknown format version") {
    Checkpoint orig = make_sample_checkpoint();
    std::string bin = orig.to_binary();
    // 覆写前 4B format version
    bin[0] = '\xff'; bin[1] = '\xff'; bin[2] = '\xff'; bin[3] = '\xff';
    Checkpoint ckp;
    std::string a, b;
    CHECK_FALSE(Checkpoint::from_binary(bin, &ckp, &a, &b));
}

TEST_CASE("O-2: empty checkpoint round-trip is identity") {
    Checkpoint ckp;
    std::string bin = ckp.to_binary();
    Checkpoint out;
    std::string a, b;
    REQUIRE(Checkpoint::from_binary(bin, &out, &a, &b));
    CHECK(out.version == 1);
    CHECK(out.last_seq == 0);
    CHECK(out.tables.empty());
    CHECK(out.table_rows.empty());
    CHECK(a.empty());  // null clog → empty string
    CHECK(b.empty());
}

TEST_CASE("O-2: long string value in row survives round-trip") {
    Checkpoint ckp;
    TableSchema t; t.name = "t"; t.row_count = 1;
    ColumnDef c{"bio", DataType::TEXT, true, false};
    t.columns = {c};
    ckp.tables.push_back(t);

    Row r;
    r["bio"] = std::string(
        "distributed systems, databases, and the\n"
        "occasional strongly-typed language rant.\n"
        "中文 / emoji 🎉 / control \x01\x02 bytes");
    ckp.table_rows["t"] = {{1, r}};

    std::string bin = ckp.to_binary();
    Checkpoint out;
    std::string a, b;
    REQUIRE(Checkpoint::from_binary(bin, &out, &a, &b));
    REQUIRE(out.table_rows["t"].size() == 1);
    CHECK(std::get<std::string>(out.table_rows["t"][0].second["bio"])
          == std::get<std::string>(r["bio"]));
}

TEST_CASE("O-2: CheckpointManager.save writes binary file with magic; load reads it back") {
    namespace fs = std::filesystem;
    fs::path tmp = fs::temp_directory_path() / "jmdb_o2_test";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    {
        CheckpointManager mgr(tmp.string());
        Checkpoint orig = make_sample_checkpoint();
        mgr.save(orig);

        // 文件应存在, 且前 8B 是 magic
        auto path = mgr.checkpoint_path();
        REQUIRE(fs::exists(path));
        std::ifstream f(path, std::ios::binary);
        std::stringstream ss; ss << f.rdbuf();
        std::string all = ss.str();
        REQUIRE(all.size() >= 8);
        CHECK(std::memcmp(all.data(), kCheckpointMagic, kCheckpointMagicLen) == 0);
    }
    {
        // 重新打开, load 走二进制路径
        CheckpointManager mgr(tmp.string());
        auto ckp = mgr.load();
        CHECK(ckp.last_seq == 12345);
        CHECK(ckp.tables.size() == 1);
        CHECK(ckp.table_rows["defaultdb.public.users"].size() == 3);
        CHECK(ckp.row_id_counters["defaultdb.public.users"] == 3);
    }
    fs::remove_all(tmp);
}

TEST_CASE("O-2: CheckpointManager.load falls back to legacy JSON (checkpoint.json)") {
    namespace fs = std::filesystem;
    fs::path tmp = fs::temp_directory_path() / "jmdb_o2_legacy_test";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    // 写入旧格式 JSON 文件
    auto legacy = tmp / "checkpoint.json";
    {
        std::ofstream f(legacy.string());
        f << R"({"version":1,"last_seq":777,"tables":[],"next_xid":42})";
    }
    {
        CheckpointManager mgr(tmp.string());
        auto ckp = mgr.load();
        CHECK(ckp.last_seq == 777);
        CHECK(ckp.next_xid == 42u);
    }
    fs::remove_all(tmp);
}

TEST_CASE("O-2: CheckpointManager.load returns empty Checkpoint on missing file") {
    namespace fs = std::filesystem;
    fs::path tmp = fs::temp_directory_path() / "jmdb_o2_missing_test";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    CheckpointManager mgr(tmp.string());
    auto ckp = mgr.load();
    CHECK(ckp.last_seq == 0);
    CHECK(ckp.tables.empty());
    fs::remove_all(tmp);
}

TEST_CASE("O-2: binary file size is comparable to JSON; win is CPU, not bytes") {
    // 与 O-1 同样的认知: 二进制的 win 不是字节数, 是 CPU (无 json parse/dump)
    // 1000 行, 每行 5 列混合类型
    Checkpoint ckp;
    TableSchema t; t.name = "t"; t.row_count = 1000;
    t.columns = {
        {"id",    DataType::BIGINT, false, true},
        {"name",  DataType::TEXT,   true,  false},
        {"age",   DataType::BIGINT, true,  false},
        {"score", DataType::DOUBLE, true,  false},
        {"active", DataType::BOOLEAN, true, false},
    };
    ckp.tables.push_back(t);
    for (int i = 0; i < 1000; ++i) {
        Row r;
        r["id"]     = int64_t(i);
        r["name"]   = std::string("user_name_xxxxxxxxxxx");
        r["age"]    = int64_t(20 + (i % 50));
        r["score"]  = 50.0 + (i % 100) * 0.5;
        r["active"] = (i % 2 == 0);
        ckp.table_rows["t"].emplace_back(i, std::move(r));
    }
    std::string bin = ckp.to_binary();
    std::string js  = ckp.to_json().dump();
    double ratio = static_cast<double>(bin.size()) / js.size();
    MESSAGE("binary=", bin.size(), " json=", js.size(), " ratio=", ratio);
    // 比例在 [0.5x, 2.0x] 视为可接受. 实际典型 0.9x ~ 1.1x
    CHECK(ratio > 0.5);
    CHECK(ratio < 2.0);
}
