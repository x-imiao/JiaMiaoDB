/* ══════════════════════════════════════════════════════════════════════
   memtable.cpp — MemTable 实现
   ══════════════════════════════════════════════════════════════════════ */

#include "memtable.h"

#include <algorithm>
#include <utility>

namespace jiamiao {

// ── 构造 ──
MemTable::MemTable(std::string qualified_name)
    : name_(std::move(qualified_name)) {}

// ── put: 写入新版本 ──
//   旧版本保留 (MVCC), 同 row_id 不同 seq 共存
void MemTable::put(int64_t row_id, const Tuple& t, uint64_t seq) {
    InternalKey k{make_user_key(name_, row_id), seq};
    Tuple copy = t;  // 按值存, 拷贝 payload
    copy.hdr.payload_len = static_cast<uint32_t>(copy.payload.size());
    skiplist_.put(k, std::move(copy));

    int64_t cur = max_row_id_.load(std::memory_order_relaxed);
    while (row_id > cur && !max_row_id_.compare_exchange_weak(
            cur, row_id, std::memory_order_relaxed)) {
        // retry
    }
}

// ── tombstone: 写 xmax 标记 ──
void MemTable::tombstone(int64_t row_id, TransactionId xmax, uint64_t seq) {
    Tuple t;
    t.hdr.row_id = static_cast<uint64_t>(row_id);
    t.hdr.xmin   = 0;
    t.hdr.xmax   = static_cast<uint32_t>(xmax);
    t.hdr.cid    = 0;
    t.hdr.flags  = 0x1;  // bit0: tombstone
    put(row_id, t, seq);
}

// ── get_versions: 同 row_id 所有版本 (seq DESC) ──
std::vector<Tuple> MemTable::get_versions(int64_t row_id) const {
    std::string uk = make_user_key(name_, row_id);
    std::vector<Tuple> out;
    auto all = skiplist_.all();
    for (const auto& t : all) {
        if (make_user_key(name_, static_cast<int64_t>(t.hdr.row_id)) == uk) {
            out.push_back(t);
        }
    }
    return out;
}

// ── get_latest: 取最新版本 ──
std::optional<Tuple> MemTable::get_latest(int64_t row_id) const {
    auto versions = get_versions(row_id);
    if (versions.empty()) return std::nullopt;
    return versions.front();
}

// ── scan_all: 全表扫描, row_id ASC, 同 row_id 内 seq DESC ──
std::vector<Tuple> MemTable::scan_all() const {
    return skiplist_.all();
}

// ── vacuum: 返回可被物理清理的 entry 数 ──
//   判定: xmax != 0 && clog.get_status(xmax) == COMMITTED
//         && xmax < oldest_active_xid
size_t MemTable::vacuum(CLog& clog, TransactionId oldest_active_xid) {
    auto all = skiplist_.all();
    size_t can_purge = 0;
    for (const auto& t : all) {
        if (t.hdr.xmax == 0) continue;
        if (clog.get_status(t.hdr.xmax) != TransactionStatus::COMMITTED) continue;
        if (t.hdr.xmax >= oldest_active_xid) continue;
        ++can_purge;
    }
    return can_purge;
}

// ── erase_all_for: 删除某 row_id 全部版本 ──
//   物理删除, 跳过 visibility 检查. 调用方负责正确性.
size_t MemTable::erase_all_for(int64_t row_id) {
    std::string uk = make_user_key(name_, row_id);
    // range_with_keys 给精确 InternalKey
    InternalKey lo{uk, UINT64_MAX};
    InternalKey hi{uk, 0};
    auto entries = skiplist_.range_with_keys(lo, hi);
    size_t erased = 0;
    for (const auto& [k, t] : entries) {
        if (static_cast<int64_t>(t.hdr.row_id) != row_id) continue;
        if (skiplist_.erase(k)) ++erased;
    }
    return erased;
}

}  // namespace jiamiao
