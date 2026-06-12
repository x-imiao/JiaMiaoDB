/* ═══════════════════════════════════════════════════════════════════════
   O-4: CLog 二进制 codec 测试
   ═══════════════════════════════════════════════════════════════════════ */

#include "doctest.h"
#include "storage/transaction.h"
#include "storage/clog_codec.h"
#include "storage/catalog_codec.h"
#include "storage/checkpoint_codec.h"
#include "storage/engine.h"
#include "types.h"
#include <filesystem>
#include <fstream>
#include <cstring>
#include <unistd.h>

using namespace jiamiao;

namespace {

void fill_sample_clog(CLog& c) {
    c.set_status(3,  TransactionStatus::COMMITTED);
    c.set_status(4,  TransactionStatus::ABORTED);
    c.set_status(5,  TransactionStatus::IN_PROGRESS);
    c.set_status(7,  TransactionStatus::COMMITTED);
    c.set_status(11, TransactionStatus::SUB_COMMITTED);
    c.set_status(99, TransactionStatus::ABORTED);
}

}  // namespace

TEST_CASE("O-4: clog binary round-trip preserves all xid + status pairs") {
    CLog orig;
    fill_sample_clog(orig);
    std::string bin = clog_encode(orig);
    REQUIRE(!bin.empty());

    CLog out;
    REQUIRE(clog_decode(bin, &out));

    CHECK(out.get_status(3)  == TransactionStatus::COMMITTED);
    CHECK(out.get_status(4)  == TransactionStatus::ABORTED);
    CHECK(out.get_status(5)  == TransactionStatus::IN_PROGRESS);
    CHECK(out.get_status(7)  == TransactionStatus::COMMITTED);
    CHECK(out.get_status(11) == TransactionStatus::SUB_COMMITTED);
    CHECK(out.get_status(99) == TransactionStatus::ABORTED);
    CHECK(out.all_xids().size() == 6);
}

TEST_CASE("O-4: clog binary empty catalog encodes 9B (4B magic + u32 fmt + u32 count=0)") {
    CLog c;
    std::string bin = clog_encode(c);
    CHECK(bin.size() == 4 + 4 + 4);  // 12B
    CHECK(std::memcmp(bin.data(), kCLogMagic, kCLogMagicLen) == 0);
    CLog out;
    REQUIRE(clog_decode(bin, &out));
    CHECK(out.all_xids().empty());
}

TEST_CASE("O-4: clog binary rejects truncated payload") {
    CLog orig;
    fill_sample_clog(orig);
    std::string bin = clog_encode(orig);
    auto truncated = bin.substr(0, bin.size() / 2);
    CLog out;
    CHECK_FALSE(clog_decode(truncated, &out));
}

TEST_CASE("O-4: clog binary rejects unknown format version") {
    CLog orig;
    fill_sample_clog(orig);
    std::string bin = clog_encode(orig);
    bin[4] = '\xff'; bin[5] = '\xff'; bin[6] = '\xff'; bin[7] = '\xff';
    CLog out;
    CHECK_FALSE(clog_decode(bin, &out));
}

TEST_CASE("O-4: clog binary rejects wrong magic (caller should have sniffed first)") {
    CLog out;
    std::string bogus = "XXXX\x01\x00\x00\x00\x00\x00\x00\x00";
    CHECK_FALSE(clog_decode(bogus, &out));
}

TEST_CASE("O-4: clog binary size is ~5B per entry — win is bytes, not just CPU") {
    CLog c;
    for (uint32_t i = 3; i < 1003; ++i) {
        c.set_status(i, (i % 2 == 0) ? TransactionStatus::COMMITTED
                                     : TransactionStatus::ABORTED);
    }
    std::string bin = clog_encode(c);
    std::string js  = c.to_json().dump();
    double ratio = static_cast<double>(bin.size()) / js.size();
    MESSAGE("entries=1000 binary=", bin.size(), " json=", js.size(), " ratio=", ratio);
    // 1000 entries * 5B + 12B header = 5012B
    // JSON: ~ 1000 * {"xid":1234,"status":1} = ~ 20-30KB
    // binary should be 4-5x smaller
    CHECK(ratio < 0.25);
}

TEST_CASE("O-4: clog binary embedded in v3 Checkpoint round-trips through engine") {
    namespace fs = std::filesystem;
    fs::path tmp = fs::temp_directory_path() / "jmdb_o4_test";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    {
        StorageEngine eng(tmp.string());
        eng.load();
        eng.create_table("defaultdb.public.t", {{"id", DataType::BIGINT, false, true}});
        // 走显式事务块, 让 CLog 有 COMMITTED + ABORTED 状态
        // 模式: begin_transaction_block + start_transaction_command + reset_context (参考 test_storage_engine.cpp)
        auto& tm = eng.txn_mgr();
        tm.begin_transaction_block();
        tm.start_transaction_command();
        eng.insert_with_txn("defaultdb.public.t", {{"id", int64_t(1)}});
        eng.write_xact_commit(tm.get_current_xid());
        tm.commit_transaction();
        tm.reset_context();

        tm.begin_transaction_block();
        tm.start_transaction_command();
        eng.insert_with_txn("defaultdb.public.t", {{"id", int64_t(2)}});
        eng.write_xact_abort(tm.get_current_xid());
        tm.abort_transaction();
        tm.reset_context();
        eng.save();  // 写 binary clog
    }
    {
        StorageEngine eng(tmp.string());
        eng.load();
        // 关键是: clog 从 binary 恢复, commit 时 row 1 应能 scan 出来
        auto rows = eng.scan("defaultdb.public.t");
        CHECK(rows.size() >= 1);
        bool found_1 = false;
        for (const auto& r : rows) {
            auto it = r.find("id");
            if (it != r.end() && std::get<int64_t>(it->second) == 1) found_1 = true;
        }
        CHECK(found_1);
    }
    fs::remove_all(tmp);
}

