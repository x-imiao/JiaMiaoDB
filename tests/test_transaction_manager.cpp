#include "doctest.h"
#include "storage/transaction.h"
#include "json.h"
#include <stdexcept>

using json = Json;
namespace jm = jiamiao;

TEST_CASE("TransactionManager: initial state") {
    jm::TransactionManager mgr;

    CHECK(mgr.get_next_xid() == jm::FirstNormalTransactionId);
    CHECK(mgr.get_current_xid() == jm::InvalidTransactionId);
    CHECK(mgr.is_in_transaction() == false);
    CHECK(mgr.is_in_transaction_block() == false);
    CHECK(mgr.get_current_command_id() == 0);
    CHECK(mgr.undo_records().empty());
    CHECK(mgr.active_xids().empty());
}

TEST_CASE("TransactionManager: assign_xid allocates FirstNormalTransactionId first") {
    jm::TransactionManager mgr;

    jm::TransactionId xid = mgr.assign_xid();

    CHECK(xid == jm::FirstNormalTransactionId);  // = 3
    CHECK(mgr.get_current_xid() == xid);
    CHECK(mgr.clog().get_status(xid) == jm::TransactionStatus::IN_PROGRESS);
    CHECK(mgr.active_xids().count(xid) == 1);
}

TEST_CASE("TransactionManager: assign_xid is lazy (not called by start_transaction_command)") {
    jm::TransactionManager mgr;

    mgr.start_transaction_command();
    // After starting a transaction command, XID should NOT be assigned yet
    CHECK(mgr.get_current_xid() == jm::InvalidTransactionId);
}

TEST_CASE("TransactionManager: consecutive XIDs are unique and monotonic") {
    jm::TransactionManager mgr;

    jm::TransactionId prev = mgr.assign_xid();
    CHECK(prev == jm::FirstNormalTransactionId);

    // Each commit_transaction resets context, allowing new XID allocation
    std::set<jm::TransactionId> seen;
    seen.insert(prev);

    for (int i = 0; i < 4; ++i) {
        mgr.commit_transaction();  // reset context
        mgr.start_transaction_command();
        jm::TransactionId next = mgr.assign_xid();
        CHECK(next > prev);                            // monotonic
        CHECK(seen.count(next) == 0);                  // unique
        seen.insert(next);
        prev = next;
    }

    CHECK(seen.size() == 5);
}

TEST_CASE("TransactionManager: get_snapshot captures active XIDs") {
    jm::TransactionManager mgr;

    // Allocate 3 XIDs without committing
    jm::TransactionId xid1 = mgr.assign_xid();  // 3
    mgr.commit_transaction();
    mgr.start_transaction_command();
    jm::TransactionId xid2 = mgr.assign_xid();  // 4
    mgr.commit_transaction();
    mgr.start_transaction_command();
    jm::TransactionId xid3 = mgr.assign_xid();  // 5

    // Now only xid3 is active
    jm::Snapshot snap = mgr.get_snapshot();
    CHECK(snap.xmin == xid3);     // smallest active
    CHECK(snap.xmax == mgr.get_next_xid());
    CHECK(snap.xip.size() == 1);
    CHECK(snap.xip[0] == xid3);
    CHECK(snap.curcid == 0);
}

TEST_CASE("TransactionManager: snapshot with no active XIDs") {
    jm::TransactionManager mgr;

    jm::Snapshot snap = mgr.get_snapshot();
    // xmin == next_xid when no active transactions
    CHECK(snap.xmin == mgr.get_next_xid());
    CHECK(snap.xmax == mgr.get_next_xid());
    CHECK(snap.xip.empty());
}

TEST_CASE("TransactionManager: start_transaction_command DEFAULT -> STARTED") {
    jm::TransactionManager mgr;

    mgr.start_transaction_command();
    CHECK(mgr.is_in_transaction() == true);
    // block_state should be STARTED (internal, verified via behavior)
}

