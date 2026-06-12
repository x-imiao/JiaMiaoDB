#ifndef JIAMIAODB_STORAGE_TRANSACTION_H
#define JIAMIAODB_STORAGE_TRANSACTION_H

#include <cstdint>
#include <vector>
#include <unordered_map>
#include <set>
#include <mutex>
#include <string>
#include <atomic>
#include "common/json.h"
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

// XID 回卷保护: 32 位 XID 单调递增, 2^32 之后会回卷
// 在 next_xid 距离 FrozenTransactionId 太近时, 必须强制冻结旧事务
// (PG 触发 anti-wraparound vacuum)
constexpr uint64_t XIDWrapThreshold     = 1000000;   // 距 2^32 的安全余量
constexpr uint64_t XIDFreezeThreshold   = 2000000;   // 强制冻结阈值 (next_xid - FrozenXid)
constexpr uint64_t XIDEmergencyStop     = 2146483648u; // 距 2^32 - 1M 强制停机

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

// ── Isolation Level (Phase 2) ────────────────────────────

enum class IsolationLevel : uint8_t {
    ReadCommitted   = 0,   // 每条语句一个新快照
    RepeatableRead  = 1,   // 事务内共用一个快照 (snapshot isolation)
    Serializable    = 2    // SI + SIREAD + rw-antidependency 检测
};

// 行级 SIREAD 锁的 key: (表, _rowid)
struct RowKey {
    std::string table;
    int64_t     row_id = 0;

    bool operator==(const RowKey& o) const {
        return table == o.table && row_id == o.row_id;
    }
};

struct RowKeyHash {
    size_t operator()(const RowKey& k) const noexcept {
        return std::hash<std::string>{}(k.table) * 31
             ^ std::hash<int64_t>{}(k.row_id);
    }
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

// ── Savepoint ───────────────────────────────────────────

struct Savepoint {
    std::string name;
    size_t      undo_position = 0;  // 创建 savepoint 时 undo_records 的 size
};

// ── Snapshot (MVCC 快照，Phase 2 启用) ───────────────────

struct Snapshot {
    TransactionId              xmin  = InvalidTransactionId;
    TransactionId              xmax  = InvalidTransactionId;
    // Phase 3 NOTE: xip 暂用默认 allocator (测试代码已用 std::vector<uint32_t> 直接 assign).
    // 转换 JMAlloc<TransactionId> 需要所有构造点同步, Phase 3b 再做.
    std::vector<TransactionId> xip;
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
    // Phase 2: 事务级快照 (RR/SI 用), 取一次后整个事务复用
    Snapshot               snapshot;
    bool                   has_snapshot = false;
    // 当前隔离级
    IsolationLevel         isolation    = IsolationLevel::ReadCommitted;
    // Savepoint 栈
    std::vector<Savepoint> savepoints;
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
    // COMMIT / COMMIT TRANSACTION: 提交当前事务 (含 SSI pivot 检查)
    void commit_transaction();
    // ROLLBACK / ABORT: 回滚当前事务
    void abort_transaction();

    // ── 隔离级 (Phase 2) ──
    void set_isolation_level(IsolationLevel lvl) { context_.isolation = lvl; }
    IsolationLevel get_isolation_level() const { return context_.isolation; }

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
    // 包含回卷检查: 若 next_xid 接近回卷, 强制冻结旧事务 (anti-wraparound)
    // 若已到强制停机点, 抛 runtime_error (需人工介入)
    TransactionId assign_xid();
    TransactionId get_next_xid() const { return next_xid_.load(); }
    void set_next_xid(TransactionId xid) { next_xid_.store(xid); }

    // 回卷保护: 强制将所有 COMMITTED 且 < oldest_active 的 xid 标记为 FROZEN
    // 返回被冻结的 xid 数量
    // 通常由 VACUUM 触发, 也可在分配 XID 时自动调用
    int64_t freeze_old_xids();

    // 最老的活跃 xid (分配新 xid 时的"地板"); 不在 active set 中时返回 next_xid
    TransactionId get_oldest_active_xid() const;

    // 检查并执行回卷保护: 若 next_xid - FrozenXid > FreezeThreshold, 冻结
    // 返回是否执行了冻结
    bool maybe_anti_wraparound();

    // ── 命令计数器 ──

    int32_t command_counter_increment() { return ++context_.command_id; }
    int32_t get_current_command_id() const { return context_.command_id; }

    // ── 快照 ──

    // 总是返回新快照 (RC 用)
    Snapshot get_snapshot();
    // 返回当前事务的快照 (RR/SI: 事务期间稳定)
    // 首次调用时分配, 后续复用
    Snapshot get_transaction_snapshot();

    // ── Undo 注册 ──

    void add_undo_record(UndoRecord&& rec);

    // ── Savepoint ──
    // 创建 savepoint: 记录当前 undo_records 的大小
    void savepoint(const std::string& name);
    // 回滚到 savepoint: 返回需要回滚的 undo 记录数 (从末尾倒数)
    // 调用方应执行该数量的 undo, 然后从 stack 移除该 savepoint
    size_t rollback_to_savepoint(const std::string& name);
    // 释放 savepoint: 仅从 stack 移除, 不回滚 (后续事务仍可见这些写入)
    void release_savepoint(const std::string& name);

    // 是否有指定名字的 savepoint
    bool has_savepoint(const std::string& name) const;
    // 当前 savepoint 数量
    size_t savepoint_count() const { return context_.savepoints.size(); }

    // ── CLog 访问 ──

    CLog& clog() { return clog_; }
    const CLog& clog() const { return clog_; }

    const std::set<TransactionId>& active_xids() const { return active_xids_; }
    const std::vector<UndoRecord>& undo_records() const { return context_.undo_records; }

    // ── SSI: SIREAD + rw-antidependency (Phase 2) ──

    // 记录: xid 读取了 row_key
    // 只在 SERIALIZABLE 下生效
    void register_siread(const RowKey& row_key);

    // 记录: xid 写入了 row_key
    // 找到所有读过该行的其他事务, 建立 rw 边: reader →rw xid
    // 副作用: 在 in_conflict_[reader] 中加入 xid
    void register_write(const RowKey& row_key);

    // 事务结束时清理它的 SIREAD 和 rw 边
    void cleanup_transaction_state(TransactionId xid);

    // 检查 xid 是否在 SSI 意义下是 pivot (即和任何其它事务形成 2-cycle)
    // 2-cycle 判定: 存在 T' 使 T' in in_conflict[xid] AND xid in in_conflict[T']
    //   即 xid →rw T' 且 T' →rw xid
    bool ssi_is_pivot(TransactionId xid) const;

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

    // ── SSI 状态 ──
    // SIREAD: row_key → 读过它的活跃事务集
    std::unordered_map<RowKey, std::set<TransactionId>, RowKeyHash> siread_map_;
    // rw 冲突: T →rw T' 表示 T 依赖于 T' 的写, 记为 T' ∈ in_conflict_[T]
    std::unordered_map<TransactionId, std::set<TransactionId>> in_conflict_;
    mutable std::mutex              ssi_mutex_;
};

} // namespace jiamiao

#endif // JIAMIAODB_STORAGE_TRANSACTION_H
