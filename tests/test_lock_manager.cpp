#include "doctest.h"
#include "storage/lock_manager.h"
#include <atomic>
#include <chrono>
#include <thread>

using namespace jiamiao;
namespace jm = jiamiao;

TEST_CASE("Spinlock: exclusive access") {
    Spinlock s;
    s.lock();
    s.unlock();
    CHECK(s.try_lock());
    s.unlock();
}

TEST_CASE("Spinlock: try_lock fails when held") {
    Spinlock s;
    CHECK(s.try_lock());
    CHECK_FALSE(s.try_lock());
    s.unlock();
    CHECK(s.try_lock());
    s.unlock();
}

TEST_CASE("SpinlockGuard: RAII releases on scope exit") {
    Spinlock s;
    {
        SpinlockGuard g(s);
        CHECK_FALSE(s.try_lock());
    }
    CHECK(s.try_lock());
    s.unlock();
}

TEST_CASE("LWLock: shared readers coexist") {
    LWLock l;
    l.lock_shared();
    l.lock_shared();   // 第二个 shared 也可
    CHECK(l.try_lock_shared());
    l.unlock_shared();
    l.unlock_shared();
    l.unlock_shared();
}

TEST_CASE("LWLock: exclusive blocks shared") {
    LWLock l;
    CHECK(l.try_lock_exclusive());
    CHECK_FALSE(l.try_lock_shared());
    l.unlock_exclusive();
    CHECK(l.try_lock_shared());
    l.unlock_shared();
}

TEST_CASE("LWLock: shared blocks exclusive") {
    LWLock l;
    l.lock_shared();
    CHECK_FALSE(l.try_lock_exclusive());
    l.unlock_shared();
    CHECK(l.try_lock_exclusive());
    l.unlock_exclusive();
}

TEST_CASE("LockManager: single exclusive acquisition") {
    LockManager mgr(0);
    auto h = mgr.acquire(10, {LockTargetType::Table, "users"}, LockMode::Exclusive);
    CHECK(h.valid());
    CHECK(mgr.holds(10, {LockTargetType::Table, "users"}));
    CHECK(mgr.is_held({LockTargetType::Table, "users"}, LockMode::Exclusive));
    CHECK(mgr.total_held_locks() == 1);
}

TEST_CASE("LockManager: multiple shared acquisitions") {
    LockManager mgr(0);
    auto h1 = mgr.acquire(10, {LockTargetType::Table, "users"}, LockMode::Shared);
    auto h2 = mgr.acquire(11, {LockTargetType::Table, "users"}, LockMode::Shared);
    auto h3 = mgr.acquire(12, {LockTargetType::Table, "users"}, LockMode::Shared);
    CHECK(h1.valid());
    CHECK(h2.valid());
    CHECK(h3.valid());
    CHECK(mgr.is_held({LockTargetType::Table, "users"}, LockMode::Shared));
    CHECK_FALSE(mgr.is_held({LockTargetType::Table, "users"}, LockMode::Exclusive));
    CHECK(mgr.total_held_locks() == 3);
}

TEST_CASE("LockManager: shared→exclusive conflict blocks") {
    LockManager mgr(50);  // 50ms timeout
    auto h1 = mgr.acquire(10, {LockTargetType::Table, "users"}, LockMode::Shared);
    // 现在试图加 exclusive, 应超时
    CHECK_THROWS_AS(
        mgr.acquire(11, {LockTargetType::Table, "users"}, LockMode::Exclusive),
        std::runtime_error);
    // 释放 shared 后, exclusive 可加
    h1.release();
    auto h2 = mgr.acquire(11, {LockTargetType::Table, "users"}, LockMode::Exclusive);
    CHECK(h2.valid());
}

TEST_CASE("LockManager: exclusive→shared conflict blocks") {
    LockManager mgr(50);
    auto h = mgr.acquire(10, {LockTargetType::Table, "orders"}, LockMode::Exclusive);
    CHECK_THROWS_AS(
        mgr.acquire(11, {LockTargetType::Table, "orders"}, LockMode::Shared),
        std::runtime_error);
}

TEST_CASE("LockManager: reentrant on same transaction") {
    LockManager mgr(0);
    auto h1 = mgr.acquire(10, {LockTargetType::Table, "users"}, LockMode::Shared);
    auto h2 = mgr.acquire(10, {LockTargetType::Table, "users"}, LockMode::Shared);
    auto h3 = mgr.acquire(10, {LockTargetType::Table, "users"}, LockMode::Shared);
    CHECK(h1.valid());
    CHECK(h2.valid());
    CHECK(h3.valid());
    CHECK(mgr.total_held_locks() == 1);  // 同一事务同一 target 只算一个
}

TEST_CASE("LockManager: shared→exclusive upgrade rejected") {
    LockManager mgr(0);
    auto h = mgr.acquire(10, {LockTargetType::Table, "users"}, LockMode::Shared);
    CHECK_THROWS_AS(
        mgr.acquire(10, {LockTargetType::Table, "users"}, LockMode::Exclusive),
        std::runtime_error);
}

