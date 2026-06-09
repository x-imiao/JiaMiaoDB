#include "transaction.h"
#include <stdexcept>
#include <iostream>
#include "json.h"

using json = Json;

namespace jiamiao {

// ═══════════════════════════════════════════════════════════
//  CLog
// ═══════════════════════════════════════════════════════════

void CLog::set_status(TransactionId xid, TransactionStatus status) {
    std::lock_guard<std::mutex> lock(mutex_);
    status_map_[xid] = status;
    dirty_.insert(xid);
}

TransactionStatus CLog::get_status(TransactionId xid) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = status_map_.find(xid);
    if (it == status_map_.end()) return TransactionStatus::IN_PROGRESS;
    return it->second;
}

Json CLog::to_json() const {
    std::lock_guard<std::mutex> lock(mutex_);
    json entries = json::array();
    for (const auto& [xid, status] : status_map_) {
        // 只序列化已结束的事务 (COMMITTED / ABORTED)
        // IN_PROGRESS 的事务不需要持久化 (崩溃后自然回滚)
        if (status != TransactionStatus::IN_PROGRESS) {
            json e;
            e["xid"] = static_cast<int64_t>(xid);
            e["status"] = static_cast<uint8_t>(status);
            entries.push_back(e);
        }
    }
    return entries;
}

void CLog::from_json(const Json& json_data) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& e : json_data) {
        TransactionId xid = static_cast<TransactionId>(e["xid"].get_int());
        TransactionStatus status = static_cast<TransactionStatus>(
            static_cast<uint8_t>(e["status"].get_int()));
        status_map_[xid] = status;
    }
}

std::set<TransactionId> CLog::all_xids() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::set<TransactionId> result;
    for (const auto& [xid, _] : status_map_) {
        result.insert(xid);
    }
    return result;
}

std::set<TransactionId> CLog::in_progress_xids() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::set<TransactionId> result;
    for (const auto& [xid, status] : status_map_) {
        if (status == TransactionStatus::IN_PROGRESS) {
            result.insert(xid);
        }
    }
    return result;
}

// ═══════════════════════════════════════════════════════════
//  TransactionManager
// ═══════════════════════════════════════════════════════════

TransactionManager::TransactionManager() {
    // 初始化: 下一个 XID = FirstNormalTransactionId
    next_xid_.store(FirstNormalTransactionId);
}

// ── XID 分配 ────────────────────────────────────────────

TransactionId TransactionManager::allocate_xid() {
    TransactionId xid = next_xid_.fetch_add(1);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        active_xids_.insert(xid);
    }
    clog_.set_status(xid, TransactionStatus::IN_PROGRESS);
    return xid;
}

TransactionId TransactionManager::assign_xid() {
    if (context_.xid == InvalidTransactionId) {
        context_.xid = allocate_xid();
    }
    return context_.xid;
}

// ── Snapshot ────────────────────────────────────────────

Snapshot TransactionManager::get_snapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    Snapshot snap;
    if (!active_xids_.empty()) {
        snap.xmin = *active_xids_.begin();
    } else {
        snap.xmin = next_xid_.load();
    }
    snap.xmax = next_xid_.load();
    snap.xip.assign(active_xids_.begin(), active_xids_.end());
    snap.curcid = context_.command_id;
    return snap;
}

TransactionStatus TransactionManager::get_transaction_status(TransactionId xid) const {
    return clog_.get_status(xid);
}

// ── Undo ────────────────────────────────────────────────

void TransactionManager::add_undo_record(UndoRecord&& rec) {
    context_.undo_records.push_back(std::move(rec));
}

// ── 状态机核心 ──────────────────────────────────────────

void TransactionManager::start_transaction_command() {
    switch (context_.block_state) {
    case TBlockState::DEFAULT:
        // 不在事务中 → 启动隐式事务
        context_.xid = InvalidTransactionId;
        context_.command_id = 0;
        context_.undo_records.clear();
        context_.block_state = TBlockState::STARTED;
        break;

    case TBlockState::INPROGRESS:
    case TBlockState::BEGIN:
        // 已在事务中, 保持不动
        break;

    default:
        break;
    }
}

