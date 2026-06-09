#include "doctest.h"
#include "storage/transaction.h"
#include "json.h"
#include <thread>
#include <vector>

using json = Json;
namespace jm = jiamiao;

// Helper: construct a minimal Json array from CLog-like entries
static json make_clog_entry(jm::TransactionId xid, jm::TransactionStatus status) {
    json e;
    e["xid"] = static_cast<int64_t>(xid);
    e["status"] = static_cast<uint8_t>(status);
    return e;
}

TEST_CASE("CLog: set_status and get_status round-trip") {
    jm::CLog clog;

    clog.set_status(5, jm::TransactionStatus::COMMITTED);
    CHECK(clog.get_status(5) == jm::TransactionStatus::COMMITTED);

    clog.set_status(7, jm::TransactionStatus::ABORTED);
    CHECK(clog.get_status(7) == jm::TransactionStatus::ABORTED);

    // Overwrite status
    clog.set_status(5, jm::TransactionStatus::ABORTED);
    CHECK(clog.get_status(5) == jm::TransactionStatus::ABORTED);
}

TEST_CASE("CLog: unknown XID returns IN_PROGRESS") {
    jm::CLog clog;
    CHECK(clog.get_status(999) == jm::TransactionStatus::IN_PROGRESS);
    CHECK(clog.get_status(0) == jm::TransactionStatus::IN_PROGRESS);
}

TEST_CASE("CLog: IN_PROGRESS is not serialized in to_json") {
    jm::CLog clog;
    clog.set_status(3, jm::TransactionStatus::IN_PROGRESS);

    json j = clog.to_json();
    CHECK(j.size() == 0);  // IN_PROGRESS entries are excluded
}

TEST_CASE("CLog: COMMITTED and ABORTED are serialized in to_json") {
    jm::CLog clog;
    clog.set_status(3, jm::TransactionStatus::COMMITTED);
    clog.set_status(4, jm::TransactionStatus::ABORTED);

    json j = clog.to_json();
    CHECK(j.size() == 2);

    // Find entries by xid (order may vary)
    bool found3 = false, found4 = false;
    for (size_t i = 0; i < j.size(); ++i) {
        int64_t xid = j[i]["xid"].get_int();
        uint8_t status = static_cast<uint8_t>(j[i]["status"].get_int());
        if (xid == 3 && status == static_cast<uint8_t>(jm::TransactionStatus::COMMITTED)) found3 = true;
        if (xid == 4 && status == static_cast<uint8_t>(jm::TransactionStatus::ABORTED)) found4 = true;
    }
    CHECK(found3);
    CHECK(found4);
}

TEST_CASE("CLog: from_json deserialization round-trip") {
    jm::CLog clog;

    json arr = json::array();
    arr.push_back(make_clog_entry(10, jm::TransactionStatus::COMMITTED));
    arr.push_back(make_clog_entry(11, jm::TransactionStatus::ABORTED));

    clog.from_json(arr);

    CHECK(clog.get_status(10) == jm::TransactionStatus::COMMITTED);
    CHECK(clog.get_status(11) == jm::TransactionStatus::ABORTED);
}

TEST_CASE("CLog: from_json overwrites existing statuses") {
    jm::CLog clog;
    clog.set_status(10, jm::TransactionStatus::ABORTED);

    json arr = json::array();
    arr.push_back(make_clog_entry(10, jm::TransactionStatus::COMMITTED));
    clog.from_json(arr);

    CHECK(clog.get_status(10) == jm::TransactionStatus::COMMITTED);
}

TEST_CASE("CLog: in_progress_xids returns only IN_PROGRESS") {
    jm::CLog clog;
    clog.set_status(3, jm::TransactionStatus::COMMITTED);
    clog.set_status(4, jm::TransactionStatus::IN_PROGRESS);
    clog.set_status(5, jm::TransactionStatus::ABORTED);
    clog.set_status(6, jm::TransactionStatus::IN_PROGRESS);

    auto in_progress = clog.in_progress_xids();
    CHECK(in_progress.size() == 2);
    CHECK(in_progress.count(4) == 1);
    CHECK(in_progress.count(6) == 1);
    CHECK(in_progress.count(3) == 0);
    CHECK(in_progress.count(5) == 0);
}

TEST_CASE("CLog: all_xids returns all known XIDs") {
    jm::CLog clog;
    clog.set_status(3, jm::TransactionStatus::COMMITTED);
    clog.set_status(4, jm::TransactionStatus::IN_PROGRESS);
    clog.set_status(5, jm::TransactionStatus::ABORTED);

    auto all = clog.all_xids();
    CHECK(all.size() == 3);
    CHECK(all.count(3) == 1);
    CHECK(all.count(4) == 1);
    CHECK(all.count(5) == 1);
}

TEST_CASE("CLog: thread safety (concurrent set_status + get_status)") {
    jm::CLog clog;
    const int num_threads = 4;
    const int ops_per_thread = 100;
    std::vector<std::thread> threads;

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&clog, t]() {
            for (int i = 0; i < ops_per_thread; ++i) {
                jm::TransactionId xid = static_cast<jm::TransactionId>(t * 1000 + i + 10);
                clog.set_status(xid, jm::TransactionStatus::COMMITTED);
                auto status = clog.get_status(xid);
                CHECK(status == jm::TransactionStatus::COMMITTED);
            }
        });
    }

    for (auto& th : threads) th.join();

    // Verify all entries persisted
    for (int t = 0; t < num_threads; ++t) {
        for (int i = 0; i < ops_per_thread; ++i) {
            jm::TransactionId xid = static_cast<jm::TransactionId>(t * 1000 + i + 10);
            CHECK(clog.get_status(xid) == jm::TransactionStatus::COMMITTED);
        }
    }
}