TEST_CASE("TransactionManager: commit_transaction_command STARTED -> DEFAULT (implicit commit)") {
    jm::TransactionManager mgr;

    mgr.start_transaction_command();
    jm::TransactionId xid = mgr.assign_xid();

    mgr.commit_transaction_command();

    CHECK(mgr.is_in_transaction() == false);
    CHECK(mgr.clog().get_status(xid) == jm::TransactionStatus::COMMITTED);
    CHECK(mgr.active_xids().count(xid) == 0);
    CHECK(mgr.get_current_xid() == jm::InvalidTransactionId);
}

TEST_CASE("TransactionManager: begin_transaction_block success path") {
    jm::TransactionManager mgr;

    mgr.begin_transaction_block();

    CHECK(mgr.is_in_transaction_block() == true);
    CHECK(mgr.get_current_xid() == jm::InvalidTransactionId);
    CHECK(mgr.get_current_command_id() == 0);
}

TEST_CASE("TransactionManager: begin_transaction_block rejects nested BEGIN") {
    jm::TransactionManager mgr;

    mgr.begin_transaction_block();
    CHECK_THROWS_AS(mgr.begin_transaction_block(), std::runtime_error);
}

TEST_CASE("TransactionManager: commit_transaction marks CLog COMMITTED") {
    jm::TransactionManager mgr;

    mgr.begin_transaction_block();
    mgr.start_transaction_command();
    jm::TransactionId xid = mgr.assign_xid();

    mgr.commit_transaction();

    CHECK(mgr.clog().get_status(xid) == jm::TransactionStatus::COMMITTED);
    CHECK(mgr.active_xids().count(xid) == 0);
    CHECK(mgr.undo_records().empty());
    CHECK(mgr.get_current_xid() == jm::InvalidTransactionId);
}

TEST_CASE("TransactionManager: commit_transaction on read-only (no XID)") {
    jm::TransactionManager mgr;

    mgr.begin_transaction_block();
    // No assign_xid() — read-only transaction
    mgr.commit_transaction();

    // Should not crash, context cleaned up
    CHECK(mgr.get_current_xid() == jm::InvalidTransactionId);
    CHECK(mgr.undo_records().empty());
}

TEST_CASE("TransactionManager: abort_transaction marks CLog ABORTED") {
    jm::TransactionManager mgr;

    mgr.begin_transaction_block();
    mgr.start_transaction_command();
    jm::TransactionId xid = mgr.assign_xid();

    mgr.abort_transaction();

    CHECK(mgr.clog().get_status(xid) == jm::TransactionStatus::ABORTED);
    CHECK(mgr.active_xids().count(xid) == 0);
    CHECK(mgr.undo_records().empty());
    CHECK(mgr.get_current_xid() == jm::InvalidTransactionId);
}

TEST_CASE("TransactionManager: add_undo_record and retrieval") {
    jm::TransactionManager mgr;

    jm::UndoRecord rec1(jm::UndoOp::INSERT, "t1", 1, Row{}, Row{});
    jm::UndoRecord rec2(jm::UndoOp::UPDATE, "t1", 2, Row{}, Row{});
    jm::UndoRecord rec3(jm::UndoOp::DELETE, "t1", 3, Row{}, Row{});

    mgr.add_undo_record(std::move(rec1));
    mgr.add_undo_record(std::move(rec2));
    mgr.add_undo_record(std::move(rec3));

    const auto& records = mgr.undo_records();
    CHECK(records.size() == 3);
    CHECK(records[0].op == jm::UndoOp::INSERT);
    CHECK(records[1].op == jm::UndoOp::UPDATE);
    CHECK(records[2].op == jm::UndoOp::DELETE);
}

