/* ═══════════════════════════════════════════════════════════════════════
   arena.h — Phase 2 LSM: 块式 Bump Allocator (Arena)

   用途: 给 SkipList 节点 + Tuple payload 分配内存, 减少 malloc 次数 90%+.
   释放: 整块释放 (reset() 或析构). 不支持单对象 free (符合 LSM 写多删少特征).

   设计:
   - 64KB 定长块链表, 块内 bump pointer 分配
   - 8 字节对齐 (SkipList atomic<Node*>, Tuple payload 都要求 8B 对齐)
   - 大对象 (> 块 1/4 = 16KB) 走独立块, 避免大块卡死小块
   - 线程安全: 内部 std::mutex (Phase 2 写者单线程, 不必 lock-free)
   - 失败: 抛 std::bad_alloc (符合 std::allocator 习惯)

   Phase 3 计划: reset() 走 flush (memtable → SST), 新 memtable 拿新 arena.
   ═══════════════════════════════════════════════════════════════════════ */

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

namespace jiamiao {

class Arena {
public:
    // 单块大小: 64KB, 经验值, 覆盖多数 SkipList 节点 (几十字节) + 大量小 Tuple
    static constexpr size_t kBlockSize = 64 * 1024;
    // 独立块阈值: 单次分配 > 块 1/4 走独立块, 避免大块"卡"住 bump pointer
    static constexpr size_t kLargeAllocThreshold = kBlockSize / 4;

    Arena();
    ~Arena();

    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    // 分配 bytes 字节, 返回 8 字节对齐的指针
    void* allocate(size_t bytes);

    // 分配 bytes 字节, 显式指定 align (8/16/64), align 必须是 2 的幂
    void* allocate_aligned(size_t bytes, size_t align);

    // 当前已分配出去 (用户拿到) 的总字节数
    size_t bytes_used() const { return bytes_used_; }

    // 含 block 头在内的总内存开销 (估块数 × kBlockSize, 简化估计)
    size_t bytes_allocated() const { return block_count_ * kBlockSize; }

    // 块数 (调试 / 监控)
    size_t block_count() const { return block_count_; }

    // 释放所有块, bytes_used_/block_count_ 清零.
    // Phase 2 仅析构时自动调; Phase 3 在 memtable flush 时显式调.
    // 不安全: 调用方必须保证 arena 内所有对象都不再被引用 (SkipList 节点已摘除).
    void reset();

private:
    struct Block {
        std::unique_ptr<Block> next;
        char*  data;        // mmap'd / new[] 块
        size_t cap;         // 块容量
        size_t used;        // 已用字节数
    };

    // 分配新块 (独立块, 不入链表)
    std::unique_ptr<Block> alloc_block(size_t cap);

    // 块内分配 (假定 bytes <= 块剩余空间, 已对齐)
    void* alloc_in_block(Block& blk, size_t bytes, size_t align);

    std::unique_ptr<Block> head_;     // 块链表头
    Block* current_ = nullptr;        // 当前分配块 (链表末尾)
    size_t bytes_used_  = 0;
    size_t block_count_ = 0;
    std::mutex mutex_;
};

}  // namespace jiamiao
