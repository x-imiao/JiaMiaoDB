/* ══════════════════════════════════════════════════════════════════════
   memtable.h — Phase 1 LSM 重构：内存表 (MemTable)

   MemTable 是 LSM-Tree 的"内存 L0 切片":
     - 接受按 (InternalKey, Tuple*) 的 put
     - 同 row_id 多版本共存, 旧版本由 vacuum 清理
     - 提供 row_id ASC, 同 row_id 内 seq DESC 的 scan 顺序

   Phase 1 实现: SkipList 是 std::map 的 shim (O(log n) + shared_mutex)
   Phase 2 计划: 替换为真并发跳表 (lock-free, Pugh style)

   vacuum 策略: 删掉 xmax != 0 且 clog.get_status(xmax) == COMMITTED
   且 xmax < oldest_active_xid 的版本. 调用方 (StorageEngine) 决定时机.
   ══════════════════════════════════════════════════════════════════════ */

#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <vector>

#include "transaction.h"  // TransactionId, CLog
#include "tuple.h"        // InternalKey, Tuple

namespace jiamiao {

// ── SkipList: Phase 1 shim ──
//   接口与真跳表一致; 实现 = std::map<K, V> + 读写锁
//   Phase 2 替换为无锁/单写多读跳表, 调用方零修改
template <typename K, typename V>
class SkipList {
public:
    void put(const K& k, V v) {
        std::unique_lock<std::shared_mutex> lk(m_);
        impl_[k] = std::move(v);
    }

    // 严格等于: InternalKey 是 (user_key, seq), 配对
    std::optional<V> get_exact(const K& k) const {
        std::shared_lock<std::shared_mutex> lk(m_);
        auto it = impl_.find(k);
        if (it == impl_.end()) return std::nullopt;
        return it->second;
    }

    // 删 (精确匹配)
    bool erase(const K& k) {
        std::unique_lock<std::shared_mutex> lk(m_);
        return impl_.erase(k) > 0;
    }

    // 范围: 返回 [lo, hi] 内所有 entry, 按 K 排序 (latest first)
    std::vector<V> range(const K& lo, const K& hi) const {
        std::vector<V> out;
        std::shared_lock<std::shared_mutex> lk(m_);
        auto it = impl_.lower_bound(lo);
        // InternalKey: lo={uk, MAX_SEQ}, hi={uk, 0}, operator< 是 user_key ASC + seq DESC.
        // 迭代条件: !(hi < it->first) ⇔ it->first <= hi, 跨过 hi 后 (下个 user_key) 停止.
        for (; it != impl_.end() && !(hi < it->first); ++it) {
            out.push_back(it->second);
        }
        return out;
    }

    // 范围 (带 key): 给 MemTable::erase_all_for 这种需要精确 InternalKey 的场景用.
    std::vector<std::pair<K, V>> range_with_keys(const K& lo, const K& hi) const {
        std::vector<std::pair<K, V>> out;
        std::shared_lock<std::shared_mutex> lk(m_);
        auto it = impl_.lower_bound(lo);
        for (; it != impl_.end() && !(hi < it->first); ++it) {
            out.emplace_back(it->first, it->second);
        }
        return out;
    }

    // 全量 dump (调试 + scan_all)
    std::vector<V> all() const {
        std::vector<V> out;
        std::shared_lock<std::shared_mutex> lk(m_);
        out.reserve(impl_.size());
        for (const auto& [_, v] : impl_) out.push_back(v);
        return out;
    }

    size_t size() const {
        std::shared_lock<std::shared_mutex> lk(m_);
        return impl_.size();
    }

    void clear() {
        std::unique_lock<std::shared_mutex> lk(m_);
        impl_.clear();
    }

private:
    mutable std::shared_mutex m_;
    std::map<K, V> impl_;
};

// ── MemTable ──
//   一张表 = 一个 MemTable. qualified_name 是 "db.schema.table" 形式.
class MemTable {
public:
    explicit MemTable(std::string qualified_name);

    // 写入新版本. seq 必须单调递增 (由 StorageEngine 分配, 见 next_seq()).
    // 不修改旧版本, 旧版本由 vacuum 处理.
    void put(int64_t row_id, const Tuple& t, uint64_t seq);

    // 写入 tombstone: xmax != 0 的空 payload, 等价于 "已删除" 标记.
    // 物理上仍占一个 InternalKey, 由 vacuum 在合适时机清理.
    void tombstone(int64_t row_id, TransactionId xmax, uint64_t seq);

    // 取该 row_id 最新版本. 没有返回 nullopt.
    std::optional<Tuple> get_latest(int64_t row_id) const;

    // 同 row_id 所有版本 (seq DESC). 给 vacuum / 历史读取用.
    std::vector<Tuple> get_versions(int64_t row_id) const;

    // 物理删除某 row_id 的全部版本. 返回删除的 entry 数.
    //   用途: non-txn remove, undo (把 row 还原到 pre-txn 状态), vacuum 物理清理.
    //   注意: 物理删除不等价于 MVCC 删除; 调用方负责确保没有活跃 reader.
    size_t erase_all_for(int64_t row_id);

    // 顺序扫: 按 row_id ASC, 同 row_id 内 seq DESC.
    // 物理顺序由 InternalKey 排序保证: user_key ASC, seq DESC.
    std::vector<Tuple> scan_all() const;

    // 真空: 删掉可清理的版本. 返回清理的 entry 数.
    //   条件: xmax != 0 && CLog::get_status(xmax) == COMMITTED
    //          && xmax < oldest_active_xid
    //  (此函数不持有 StorageEngine 锁, 真空期间其它线程可能 put)
    size_t vacuum(CLog& clog, TransactionId oldest_active_xid);

    size_t       size() const { return skiplist_.size(); }
    int64_t      max_row_id() const { return max_row_id_.load(std::memory_order_relaxed); }
    const std::string& name() const { return name_; }

private:
    std::string name_;
    SkipList<InternalKey, Tuple> skiplist_;
    std::atomic<int64_t> max_row_id_{0};
};

}  // namespace jiamiao