TEST_CASE("TransactionManager: undo_records cleared on commit") {
    jm::TransactionManager mgr;

    mgr.begin_transaction_block();
    mgr.start_transaction_command();
    mgr.assign_xid();
    mgr.add_undo_record(jm::UndoRecord(jm::UndoOp::INSERT, "t", 1, Row{}, Row{}));

    mgr.commit_transaction();

    CHECK(mgr.undo_records().empty());
}

TEST_CASE("TransactionManager: undo_records cleared on abort") {
    jm::TransactionManager mgr;

    mgr.begin_transaction_block();
    mgr.start_transaction_command();
    mgr.assign_xid();
    mgr.add_undo_record(jm::UndoRecord(jm::UndoOp::INSERT, "t", 1, Row{}, Row{}));

    mgr.abort_transaction();

    CHECK(mgr.undo_records().empty());
}

TEST_CASE("TransactionManager: to_json / from_json round-trip") {
    jm::TransactionManager mgr;

    // Allocate some XIDs and commit/abort them
    mgr.begin_transaction_block();
    mgr.start_transaction_command();
    jm::TransactionId xid1 = mgr.assign_xid();
    mgr.commit_transaction();

    mgr.reset_context();  // reset block_state before next begin

    mgr.begin_transaction_block();
    mgr.start_transaction_command();
    jm::TransactionId xid2 = mgr.assign_xid();
    mgr.abort_transaction();

    json state = mgr.to_json();

    // Reconstruct a new manager from the state
    jm::TransactionManager mgr2;
    mgr2.from_json(state);

    CHECK(mgr2.get_next_xid() == mgr.get_next_xid());
    CHECK(mgr2.clog().get_status(xid1) == jm::TransactionStatus::COMMITTED);
    CHECK(mgr2.clog().get_status(xid2) == jm::TransactionStatus::ABORTED);
}

TEST_CASE("TransactionManager: rebuild_active_from_clog marks IN_PROGRESS as ABORTED") {
    jm::TransactionManager mgr;

    // Simulate crash scenario: some XIDs are IN_PROGRESS in CLog
    mgr.clog().set_status(10, jm::TransactionStatus::IN_PROGRESS);
    mgr.clog().set_status(11, jm::TransactionStatus::IN_PROGRESS);
    mgr.clog().set_status(12, jm::TransactionStatus::COMMITTED);

    mgr.rebuild_active_from_clog();

    // IN_PROGRESS entries should be marked ABORTED
    CHECK(mgr.clog().get_status(10) == jm::TransactionStatus::ABORTED);
    CHECK(mgr.clog().get_status(11) == jm::TransactionStatus::ABORTED);
    // COMMITTED entry should be untouched
    CHECK(mgr.clog().get_status(12) == jm::TransactionStatus::COMMITTED);
    // active_xids should be empty
    CHECK(mgr.active_xids().empty());
}

TEST_CASE("TransactionManager: reset_context restores defaults") {
    jm::TransactionManager mgr;

    // Modify context
    mgr.begin_transaction_block();
    mgr.start_transaction_command();
    mgr.assign_xid();
    mgr.add_undo_record(jm::UndoRecord(jm::UndoOp::INSERT, "t", 1, Row{}, Row{}));

    mgr.reset_context();

    CHECK(mgr.get_current_xid() == jm::InvalidTransactionId);
    CHECK(mgr.is_in_transaction() == false);
    CHECK(mgr.is_in_transaction_block() == false);
    CHECK(mgr.get_current_command_id() == 0);
    CHECK(mgr.undo_records().empty());
}

TEST_CASE("TransactionManager: command_counter_increment") {
    jm::TransactionManager mgr;

    CHECK(mgr.get_current_command_id() == 0);

    int32_t cid1 = mgr.command_counter_increment();
    CHECK(cid1 == 1);
    CHECK(mgr.get_current_command_id() == 1);

    int32_t cid2 = mgr.command_counter_increment();
    CHECK(cid2 == 2);
    CHECK(mgr.get_current_command_id() == 2);
}
