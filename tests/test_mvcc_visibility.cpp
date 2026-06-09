#include "doctest.h"
#include "storage/transaction.h"
#include "json.h"

namespace jm = jiamiao;

// Helper: create a Row with given _xmin, _xmax, _cid
static Row make_row(int64_t xmin, int64_t xmax, int64_t cid,
                    const std::map<std::string, Value>& extra = {}) {
    Row r = extra;
    r["_xmin"] = xmin;
    r["_xmax"] = xmax;
    r["_cid"]  = cid;
    return r;
}

// Helper: make a snapshot with given active set
static jm::Snapshot make_snap(jm::TransactionId xmin, jm::TransactionId xmax,
                               std::vector<jm::TransactionId> xip, int32_t curcid = 0) {
    jm::Snapshot snap;
    snap.xmin  = xmin;
    snap.xmax  = xmax;
    snap.xip   = std::move(xip);
    snap.curcid = curcid;
    return snap;
}

TEST_CASE("MVCC: row without _xmin is always visible") {
    jm::CLog clog;
    jm::Snapshot snap = make_snap(3, 10, {});

    Row row;  // no _xmin key
    row["col"] = 42L;

    CHECK(jm::check_tuple_visibility(row, 5, snap, 0, clog) == true);
}

TEST_CASE("MVCC: Rule 1 — own inserted row is visible") {
    jm::CLog clog;
    jm::Snapshot snap = make_snap(3, 10, {3, 4, 5});  // current_xid=5 is active

    Row row = make_row(5, 0, 0);  // xmin=5 (self), xmax=0, cid=0

    // cur_cid >= _cid, so visible
    CHECK(jm::check_tuple_visibility(row, 5, snap, 0, clog) == true);
}

TEST_CASE("MVCC: Rule 1 — own inserted row with future cid is NOT visible") {
    jm::CLog clog;
    jm::Snapshot snap = make_snap(3, 10, {3, 4, 5});

    Row row = make_row(5, 0, 2);  // cid=2 > cur_cid=1

    CHECK(jm::check_tuple_visibility(row, 5, snap, 1, clog) == false);
}

TEST_CASE("MVCC: Rule 1 — own inserted row, self-deleted is NOT visible") {
    jm::CLog clog;
    jm::Snapshot snap = make_snap(3, 10, {3, 4, 5});

    Row row = make_row(5, 5, 0);  // xmin=5 (self), xmax=5 (self-deleted)

    CHECK(jm::check_tuple_visibility(row, 5, snap, 0, clog) == false);
}

TEST_CASE("MVCC: Rule 2 — inserted by ABORTED txn is NOT visible") {
    jm::CLog clog;
    clog.set_status(6, jm::TransactionStatus::ABORTED);
    jm::Snapshot snap = make_snap(3, 10, {});

    Row row = make_row(6, 0, 0);  // xmin=6 (ABORTED)

    CHECK(jm::check_tuple_visibility(row, 5, snap, 0, clog) == false);
}

TEST_CASE("MVCC: Rule 3 — inserted by IN_PROGRESS txn (not self) is NOT visible") {
    jm::CLog clog;
    clog.set_status(7, jm::TransactionStatus::IN_PROGRESS);
    jm::Snapshot snap = make_snap(3, 10, {7});  // XID 7 is active

    Row row = make_row(7, 0, 0);  // xmin=7 (IN_PROGRESS, not self)

    CHECK(jm::check_tuple_visibility(row, 5, snap, 0, clog) == false);
}

TEST_CASE("MVCC: Rule 4 — inserted by txn active in snapshot is NOT visible") {
    jm::CLog clog;
    clog.set_status(8, jm::TransactionStatus::COMMITTED);
    // snap.xmin=3, snap.xmax=10, xip contains 8 (was active at snapshot time)
    jm::Snapshot snap = make_snap(3, 10, {8});

    Row row = make_row(8, 0, 0);  // xmin=8 (COMMITTED but was active in snapshot)

    CHECK(jm::check_tuple_visibility(row, 5, snap, 0, clog) == false);
}

