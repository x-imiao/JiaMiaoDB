#ifndef JIAMIAODB_STORAGE_TRANSACTION_H
#define JIAMIAODB_STORAGE_TRANSACTION_H

#include <cstdint>
#include <vector>
#include <unordered_map>
#include <set>
#include <mutex>
#include <string>
#include <atomic>
#include "json.h"
#include "../types.h"

/* ═══════════════════════════════════════════════════════
   Transaction Subsystem — 事务子系统

   参考 PostgreSQL 18.3 事务设计，针对 JMDB 纯内存架构定制。
   核心组件:
   - TransactionId: 32位单调递增事务ID
   - CLog: 内存事务状态日志（提交/回滚/进行中）
   - TransactionManager: XID分配 + 状态机 + Undo管理
   - UndoRecord: 运行时回滚的撤销记录

   Phase 1 聚焦原子性和持久性，并发控制仍由 global_mutex_ 保证。
   ═══════════════════════════════════════════════════════ */

namespace jiamiao {

// ── Transaction ID ──────────────────────────────────────

using TransactionId = uint32_t;

constexpr TransactionId InvalidTransactionId     = 0;
constexpr TransactionId BootstrapTransactionId   = 1;
constexpr TransactionId FrozenTransactionId      = 2;
constexpr TransactionId FirstNormalTransactionId = 3;

// ── Transaction Status (CLog 中每个 XID 的 2-bit 状态) ──

enum class TransactionStatus : uint8_t {
    IN_PROGRESS   = 0,
    COMMITTED     = 1,
    ABORTED       = 2,
    SUB_COMMITTED = 3   // 子事务已提交（为 Savepoint 预留）
};

inline const char* txn_status_name(TransactionStatus s) {
    switch (s) {
        case TransactionStatus::IN_PROGRESS:   return "IN_PROGRESS";
        case TransactionStatus::COMMITTED:     return "COMMITTED";
        case TransactionStatus::ABORTED:       return "ABORTED";
        case TransactionStatus::SUB_COMMITTED: return "SUB_COMMITTED";
    }
    return "UNKNOWN";
}

// ── Block State (事务块语义状态，参考 PG TBlockState) ──

enum class TBlockState : uint8_t {
    DEFAULT,          // 不在事务块中
    STARTED,          // 隐式单语句事务已启动
    BEGIN,            // BEGIN 已执行
    INPROGRESS,       // 显式事务块进行中
    END,              // COMMIT 已执行
    ABORT_PENDING     // ROLLBACK 已执行
};

// ── Undo Record ─────────────────────────────────────────

enum class UndoOp : uint8_t {
    INSERT,   // 回滚 = 删除该行
    UPDATE,   // 回滚 = 恢复旧行
    DELETE    // 回滚 = 重新插入旧行
};

struct UndoRecord {
    UndoOp      op;          // 操作类型
    std::string table_name;  // 目标表
    int64_t     row_id;      // 受影响行的 _rowid
    Row         old_row;     // 修改前的行 (UPDATE/DELETE)
    Row         new_row;     // 修改后的行 (INSERT/UPDATE)

    UndoRecord() = default;
    UndoRecord(UndoOp o, std::string t, int64_t rid, Row old_r, Row new_r)
        : op(o), table_name(std::move(t)), row_id(rid),
          old_row(std::move(old_r)), new_row(std::move(new_r)) {}
};

// ── Snapshot (MVCC 快照，Phase 2 启用) ───────────────────

struct Snapshot {
    TransactionId              xmin  = InvalidTransactionId;
    TransactionId              xmax  = InvalidTransactionId;
    std::vector<TransactionId> xip;   // 活跃事务列表
    int32_t                    curcid = 0;
};

// ── CLog (Commit Log) ───────────────────────────────────

class CLog {
public:
    void set_status(TransactionId xid, TransactionStatus status);
    TransactionStatus get_status(TransactionId xid) const;

