/* ══════════════════════════════════════════════════════════════════════
   memtable.h — Phase 2 LSM: 内存表 (MemTable) + 真并发 SkipList

   MemTable 是 LSM-Tree 的"内存 L0 切片":
     - 接受按 (InternalKey, Tuple*) 的 put
     - 同 row_id 多版本共存, 旧版本由 vacuum 清理
     - 提供 row_id ASC, 同 row_id 内 seq DESC 的 scan 顺序

   Phase 2 实现: SkipList 是真 Pugh 跳表 (单写多读 lock-free), 见 skiplist.h.
   数据节点走 Arena 分配, 节点不 free (arena 析构统一回收).

   vacuum 策略: 删掉 xmax != 0 且 clog.get_status(xmax) == COMMITTED
   且 xmax < oldest_active_xid 的版本. 调用方 (StorageEngine) 决定时机.
   ══════════════════════════════════════════════════════════════════════ */

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "arena.h"
#include "skiplist.h"
#include "transaction.h"  // TransactionId, CLog
#include "tuple.h"        // InternalKey, Tuple

namespace jiamiao {

// ── MemTable ──
//   一张表 = 一个 MemTable. qualified_name 是 "db.schema.table" 形式.
class MemTable {
public:
    explicit MemTable(std::string qualified_name);

    ~MemTable();

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

    size_t       size() const;
    int64_t      max_row_id() const { return max_row_id_.load(std::memory_order_relaxed); }
    const std::string& name() const { return name_; }

    // 暴露 Arena / SkipList 指针 (debug / 测试用)
    Arena*   arena()    const { return arena_.get(); }
    SkipList<InternalKey, Tuple>* skiplist() { return skiplist_.get(); }

private:
    std::string name_;
    std::unique_ptr<Arena> arena_;
    std::unique_ptr<SkipList<InternalKey, Tuple>> skiplist_;
    std::atomic<int64_t> max_row_id_{0};
};

}  // namespace jiamiao