TEST_CASE("O-4: v2 binary Checkpoint (legacy JSON clog) still loads via fallback") {
    namespace fs = std::filesystem;
    fs::path tmp = fs::temp_directory_path() / "jmdb_o4_v2_test";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    // 手工构造 v2 binary Checkpoint 文件 (含 JSON clog)
    auto bin_path = tmp / "checkpoint.bin";
    {
        std::string v2;
        v2.append(kCheckpointMagic, kCheckpointMagicLen);
        uint32_t fmt = kCheckpointFormatVersionV2;
        for (int i = 0; i < 4; ++i) v2.push_back(static_cast<char>((fmt >> (i*8)) & 0xFF));
        auto put_i64 = [&](int64_t v) { for (int i=0;i<8;++i) v2.push_back(static_cast<char>((v >> (i*8)) & 0xFF)); };
        auto put_u32 = [&](uint32_t v) { for (int i=0;i<4;++i) v2.push_back(static_cast<char>((v >> (i*8)) & 0xFF)); };
        auto put_u16 = [&](uint16_t v) { v2.push_back(static_cast<char>(v & 0xFF)); v2.push_back(static_cast<char>((v>>8)&0xFF)); };
        auto put_str = [&](const std::string& s) { put_u16(static_cast<uint16_t>(s.size())); v2.append(s); };
        put_i64(1);    // version
        put_i64(0);    // last_seq
        put_i64(0);    // timestamp
        put_u32(3);    // next_xid
        // tables
        put_u32(0);
        // table_data
        put_u32(0);
        // row_id_counters
        put_u32(0);
        // indexes_data
        put_u32(0);
        // clog (JSON) — 含 2 个 entry
        std::string clog_json = "[{\"xid\":5,\"status\":1},{\"xid\":6,\"status\":2}]";
        put_u32(static_cast<uint32_t>(clog_json.size()));
        v2.append(clog_json);
        // catalog (JSON)
        std::string cat_json = "{\"databases\":[{\"name\":\"defaultdb\",\"schemas\":[\"public\"]}]}";
        put_u32(static_cast<uint32_t>(cat_json.size()));
        v2.append(cat_json);
        std::ofstream f(bin_path.string(), std::ios::binary);
        f.write(v2.data(), static_cast<std::streamsize>(v2.size()));
    }
    {
        StorageEngine eng(tmp.string());
        eng.load();  // 应能加载 — clog 走 JSON fallback
        CHECK(eng.catalog()->database_exists("defaultdb"));
    }
    fs::remove_all(tmp);
}

TEST_CASE("O-4: v3 Checkpoint with JSON clog (legacy) loads via magic sniff fallback") {
    namespace fs = std::filesystem;
    fs::path tmp = fs::temp_directory_path() / "jmdb_o4_v3jsonclog_test";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    // 手工构造 v3 binary Checkpoint 文件, 但 clog 段故意是 JSON 数组 (无 "CLGB" magic)
    auto bin_path = tmp / "checkpoint.bin";
    {
        std::string v3;
        v3.append(kCheckpointMagic, kCheckpointMagicLen);
        uint32_t fmt = kCheckpointFormatVersionV3;
        for (int i = 0; i < 4; ++i) v3.push_back(static_cast<char>((fmt >> (i*8)) & 0xFF));
        auto put_i64 = [&](int64_t v) { for (int i=0;i<8;++i) v3.push_back(static_cast<char>((v >> (i*8)) & 0xFF)); };
        auto put_u32 = [&](uint32_t v) { for (int i=0;i<4;++i) v3.push_back(static_cast<char>((v >> (i*8)) & 0xFF)); };
        auto put_u16 = [&](uint16_t v) { v3.push_back(static_cast<char>(v & 0xFF)); v3.push_back(static_cast<char>((v>>8)&0xFF)); };
        auto put_str = [&](const std::string& s) { put_u16(static_cast<uint16_t>(s.size())); v3.append(s); };
        put_i64(1);
        put_i64(0);
        put_i64(0);
        put_u32(3);
        put_u32(0);  // tables
        put_u32(0);  // table_data
        put_u32(0);  // row_id_counters
        put_u32(0);  // indexes_data
        // clog (JSON, 旧 v3 形式) — 故意不用 "CLGB" magic
        std::string clog_json = "[{\"xid\":10,\"status\":1}]";
        put_str(clog_json);
        // catalog (binary, 新 v3 形式)
        Catalog c;
        std::string cat_bin = jiamiao::catalog_encode(c);
        put_str(cat_bin);
        std::ofstream f(bin_path.string(), std::ios::binary);
        f.write(v3.data(), static_cast<std::streamsize>(v3.size()));
    }
    {
        StorageEngine eng(tmp.string());
        eng.load();  // 应能从 binary catalog + JSON clog 加载
        CHECK(eng.catalog()->database_exists("defaultdb"));
    }
    fs::remove_all(tmp);
}
