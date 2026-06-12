#include "transaction.h"
#include <stdexcept>
#include <iostream>
#include "common/json.h"

using json = Json;

namespace jiamiao {

// ═══════════════════════════════════════════════════════════
//  Tuple Visibility (MVCC)
// ═══════════════════════════════════════════════════════════

bool check_tuple_visibility(const Row& row, TransactionId current_xid,
                            const Snapshot& snap, int32_t cur_cid,
                            const CLog& clog) {
    // 读取 _xmin（插入该行的事务ID）
    TransactionId xmin = InvalidTransactionId;
    auto xmin_it = row.find("_xmin");
    if (xmin_it != row.end()) {
        auto* v = std::get_if<int64_t>(&xmin_it->second);
        if (v && *v > 0) xmin = static_cast<TransactionId>(*v);
    }
    // 没有 _xmin 的行（非事务写入的旧数据）视为可见
    if (xmin == InvalidTransactionId) return true;

    // 读取 _xmax（删除该行的事务ID，0 = 未删除）
    TransactionId xmax = InvalidTransactionId;
    auto xmax_it = row.find("_xmax");
    if (xmax_it != row.end()) {
        auto* v = std::get_if<int64_t>(&xmax_it->second);
        if (v && *v > 0) xmax = static_cast<TransactionId>(*v);
    }

    // 冻结事务: _xmin == FrozenTransactionId (2) 表示该行在远古历史中已存在,
    // 永远可见 (除非被 _xmax 删除)
    if (xmin == FrozenTransactionId) {
        if (xmax == InvalidTransactionId) return true;
        if (xmax == current_xid && current_xid != InvalidTransactionId) return false;
        auto xmax_status = clog.get_status(xmax);
        return xmax_status != TransactionStatus::COMMITTED;
    }

    // ── 规则 1: 自己插入的行 ──
    if (xmin == current_xid && current_xid != InvalidTransactionId) {
        // 检查命令 ID: 只能看到当前命令及之前插入的行
        auto cid_it = row.find("_cid");
        if (cid_it != row.end()) {
            auto* cv = std::get_if<int64_t>(&cid_it->second);
            if (cv && static_cast<int32_t>(*cv) > cur_cid) return false;
        }
        // 自己删除的 → 不可见
        if (xmax == current_xid) return false;
        return true;
    }

    // ── 规则 2: 插入者已回滚 → 不可见 ──
    TransactionStatus xmin_status = clog.get_status(xmin);
    if (xmin_status == TransactionStatus::ABORTED) return false;

    // ── 规则 3: 插入者仍在进行中（非自己） → 不可见 ──
    if (xmin_status == TransactionStatus::IN_PROGRESS) return false;

    // ── 规则 4: 快照时插入者仍在活跃 → 不可见 ──
    // xmin 已 COMMITTED，但需检查在快照时刻它是否活跃
    if (xmin >= snap.xmin && xmin < snap.xmax) {
        for (auto xip_xid : snap.xip) {
            if (xip_xid == xmin) return false; // 快照时活跃
        }
    }

    // ── 规则 5: 未删除 → 可见 ──
    if (xmax == InvalidTransactionId) return true;

    // ── 规则 6: 自己删除的 → 不可见 ──
    if (xmax == current_xid && current_xid != InvalidTransactionId) return false;

    // ── 规则 7: 删除者已提交 → 不可见 ──
    TransactionStatus xmax_status = clog.get_status(xmax);
    if (xmax_status == TransactionStatus::COMMITTED) return false;

    // ── 规则 8: 删除者已回滚或进行中 → 可见 ──
    return true;
}

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
        // 序列化所有条目: 已结束的用于恢复可见性判断,
        // IN_PROGRESS 的用于崩溃恢复 (会被 rebuild_active_from_clog 标记为 ABORTED)
        json e;
        e["xid"] = static_cast<int64_t>(xid);
        e["status"] = static_cast<uint8_t>(status);
        entries.push_back(e);
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
    // 回卷保护: 先尝试冻结, 再检查是否还能分配
    maybe_anti_wraparound();
    uint64_t next = next_xid_.load();
    if (next >= XIDEmergencyStop) {
        throw std::runtime_error(
            "XID 计数已达紧急停机点, 需执行全库 VACUUM (anti-wraparound)");
    }
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

// ── XID 回卷保护 ────────────────────────────────────────

TransactionId TransactionManager::get_oldest_active_xid() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_xids_.empty()) return next_xid_.load();
    return *active_xids_.begin();
}

