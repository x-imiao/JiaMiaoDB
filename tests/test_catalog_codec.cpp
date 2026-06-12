/* ═══════════════════════════════════════════════════════════════════════
   O-3: Catalog 二进制 codec 测试
   ═══════════════════════════════════════════════════════════════════════ */

#include "doctest.h"
#include "storage/catalog.h"
#include "storage/catalog_codec.h"
#include "storage/checkpoint_codec.h"
#include "storage/engine.h"
#include "types.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cstring>
#include <unistd.h>

using namespace jiamiao;

namespace {

Catalog make_sample_catalog() {
    Catalog c;
    // ctor 已创建 defaultdb/public
    c.create_database("analytics");  // 自带 public
    c.create_schema("defaultdb", "audit");
    c.create_schema("analytics", "staging");
    c.set_current_database("analytics");

    // users — 走 create_user 真实 hash + salt, 验证 round-trip 后 verify_password 仍能通过
    c.create_user("alice", "alice_pw");
    c.create_user("bob",   "bob_pw");
    return c;
}

}  // namespace

TEST_CASE("O-3: catalog binary round-trip preserves databases / schemas / users") {
    Catalog orig = make_sample_catalog();
    std::string bin = catalog_encode(orig);
    REQUIRE(!bin.empty());

    Catalog out;
    REQUIRE(catalog_decode(bin, &out));

    CHECK(out.list_databases().size() == 2);
    auto dbs = out.list_databases();
    // map 排序: analytics < defaultdb
    CHECK(dbs[0] == "analytics");
    CHECK(dbs[1] == "defaultdb");

    // schemas
    CHECK(out.list_schemas("defaultdb").size() == 2);
    CHECK(out.schema_exists("defaultdb", "public"));
    CHECK(out.schema_exists("defaultdb", "audit"));
    CHECK(out.list_schemas("analytics").size() == 2);
    CHECK(out.schema_exists("analytics", "staging"));

    // users (含 password_hash + salt)
    CHECK(out.list_users().size() == 2);
    CHECK(out.user_exists("alice"));
    CHECK(out.user_exists("bob"));
    // 真实 create_user 走 sha256(salt + password), round-trip 后 verify_password 必须通过
    CHECK(out.verify_password("alice", "alice_pw"));
    CHECK(out.verify_password("bob",   "bob_pw"));
    CHECK_FALSE(out.verify_password("alice", "wrong"));

    // current_db
    CHECK(out.current_database() == "analytics");
}

TEST_CASE("O-3: from_binary rejects truncated payload") {
    Catalog orig = make_sample_catalog();
    std::string bin = catalog_encode(orig);
    auto truncated = bin.substr(0, bin.size() / 2);
    Catalog out;
    CHECK_FALSE(catalog_decode(truncated, &out));
}

TEST_CASE("O-3: from_binary rejects unknown format version") {
    Catalog orig = make_sample_catalog();
    std::string bin = catalog_encode(orig);
    bin[0] = '\xff'; bin[1] = '\xff'; bin[2] = '\xff'; bin[3] = '\xff';
    Catalog out;
    CHECK_FALSE(catalog_decode(bin, &out));
}

TEST_CASE("O-3: empty catalog (only defaultdb) round-trips") {
    Catalog orig;  // ctor 自带 defaultdb/public
    std::string bin = catalog_encode(orig);
    Catalog out;
    REQUIRE(catalog_decode(bin, &out));
    CHECK(out.list_databases().size() == 1);
    CHECK(out.list_schemas("defaultdb").size() == 1);
    CHECK(out.list_users().empty());
    CHECK(out.current_database() == "defaultdb");
}

TEST_CASE("O-3: Snapshot includes all internal state (databases, users, current_db)") {
    Catalog c;
    c.create_database("mydb");
    c.create_schema("mydb", "schema_a");
    c.create_schema("mydb", "schema_b");
    c.internal_set_user("u1", "h1", "s1");
    c.set_current_database("mydb");

    auto snap = c.internal_snapshot();
    CHECK(snap.current_db == "mydb");
    CHECK(snap.databases.size() == 2);  // defaultdb + mydb
    CHECK(snap.users.size() == 1);
    CHECK(std::get<0>(snap.users[0]) == "u1");
    CHECK(std::get<1>(snap.users[0]) == "h1");
    CHECK(std::get<2>(snap.users[0]) == "s1");
}