    // 同步: 在 commit/abort 后, 将已结束的事务状态刷到 checkpoint 持久化区
    // 调用时机: checkpoint() 时
    void mark_dirty(TransactionId xid) { dirty_.insert(xid); }
    void mark_clean(TransactionId xid) { dirty_.erase(xid); }

    Json to_json() const;
    void from_json(const Json& json);

    // 遍历所有已知 xid (用于 checkpoint 序列化 + recovery 重建 active set)
    std::set<TransactionId> all_xids() const;

    // 获取所有进行中的 xid (用于 rebuild active set)
    std::set<TransactionId> in_progress_xids() const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<TransactionId, TransactionStatus> status_map_;
    std::set<TransactionId> dirty_;  // 自上次 checkpoint 以来改变过的 xid
};

// ── Tuple Visibility (MVCC) ─────────────────────────────

// Read Committed 行可见性判断。
// current_xid: 当前事务 XID (InvalidTransactionId = 无事务)
// snap: 语句开始时获取的快照
// cur_cid: 当前命令 ID (事务内可见性)
bool check_tuple_visibility(const Row& row, TransactionId current_xid,
                            const Snapshot& snap, int32_t cur_cid,
                            const CLog& clog);

// ── TransactionContext ──────────────────────────────────

struct TransactionContext {
    TransactionId          xid          = InvalidTransactionId;
    TBlockState            block_state  = TBlockState::DEFAULT;
    int32_t                command_id   = 0;
    std::vector<UndoRecord> undo_records;
};

// ── TransactionManager ──────────────────────────────────

class TransactionManager {
public:
    TransactionManager();

    // ── 高层接口 (server 层调用) ──

    // 每条 SQL 开始前调用, 处理隐式事务启动
    void start_transaction_command();
    // 每条 SQL 结束后调用, 处理隐式提交 / 命令计数递增
    void commit_transaction_command();

    // ── 显式事务控制 ──

    // BEGIN / BEGIN TRANSACTION
    void begin_transaction_block();
    // COMMIT / COMMIT TRANSACTION: 提交当前事务
    void commit_transaction();
    // ROLLBACK / ABORT: 回滚当前事务
    void abort_transaction();

    // ── 状态查询 ──

    bool is_in_transaction() const {
        return context_.block_state != TBlockState::DEFAULT;
    }
    bool is_in_transaction_block() const {
        return context_.block_state == TBlockState::INPROGRESS ||
               context_.block_state == TBlockState::BEGIN ||
               context_.block_state == TBlockState::END;
    }
    TransactionId get_current_xid() const { return context_.xid; }
    TransactionStatus get_transaction_status(TransactionId xid) const;

    // ── XID 管理 ──

    // 延迟分配: 首次写操作时调用
    TransactionId assign_xid();
    TransactionId get_next_xid() const { return next_xid_.load(); }
    void set_next_xid(TransactionId xid) { next_xid_.store(xid); }

    // ── 命令计数器 ──

    int32_t command_counter_increment() { return ++context_.command_id; }
    int32_t get_current_command_id() const { return context_.command_id; }

    // ── 快照 ──

    Snapshot get_snapshot();

    // ── Undo 注册 ──

    void add_undo_record(UndoRecord&& rec);

    // ── CLog 访问 ──

    CLog& clog() { return clog_; }
    const CLog& clog() const { return clog_; }

    const std::set<TransactionId>& active_xids() const { return active_xids_; }
    const std::vector<UndoRecord>& undo_records() const { return context_.undo_records; }

    // ── Checkpoint / Recovery ──

    Json to_json() const;
    void from_json(const Json& json);

    // 从 CLog 中重建活跃事务集合 (recovery 用)
    void rebuild_active_from_clog();

    // 清理: 恢复后重置状态
    void reset_context();

private:
    TransactionId allocate_xid();

    TransactionContext              context_;
    std::atomic<TransactionId>      next_xid_{FirstNormalTransactionId};
    CLog                            clog_;
    std::set<TransactionId>         active_xids_;
    mutable std::mutex              mutex_;
};

} // namespace jiamiao

#endif // JIAMIAODB_STORAGE_TRANSACTION_H