TEST_CASE("MVCC: Rule 4 — inserted by txn committed before snapshot is visible") {
    jm::CLog clog;
    clog.set_status(2, jm::TransactionStatus::COMMITTED);
    // snap.xmin=3, so xmin=2 is before the snapshot window
    jm::Snapshot snap = make_snap(3, 10, {});

    Row row = make_row(2, 0, 0);  // xmin=2 < snap.xmin

    CHECK(jm::check_tuple_visibility(row, 5, snap, 0, clog) == true);
}

TEST_CASE("MVCC: Rule 5 — no xmax (not deleted) is visible") {
    jm::CLog clog;
    clog.set_status(10, jm::TransactionStatus::COMMITTED);
    jm::Snapshot snap = make_snap(3, 20, {});

    Row row = make_row(10, 0, 0);  // xmax=0 → not deleted

    CHECK(jm::check_tuple_visibility(row, 5, snap, 0, clog) == true);
}

TEST_CASE("MVCC: Rule 6 — self-deleted (xmax == current_xid) is NOT visible") {
    jm::CLog clog;
    clog.set_status(10, jm::TransactionStatus::COMMITTED);
    jm::Snapshot snap = make_snap(3, 20, {5});

    Row row = make_row(10, 5, 0);  // xmax=5 == current_xid

    CHECK(jm::check_tuple_visibility(row, 5, snap, 0, clog) == false);
}

TEST_CASE("MVCC: Rule 7 — deleted by COMMITTED txn is NOT visible") {
    jm::CLog clog;
    clog.set_status(10, jm::TransactionStatus::COMMITTED);
    clog.set_status(11, jm::TransactionStatus::COMMITTED);
    jm::Snapshot snap = make_snap(3, 20, {});

    Row row = make_row(10, 11, 0);  // xmax=11 COMMITTED

    CHECK(jm::check_tuple_visibility(row, 5, snap, 0, clog) == false);
}

TEST_CASE("MVCC: Rule 8 — deleted by ABORTED txn IS visible") {
    jm::CLog clog;
    clog.set_status(10, jm::TransactionStatus::COMMITTED);
    clog.set_status(11, jm::TransactionStatus::ABORTED);
    jm::Snapshot snap = make_snap(3, 20, {});

    Row row = make_row(10, 11, 0);  // xmax=11 ABORTED (deletion rolled back)

    CHECK(jm::check_tuple_visibility(row, 5, snap, 0, clog) == true);
}

TEST_CASE("MVCC: Rule 8 — deleted by IN_PROGRESS txn IS visible") {
    jm::CLog clog;
    clog.set_status(10, jm::TransactionStatus::COMMITTED);
    clog.set_status(11, jm::TransactionStatus::IN_PROGRESS);
    jm::Snapshot snap = make_snap(3, 20, {11});

    Row row = make_row(10, 11, 0);  // xmax=11 IN_PROGRESS (deletion not committed)

    CHECK(jm::check_tuple_visibility(row, 5, snap, 0, clog) == true);
}

TEST_CASE("MVCC: current_xid == InvalidTransactionId (no transaction context)") {
    jm::CLog clog;
    clog.set_status(10, jm::TransactionStatus::COMMITTED);
    jm::Snapshot snap = make_snap(3, 20, {});

    Row row = make_row(10, 0, 0);  // committed, not deleted

    // Reading without a transaction should see committed data
    CHECK(jm::check_tuple_visibility(row, jm::InvalidTransactionId, snap, 0, clog) == true);
}

TEST_CASE("MVCC: edge case — xmin == 0 treated as invalid (old data)") {
    jm::CLog clog;
    jm::Snapshot snap = make_snap(3, 10, {});

    Row row = make_row(0, 0, 0);  // xmin=0 == InvalidTransactionId

    // Falls through to "no _xmin" early return → visible
    CHECK(jm::check_tuple_visibility(row, 5, snap, 0, clog) == true);
}