TEST_CASE("LockManager: different targets independent") {
    LockManager mgr(0);
    auto h_users  = mgr.acquire(10, {LockTargetType::Table, "users"},  LockMode::Exclusive);
    auto h_orders = mgr.acquire(10, {LockTargetType::Table, "orders"}, LockMode::Shared);
    auto h_items  = mgr.acquire(10, {LockTargetType::Table, "items"},  LockMode::Shared);
    CHECK(h_users.valid());
    CHECK(h_orders.valid());
    CHECK(h_items.valid());
    CHECK(mgr.total_held_locks() == 3);
}

TEST_CASE("LockManager: release_all releases everything for xid") {
    LockManager mgr(0);
    auto h1 = mgr.acquire(10, {LockTargetType::Table, "t1"}, LockMode::Shared);
    auto h2 = mgr.acquire(10, {LockTargetType::Table, "t2"}, LockMode::Exclusive);
    auto h3 = mgr.acquire(10, {LockTargetType::Table, "t3"}, LockMode::Shared);
    CHECK(h1.valid());
    CHECK(h2.valid());
    CHECK(h3.valid());
    CHECK(mgr.total_held_locks() == 3);
    mgr.release_all(10);
    CHECK(mgr.total_held_locks() == 0);
    CHECK_FALSE(mgr.holds(10, {LockTargetType::Table, "t1"}));
}

TEST_CASE("LockManager: after release_all, locks free for others") {
    LockManager mgr(0);
    auto h = mgr.acquire(10, {LockTargetType::Table, "users"}, LockMode::Exclusive);
    CHECK(mgr.is_held({LockTargetType::Table, "users"}, LockMode::Exclusive));
    h.release();
    CHECK_FALSE(mgr.is_held({LockTargetType::Table, "users"}, LockMode::Exclusive));
    // 另一事务可加 shared
    auto h2 = mgr.acquire(11, {LockTargetType::Table, "users"}, LockMode::Shared);
    CHECK(h2.valid());
}

TEST_CASE("LockManager: Handle move semantics") {
    LockManager mgr(0);
    auto h1 = mgr.acquire(10, {LockTargetType::Table, "x"}, LockMode::Exclusive);
    CHECK(h1.valid());
    auto h2 = std::move(h1);
    CHECK_FALSE(h1.valid());
    CHECK(h2.valid());
    // 析构 h2 时释放
}

TEST_CASE("LockManager: concurrent threads — only one writer") {
    LockManager mgr(10000);
    LockTarget t{LockTargetType::Table, "concurrent_test"};
    std::atomic<int> exclusive_held_count{0};
    std::atomic<int> max_concurrent_writers{0};
    std::atomic<int> error_count{0};

    auto writer = [&](int id) {
        try {
            // 真实事务 xid 从 FirstNormalTransactionId(=3) 起, 偏移避免与保留值冲突
            auto h = mgr.acquire(id + jm::FirstNormalTransactionId, t, LockMode::Exclusive);
            int after = exclusive_held_count.fetch_add(1) + 1;
            int prev_max = max_concurrent_writers.load();
            while (after > prev_max &&
                   !max_concurrent_writers.compare_exchange_weak(prev_max, after)) {}
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            exclusive_held_count.fetch_sub(1);
        } catch (const std::exception&) {
            error_count.fetch_add(1);
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) threads.emplace_back(writer, i);
    for (auto& t_ : threads) t_.join();

    CHECK(error_count.load() == 0);
    CHECK(max_concurrent_writers.load() == 1);
}

TEST_CASE("LockManager: concurrent readers coexist") {
    LockManager mgr(10000);
    LockTarget t{LockTargetType::Table, "read_test"};
    std::atomic<int> shared_held_count{0};
    std::atomic<int> max_concurrent_readers{0};
    std::atomic<int> error_count{0};

    auto reader = [&](int id) {
        try {
            auto h = mgr.acquire(id + jm::FirstNormalTransactionId, t, LockMode::Shared);
            int cur = shared_held_count.fetch_add(1) + 1;
            int prev = max_concurrent_readers.load();
            while (cur > prev && !max_concurrent_readers.compare_exchange_weak(prev, cur)) {}
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            shared_held_count.fetch_sub(1);
        } catch (const std::exception&) {
            error_count.fetch_add(1);
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) threads.emplace_back(reader, i);
    for (auto& t_ : threads) t_.join();

    CHECK(error_count.load() == 0);
    CHECK(max_concurrent_readers.load() == 8);
}

TEST_CASE("LockManager: clear_for_test works") {
    LockManager mgr(0);
    auto h1 = mgr.acquire(1, {LockTargetType::Table, "a"}, LockMode::Exclusive);
    auto h2 = mgr.acquire(2, {LockTargetType::Table, "b"}, LockMode::Shared);
    CHECK(h1.valid());
    CHECK(h2.valid());
    CHECK(mgr.total_held_locks() == 2);
    mgr.clear_for_test();
    CHECK(mgr.total_held_locks() == 0);
}
