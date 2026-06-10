#include "lock_manager.h"
#include <stdexcept>
#include <thread>

namespace jiamiao {

// ═══════════════════════════════════════════════════════
//  Spinlock
// ═══════════════════════════════════════════════════════

void Spinlock::lock() noexcept {
    // bounded spin: 1000 次后 yield, 避免饿死
    for (int i = 0; i < 1000; ++i) {
        if (try_lock()) return;
    }
    // 退化为 yield
    while (!try_lock()) {
        std::this_thread::yield();
    }
}

void Spinlock::unlock() noexcept {
    flag_.clear(std::memory_order_release);
}

bool Spinlock::try_lock() noexcept {
    return !flag_.test_and_set(std::memory_order_acquire);
}

// ═══════════════════════════════════════════════════════
//  LWLock (供业务代码使用, 不被 LockManager 内部使用)
// ═══════════════════════════════════════════════════════

void LWLock::lock_shared() {
    std::unique_lock<std::mutex> lk(cv_mutex_);
    shared_cv_.wait(lk, [this] { return !exclusive_.load(); });
    shared_count_.fetch_add(1);
}

void LWLock::unlock_shared() {
    int n = shared_count_.fetch_sub(1);
    if (n == 1) {
        std::lock_guard<std::mutex> lk(cv_mutex_);
        exclusive_cv_.notify_one();
    }
}

bool LWLock::try_lock_shared() {
    if (exclusive_.load()) return false;
    {
        std::lock_guard<std::mutex> lk(cv_mutex_);
        if (exclusive_.load()) return false;
        shared_count_.fetch_add(1);
    }
    return true;
}

void LWLock::lock_exclusive() {
    std::unique_lock<std::mutex> lk(cv_mutex_);
    exclusive_cv_.wait(lk, [this] {
        return shared_count_.load() == 0 && !exclusive_.load();
    });
    exclusive_.store(true);
}

void LWLock::unlock_exclusive() {
    {
        std::lock_guard<std::mutex> lk(cv_mutex_);
        exclusive_.store(false);
    }
    shared_cv_.notify_all();
    exclusive_cv_.notify_one();
}

bool LWLock::try_lock_exclusive() {
    std::lock_guard<std::mutex> lk(cv_mutex_);
    if (shared_count_.load() > 0 || exclusive_.load()) return false;
    exclusive_.store(true);
    return true;
}

// ═══════════════════════════════════════════════════════
//  LockManager
// ═══════════════════════════════════════════════════════
//
// 核心模式: mutex_ 同时保护状态和条件变量
//   acquire 期间持有 mutex_, 在 mutex_ 上 wait_cv_.wait()
//   release 期间持有 mutex_, 完成后 wait_cv_.notify_all()

LockManager::LockManager(uint32_t wait_timeout_ms)
    : wait_timeout_ms_(wait_timeout_ms) {}

bool LockManager::holds(TransactionId xid, const LockTarget& target) const {
    std::lock_guard<std::mutex> g(mutex_);
    auto txit = txn_locks_.find(xid);
    if (txit == txn_locks_.end()) return false;
    return txit->second.held.count(target) > 0;
}

bool LockManager::is_held(const LockTarget& target, LockMode mode) const {
    std::lock_guard<std::mutex> g(mutex_);
    auto it = key_states_.find(target);
    if (it == key_states_.end()) return false;
    if (mode == LockMode::Shared) return it->second.has_shared();
    return it->second.has_exclusive();
}

size_t LockManager::total_held_locks() const {
    std::lock_guard<std::mutex> g(mutex_);
    size_t n = 0;
    for (const auto& [_, ls] : txn_locks_) n += ls.held.size();
    return n;
}

size_t LockManager::total_waiters() const {
    std::lock_guard<std::mutex> g(mutex_);
    size_t n = 0;
    for (const auto& [_, ls] : txn_locks_) n += ls.waiting.size();
    return n;
}

void LockManager::clear_for_test() {
    std::lock_guard<std::mutex> g(mutex_);
    key_states_.clear();
    txn_locks_.clear();
}

void LockManager::acquire_locked(TransactionId xid, LockTarget target, LockMode mode) {
    // 保留为占位符: 实际逻辑在 acquire() 中, 那里持有 unique_lock
    (void)xid; (void)target; (void)mode;
}

void LockManager::release_locked(TransactionId xid, LockTarget target, LockMode mode) {
    // mutex_ 已持有
    auto kit = key_states_.find(target);
    if (kit == key_states_.end()) return;
    auto& ks = kit->second;

    auto txit = txn_locks_.find(xid);
    if (txit == txn_locks_.end()) return;
    auto& tls = txit->second;

    auto hit = tls.held.find(target);
    if (hit == tls.held.end()) return;
    if (hit->second != mode) return;

    if (mode == LockMode::Shared) {
        ks.shared_count -= 1;
    } else {
        ks.has_excl = false;
        ks.exclusive_xid = InvalidTransactionId;
    }
    tls.held.erase(target);
    if (tls.held.empty() && tls.waiting.empty()) {
        txn_locks_.erase(txit);
    }
    if (ks.is_free()) {
        key_states_.erase(kit);
    }
}

LockManager::Handle LockManager::acquire(TransactionId xid, LockTarget target, LockMode mode) {
    std::unique_lock<std::mutex> lk(mutex_);

    // 重入检查 (立即)
    {
        auto txit = txn_locks_.find(xid);
        if (txit != txn_locks_.end()) {
            auto hit = txit->second.held.find(target);
            if (hit != txit->second.held.end()) {
                if (hit->second == LockMode::Shared && mode == LockMode::Exclusive) {
                    throw std::runtime_error("lock upgrade not supported: shared→exclusive");
                }
                // 重入成功
                return Handle(this, xid, target, mode);
            }
        }
    }

    // 等待循环
    // 关键: 每次重检时重新查找 key_states_ 元素, 避免引用悬空
    auto start = std::chrono::steady_clock::now();
    while (true) {
        // 取当前状态
        auto kit = key_states_.find(target);
        bool ks_exists = (kit != key_states_.end());

        bool can_grant = false;
        if (mode == LockMode::Shared) {
            // shared: 无 exclusive 即可
            can_grant = !ks_exists || !kit->second.has_exclusive();
        } else {
            // exclusive: 完全空闲 (无 entry 也算空闲)
            can_grant = !ks_exists || kit->second.is_free();
        }

        if (can_grant) {
            KeyLockState& ks = key_states_[target];  // 创建或获取引用
            TransactionLockSet& tls = txn_locks_[xid];

            if (mode == LockMode::Shared) {
                ks.shared_count += 1;
            } else {
                ks.has_excl = true;
                ks.exclusive_xid = xid;
            }
            tls.held[target] = mode;
            tls.waiting.erase(target);
            return Handle(this, xid, target, mode);
        }

        // 标记为等待
        TransactionLockSet& tls = txn_locks_[xid];
        tls.waiting[target] = mode;

        // 等待
        if (wait_timeout_ms_ == 0) {
            wait_cv_.wait(lk);
            continue;
        }
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed >= static_cast<int64_t>(wait_timeout_ms_)) {
            // 超时: 清理 waiting
            tls.waiting.erase(target);
            if (tls.held.empty() && tls.waiting.empty()) {
                txn_locks_.erase(xid);
            }
            throw std::runtime_error("lock acquire timeout on " + target.name);
        }
        uint32_t remain_ms = wait_timeout_ms_ - static_cast<uint32_t>(elapsed);
        wait_cv_.wait_for(lk, std::chrono::milliseconds(remain_ms));
    }
}

void LockManager::release_all(TransactionId xid) {
    {
        std::lock_guard<std::mutex> g(mutex_);
        auto txit = txn_locks_.find(xid);
        if (txit == txn_locks_.end()) return;
        // 复制 held 集合, 避免迭代中修改
        auto held = txit->second.held;
        for (const auto& [target, mode] : held) {
            release_locked(xid, target, mode);
        }
    }
    wait_cv_.notify_all();
}

void LockManager::Handle::release() {
    if (!valid_) return;
    if (mgr_) mgr_->release_all(xid_);
    valid_ = false;
}

} // namespace jiamiao