void TransactionManager::commit_transaction_command() {
    switch (context_.block_state) {
    case TBlockState::STARTED:
        // 隐式单语句事务 → 自动提交
        commit_transaction();
        context_.block_state = TBlockState::DEFAULT;
        break;

    case TBlockState::BEGIN:
        // BEGIN 后面的第一条语句 → 进入事务块
        context_.block_state = TBlockState::INPROGRESS;
        break;

    case TBlockState::INPROGRESS:
        // 事务块中的语句间隔 → 递增命令计数器
        command_counter_increment();
        break;

    case TBlockState::END:
        // COMMIT 已执行 → 真正提交
        commit_transaction();
        context_.block_state = TBlockState::DEFAULT;
        break;

    case TBlockState::ABORT_PENDING:
        // ROLLBACK 已执行 → 真正回滚
        abort_transaction();
        context_.block_state = TBlockState::DEFAULT;
        break;

    default:
        break;
    }
}

// ── 显式事务控制 ────────────────────────────────────────

void TransactionManager::begin_transaction_block() {
    if (context_.block_state != TBlockState::DEFAULT &&
        context_.block_state != TBlockState::STARTED) {
        throw std::runtime_error("已在事务块中");
    }
    // 确保已启动底层事务
    if (context_.block_state == TBlockState::DEFAULT) {
        context_.xid = InvalidTransactionId;
        context_.command_id = 0;
        context_.undo_records.clear();
    }
    context_.block_state = TBlockState::BEGIN;
}

void TransactionManager::commit_transaction() {
    TransactionId xid = context_.xid;

    if (xid == InvalidTransactionId) {
        // 只读事务, 无 WAL, 直接清理
        {
            std::lock_guard<std::mutex> lock(mutex_);
            active_xids_.erase(xid);
        }
        context_.undo_records.clear();
        return;
    }

    // 标记 CLOG 为 COMMITTED
    clog_.set_status(xid, TransactionStatus::COMMITTED);

    // 从活跃集合中移除
    {
        std::lock_guard<std::mutex> lock(mutex_);
        active_xids_.erase(xid);
    }

    // 清理 undo (已提交无需回滚)
    context_.undo_records.clear();

    // 重置上下文
    context_.xid = InvalidTransactionId;
    context_.command_id = 0;
}

void TransactionManager::abort_transaction() {
    // 反向重放 undo records (回滚)
    // undo 的执行需要 StorageEngine 配合, 这里只提供 undo 数据
    // 实际回滚在 StorageEngine 中完成

    TransactionId xid = context_.xid;

    if (xid != InvalidTransactionId) {
        // 标记 CLOG 为 ABORTED
        clog_.set_status(xid, TransactionStatus::ABORTED);

        // 从活跃集合中移除
        {
            std::lock_guard<std::mutex> lock(mutex_);
            active_xids_.erase(xid);
        }
    }

    // 清理 undo
    context_.undo_records.clear();

    // 重置上下文
    context_.xid = InvalidTransactionId;
    context_.command_id = 0;
}

// ── Checkpoint 序列化 ───────────────────────────────────

Json TransactionManager::to_json() const {
    json j;
    j["next_xid"] = static_cast<int64_t>(next_xid_.load());
    j["clog"] = clog_.to_json();
    return j;
}

void TransactionManager::from_json(const Json& json_data) {
    if (json_data.contains("next_xid")) {
        TransactionId nxid = static_cast<TransactionId>(json_data["next_xid"].get_int());
        if (nxid >= FirstNormalTransactionId) {
            next_xid_.store(nxid);
        }
    }
    if (json_data.contains("clog")) {
        clog_.from_json(json_data["clog"]);
    }
}

void TransactionManager::rebuild_active_from_clog() {
    std::lock_guard<std::mutex> lock(mutex_);
    active_xids_.clear();
    // 恢复进行中的事务: recovery 时, 崩溃前的 IN_PROGRESS
    // 事务在重启后视为 ABORTED (因为 WAL 中没有 xact_commit 记录)
    auto in_progress = clog_.in_progress_xids();
    for (auto xid : in_progress) {
        clog_.set_status(xid, TransactionStatus::ABORTED);
    }
}

void TransactionManager::reset_context() {
    context_.xid = InvalidTransactionId;
    context_.block_state = TBlockState::DEFAULT;
    context_.command_id = 0;
    context_.undo_records.clear();
}

} // namespace jiamiao
