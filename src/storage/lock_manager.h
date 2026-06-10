#ifndef JIAMIAODB_STORAGE_LOCK_MANAGER_H
#define JIAMIAODB_STORAGE_LOCK_MANAGER_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <string>

#include "transaction.h"

/* ═══════════════════════════════════════════════════════
   LockManager — 多级锁管理器

   三级锁层级（参考 PostgreSQL lwlock 设计）:
   - Spinlock:   std::atomic_flag 忙等, 极短临界区 (< 10 条指令)
   - LWLock:     共享/排他模式, shared_mutex 等价物
   - RegularLock: std::mutex 长持有, 跨函数

   锁粒度 (Phase 2):
   - Table 级: 保护 data_[table] 的并发读写
   - Key 级:   未来扩展, per-row 锁

   锁释放:
   - RAII Handle 离开作用域时自动释放
   - 事务结束 (commit/abort) 时 release_all
   - 不做死锁检测, 依赖按固定顺序加锁避免

   等待机制:
   - 单个 std::mutex + std::condition_variable 保护所有状态
   - 加锁期间在 cv 上等待, 释放/获取时 notify_all
   - 简化版: notify_all 而非精确唤醒, 适合低争用场景

   AI Hooks:
   - 当前实现: 单 mutex + 广播通知
   - 替换条件: 高争用时, 可换 per-target 细粒度 cv
   - 替换方法: 替换 KeyLockState 内的 cv 字段即可
   ═══════════════════════════════════════════════════════ */

namespace jiamiao {

// ── Lock Mode ───────────────────────────────────────────

enum class LockMode {
    Shared,      // 读锁: 多事务可同时持有
    Exclusive    // 写锁: 仅一事务可持有
};

// ── Lock Target ─────────────────────────────────────────

enum class LockTargetType {
    Table,
    Key,         // 预留: per-row / per-key 锁
    CLog         // 预留: 事务状态日志
};

struct LockTarget {
    LockTargetType type = LockTargetType::Table;
    std::string    name;        // 表名 / row_id / clog 标识

    bool operator<(const LockTarget& o) const {
        if (type != o.type) return type < o.type;
        return name < o.name;
    }
    bool operator==(const LockTarget& o) const {
        return type == o.type && name == o.name;
    }
};

// ── Spinlock ────────────────────────────────────────────
// 极短临界区 (< 10 条指令)。基于 std::atomic_flag 自旋。

class Spinlock {
public:
    Spinlock() = default;

    void lock() noexcept;
    void unlock() noexcept;
    bool try_lock() noexcept;

private:
    std::atomic_flag flag_ = {};
};

// RAII
class SpinlockGuard {
public:
    explicit SpinlockGuard(Spinlock& s) : s_(s) { s_.lock(); }
    ~SpinlockGuard() { s_.unlock(); }
    SpinlockGuard(const SpinlockGuard&) = delete;
    SpinlockGuard& operator=(const SpinlockGuard&) = delete;

private:
    Spinlock& s_;
};

// ── LWLock (Lightweight Lock) ───────────────────────────
// 共享/排他模式。供业务代码使用 (例如保护单个 range 的内部状态)。
// LockManager 本身不用 LWLock, 改用 std::mutex + cv (见下)。

class LWLock {
public:
    LWLock() = default;

    // shared 模式: 多个读者可同时进入
    void lock_shared();
    void unlock_shared();
    bool try_lock_shared();

    // exclusive 模式: 排他访问
    void lock_exclusive();
    void unlock_exclusive();
    bool try_lock_exclusive();

private:
    std::atomic<int> shared_count_{0};
    std::atomic<bool> exclusive_{false};
    std::condition_variable shared_cv_;
    std::condition_variable exclusive_cv_;
    std::mutex cv_mutex_;
};

class LWLockSharedGuard {
public:
    explicit LWLockSharedGuard(LWLock& l) : l_(l) { l_.lock_shared(); }
    ~LWLockSharedGuard() { l_.unlock_shared(); }
    LWLockSharedGuard(const LWLockSharedGuard&) = delete;
    LWLockSharedGuard& operator=(const LWLockSharedGuard&) = delete;

private:
    LWLock& l_;
};

class LWLockExclusiveGuard {
public:
    explicit LWLockExclusiveGuard(LWLock& l) : l_(l) { l_.lock_exclusive(); }
    ~LWLockExclusiveGuard() { l_.unlock_exclusive(); }
    LWLockExclusiveGuard(const LWLockExclusiveGuard&) = delete;
    LWLockExclusiveGuard& operator=(const LWLockExclusiveGuard&) = delete;

private:
    LWLock& l_;
};

// ── RegularLock ─────────────────────────────────────────
// 长持有锁的别名。直接用 std::mutex。

using RegularLock = std::mutex;

class RegularLockGuard {
public:
    explicit RegularLockGuard(RegularLock& l) : l_(l) { l.lock(); }
    ~RegularLockGuard() { l_.unlock(); }
    RegularLockGuard(const RegularLockGuard&) = delete;
    RegularLockGuard& operator=(const RegularLockGuard&) = delete;

private:
    RegularLock& l_;
};

// ── Per-key lock state ──────────────────────────────────

struct KeyLockState {
    int           shared_count    = 0;
    // 是否持有 exclusive 锁. 用 bool 标记, 不依赖 InvalidTransactionId 的具体数值
    // (因为正常事务 xid 可能从 FirstNormalTransactionId 起, 旧实现中 0 同时是 Invalid)
    bool          has_excl        = false;
    TransactionId exclusive_xid   = InvalidTransactionId;