TEST_CASE("O-3: catalog binary embedded in v3 Checkpoint round-trips through engine") {
    namespace fs = std::filesystem;
    fs::path tmp = fs::temp_directory_path() / "jmdb_o3_test";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    // 走完整 StorageEngine 路径
    {
        StorageEngine eng(tmp.string());
        eng.load();
        eng.create_database("mydb");
        eng.create_schema("mydb", "audit");
        eng.create_user("alice", "secret123");
        eng.set_current_db("mydb");
        eng.create_table("mydb.public.users", {{"id", DataType::BIGINT, false, true}});
        eng.insert("mydb.public.users", {{"id", int64_t(1)}});
        eng.save();  // 强制 save (写 binary catalog)
    }
    {
        // 重新加载, 应从 binary catalog 恢复
        StorageEngine eng(tmp.string());
        eng.load();
        CHECK(eng.catalog()->database_exists("mydb"));
        CHECK(eng.catalog()->schema_exists("mydb", "audit"));
        CHECK(eng.catalog()->user_exists("alice"));
        CHECK(eng.catalog()->current_database() == "mydb");

        auto rows = eng.scan("mydb.public.users");
        CHECK(rows.size() == 1);
    }
    fs::remove_all(tmp);
}

TEST_CASE("O-3: v2 binary Checkpoint (legacy JSON catalog) still loads via fallback") {
    namespace fs = std::filesystem;
    fs::path tmp = fs::temp_directory_path() / "jmdb_o3_v2_test";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    // 手工构造 v2 binary Checkpoint 文件
    auto bin_path = tmp / "checkpoint.bin";
    {
        // 写 v2 binary: [magic 8B] + [u32 fmt=2] + [header] + ... + [JSON clog] + [JSON catalog]
        std::string v2;
        v2.append(kCheckpointMagic, kCheckpointMagicLen);
        // format_version
        uint32_t fmt = kCheckpointFormatVersionV2;
        for (int i = 0; i < 4; ++i) v2.push_back(static_cast<char>((fmt >> (i*8)) & 0xFF));
        // header
        auto put_i64 = [&](int64_t v) { for (int i=0;i<8;++i) v2.push_back(static_cast<char>((v >> (i*8)) & 0xFF)); };
        auto put_u32 = [&](uint32_t v) { for (int i=0;i<4;++i) v2.push_back(static_cast<char>((v >> (i*8)) & 0xFF)); };
        auto put_u16 = [&](uint16_t v) { v2.push_back(static_cast<char>(v & 0xFF)); v2.push_back(static_cast<char>((v>>8)&0xFF)); };
        auto put_str = [&](const std::string& s) { put_u16(static_cast<uint16_t>(s.size())); v2.append(s); };
        put_i64(1);    // version
        put_i64(0);    // last_seq
        put_i64(0);    // timestamp
        put_u32(3);    // next_xid
        // tables (空)
        put_u32(0);
        // table_data (空)
        put_u32(0);
        // row_id_counters (空)
        put_u32(0);
        // indexes_data (空)
        put_u32(0);
        // clog (JSON, 嵌入)
        put_str(std::string("{}"));
        // catalog (JSON, 嵌入) — 含 defaultdb
        put_str(std::string("{\"databases\":[{\"name\":\"defaultdb\",\"schemas\":[\"public\"]}]}"));
        std::ofstream f(bin_path.string(), std::ios::binary);
        f.write(v2.data(), static_cast<std::streamsize>(v2.size()));
    }
    {
        StorageEngine eng(tmp.string());
        eng.load();
        CHECK(eng.catalog()->database_exists("defaultdb"));
        CHECK(eng.catalog()->schema_exists("defaultdb", "public"));
    }
    fs::remove_all(tmp);
}

TEST_CASE("O-3: catalog binary size is comparable to JSON; win is CPU, not bytes") {
    Catalog c;
    c.create_database("analytics");  // 自带 public
    c.create_schema("analytics", "staging");
    c.create_user("alice", "x");
    c.create_user("bob",   "y");
    c.create_user("carol", "z");

    std::string bin = catalog_encode(c);
    std::string js  = c.to_json().dump();
    double ratio = static_cast<double>(bin.size()) / js.size();
    MESSAGE("binary=", bin.size(), " json=", js.size(), " ratio=", ratio);
    CHECK(ratio > 0.3);
    CHECK(ratio < 2.0);
}