int64_t TransactionManager::freeze_old_xids() {
    // 冻结策略: 把所有非活跃且 < next_xid - threshold 的 CLog 条目清掉
    // 活跃 xid 不会被冻结 (它们还在使用)
    int64_t frozen = 0;
    auto next = next_xid_.load();
    auto oldest = get_oldest_active_xid();

    // 遍历 CLog, 移除/标记可冻结的条目
    auto all = clog_.all_xids();
    for (auto xid : all) {
        if (xid < FirstNormalTransactionId) continue;  // 跳过保留值
        if (xid >= oldest) continue;  // 还在活跃窗口内
        auto status = clog_.get_status(xid);
        if (status == TransactionStatus::IN_PROGRESS) {
            // 僵尸事务: 强制 abort
            clog_.set_status(xid, TransactionStatus::ABORTED);
            frozen++;
        } else {
            // COMMITTED / ABORTED: 已被快照记录, 可清掉 CLog 条目
            // 这里只统计数量, 实际删除由 CLog 内部完成
            frozen++;
        }
    }
    return frozen;
}

bool TransactionManager::maybe_anti_wraparound() {
    // next_xid 是单调计数器, FrozenTransactionId=2 是历史下限
    // 当 next_xid - FrozenXid > XIDFreezeThreshold 时触发冻结
    auto next = next_xid_.load();
    if (next <= XIDFreezeThreshold) return false;
    auto distance = next - FrozenTransactionId;
    if (distance > XIDFreezeThreshold) {
        freeze_old_xids();
        return true;
    }
    return false;
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

// ── Snapshot (事务级快照, RR/SI 使用) ─────────────────────

Snapshot TransactionManager::get_transaction_snapshot() {
    // RR / SI: 事务内共用一个快照, 取一次后整个事务复用
    if (!context_.has_snapshot) {
        context_.snapshot = get_snapshot();
        context_.has_snapshot = true;
    }
    return context_.snapshot;
}

// ── SSI: SIREAD 注册与 rw-antidependency ─────────────────

void TransactionManager::register_siread(const RowKey& row_key) {
    // SIREAD 仅在 SERIALIZABLE 下生效
    if (context_.isolation != IsolationLevel::Serializable) return;
    if (context_.xid == InvalidTransactionId) return;

    std::lock_guard<std::mutex> lock(ssi_mutex_);
    siread_map_[row_key].insert(context_.xid);
}

void TransactionManager::register_write(const RowKey& row_key) {
    // 仅 SERIALIZABLE 下需追踪 rw-antidependency
    if (context_.isolation != IsolationLevel::Serializable) return;
    if (context_.xid == InvalidTransactionId) return;

    std::lock_guard<std::mutex> lock(ssi_mutex_);
    auto it = siread_map_.find(row_key);
    if (it == siread_map_.end()) return;

    // 找出所有读过该行的其他活跃事务, 建立 rw 边
    for (TransactionId reader : it->second) {
        if (reader == context_.xid) continue;
        in_conflict_[reader].insert(context_.xid);
    }
}

void TransactionManager::cleanup_transaction_state(TransactionId xid) {
    std::lock_guard<std::mutex> lock(ssi_mutex_);
    // 从 SIREAD 集合移除 xid
    for (auto& [key, readers] : siread_map_) {
        readers.erase(xid);
    }
    // 清理空的 SIREAD 条目 (避免无限增长)
    for (auto it = siread_map_.begin(); it != siread_map_.end(); ) {
        if (it->second.empty()) it = siread_map_.erase(it);
        else ++it;
    }
    // 清理 rw 边
    in_conflict_.erase(xid);
    // 同时清理其它事务指向 xid 的边
    for (auto& [_, conflicts] : in_conflict_) {
        conflicts.erase(xid);
    }
}

bool TransactionManager::ssi_is_pivot(TransactionId xid) const {
    std::lock_guard<std::mutex> lock(ssi_mutex_);
    auto it = in_conflict_.find(xid);
    if (it == in_conflict_.end()) return false;

    // 2-cycle: xid →rw T' 且 T' →rw xid
    for (TransactionId other : it->second) {
        if (other == xid) continue;
        auto oit = in_conflict_.find(other);
        if (oit != in_conflict_.end() && oit->second.count(xid)) {
            return true;
        }
    }
    return false;
}

// ── Undo ────────────────────────────────────────────────

void TransactionManager::add_undo_record(UndoRecord&& rec) {
    context_.undo_records.push_back(std::move(rec));
}

// ── Savepoint ───────────────────────────────────────────

void TransactionManager::savepoint(const std::string& name) {
    Savepoint sp;
    sp.name = name;
    sp.undo_position = context_.undo_records.size();
    context_.savepoints.push_back(sp);
}

size_t TransactionManager::rollback_to_savepoint(const std::string& name) {
    // 找到同名 savepoint (取最近一个)
    for (auto it = context_.savepoints.rbegin(); it != context_.savepoints.rend(); ++it) {
        if (it->name == name) {
            size_t current = context_.undo_records.size();
            size_t target = it->undo_position;
            // 弹出该 savepoint 之后的所有 savepoint
            // (嵌套语义: 回滚到 sp1, 之后在 sp1 之后创建的 sp2 也不存在)
            context_.savepoints.erase(
                (it + 1).base(), context_.savepoints.end());
            // 需要回滚的记录数
            if (current < target) return 0;  // 异常状态
            return current - target;
        }
    }
    throw std::runtime_error("savepoint \"" + name + "\" 不存在");
}

void TransactionManager::release_savepoint(const std::string& name) {
    // 移除 savepoint, 保留其后的所有 undo 记录 (后续提交时仍生效)
    for (auto it = context_.savepoints.rbegin(); it != context_.savepoints.rend(); ++it) {
        if (it->name == name) {
            context_.savepoints.erase((it + 1).base(), context_.savepoints.end());
            return;
        }
    }
    throw std::runtime_error("savepoint \"" + name + "\" 不存在");
}

bool TransactionManager::has_savepoint(const std::string& name) const {
    for (const auto& sp : context_.savepoints) {
        if (sp.name == name) return true;
    }
    return false;
}

// ── 状态机核心 ──────────────────────────────────────────

void TransactionManager::start_transaction_command() {
    switch (context_.block_state) {
    case TBlockState::DEFAULT:
        // 不在事务中 → 启动隐式事务
        context_.xid = InvalidTransactionId;
        context_.command_id = 0;
        context_.undo_records.clear();
        context_.has_snapshot = false;
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
        context_.has_snapshot = false;
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
        context_.has_snapshot = false;
        return;
    }

    // SSI pivot 检查: 在 SERIALIZABLE 下, 若本事务形成 2-cycle, 必须回滚
    if (context_.isolation == IsolationLevel::Serializable && ssi_is_pivot(xid)) {
        // 抛出异常, 由调用方走回滚路径 (apply_undo)
        throw std::runtime_error("could not serialize access (SSI pivot detected)");
    }

    // 标记 CLOG 为 COMMITTED
    clog_.set_status(xid, TransactionStatus::COMMITTED);

    // 从活跃集合中移除
    {
        std::lock_guard<std::mutex> lock(mutex_);
        active_xids_.erase(xid);
    }

    // 清理 SSI 状态
    cleanup_transaction_state(xid);

    // 清理 undo (已提交无需回滚)
    context_.undo_records.clear();
    context_.savepoints.clear();

    // 重置上下文
    context_.xid = InvalidTransactionId;
    context_.command_id = 0;
    context_.has_snapshot = false;
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

        // 清理 SSI 状态
        cleanup_transaction_state(xid);
    }

    // 清理 undo
    context_.undo_records.clear();
    context_.savepoints.clear();

    // 重置上下文
    context_.xid = InvalidTransactionId;
    context_.command_id = 0;
    context_.has_snapshot = false;
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
    context_.has_snapshot = false;
    context_.savepoints.clear();
}

} // namespace jiamiao