    bool has_shared()   const { return shared_count > 0; }
    bool has_exclusive() const { return has_excl; }
    bool is_free()       const { return !has_shared() && !has_exclusive(); }
};

// ── Per-transaction lock bookkeeping ────────────────────

struct TransactionLockSet {
    std::map<LockTarget, LockMode> held;
    std::map<LockTarget, LockMode> waiting;
};

// ── LockManager ─────────────────────────────────────────

class LockManager {
public:
    // 锁等待超时 (ms)。超时抛异常, 避免死锁。
    // 0 = 永不超时 (仅供测试或 debug 用)
    explicit LockManager(uint32_t wait_timeout_ms = 5000);

    // RAII 句柄: 离开作用域自动 release_all(xid)
    // 当前实现: 释放该 xid 持有的所有锁
    // 未来如需 per-target 释放, 改为 release_one
    class Handle {
    public:
        Handle() = default;
        Handle(LockManager* mgr, TransactionId xid, LockTarget target, LockMode mode)
            : mgr_(mgr), xid_(xid), target_(target), mode_(mode), valid_(true) {}

        ~Handle() { release(); }

        Handle(Handle&& o) noexcept
            : mgr_(o.mgr_), xid_(o.xid_), target_(o.target_), mode_(o.mode_), valid_(o.valid_) {
            o.valid_ = false;
        }
        Handle& operator=(Handle&& o) noexcept {
            if (this != &o) {
                release();
                mgr_ = o.mgr_; xid_ = o.xid_; target_ = o.target_;
                mode_ = o.mode_; valid_ = o.valid_;
                o.valid_ = false;
            }
            return *this;
        }
        Handle(const Handle&) = delete;
        Handle& operator=(const Handle&) = delete;

        bool valid() const { return valid_; }
        TransactionId xid() const { return xid_; }
        LockTarget   target() const { return target_; }
        LockMode     mode() const { return mode_; }

        void release();

    private:
        LockManager*  mgr_    = nullptr;
        TransactionId xid_    = InvalidTransactionId;
        LockTarget    target_;
        LockMode      mode_   = LockMode::Shared;
        bool          valid_  = false;
    };

    // 加锁 (阻塞到获得 / 超时 / 错误)
    //   - 同一事务对同一 target 重复加锁 → 重入 (no-op)
    //   - 已持有 shared 再请求 exclusive → 抛异常 (不支持升级)
    //   - 超时 → 抛 std::runtime_error
    Handle acquire(TransactionId xid, LockTarget target, LockMode mode);

    // 释放某事务所有锁 (commit/abort 时调用)
    void release_all(TransactionId xid);

    // 查询
    bool holds(TransactionId xid, const LockTarget& target) const;
    bool is_held(const LockTarget& target, LockMode mode) const;
    size_t total_held_locks() const;
    size_t total_waiters() const;

    // 测试用
    void clear_for_test();

private:
    // 加锁 (假定 mutex_ 已持有)
    void acquire_locked(TransactionId xid, LockTarget target, LockMode mode);
    // 释放 (假定 mutex_ 已持有)
    void release_locked(TransactionId xid, LockTarget target, LockMode mode);

    // 单一 mutex 保护所有状态 (与 cv 共享)
    mutable std::mutex                              mutex_;
    std::condition_variable                         wait_cv_;
    std::map<LockTarget, KeyLockState>              key_states_;
    std::map<TransactionId, TransactionLockSet>     txn_locks_;
    uint32_t                                        wait_timeout_ms_;
};

} // namespace jiamiao

#endif // JIAMIAODB_STORAGE_LOCK_MANAGER_H
