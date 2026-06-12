/* ═══════════════════════════════════════════════════════════════════════
   arena.cpp — Arena 实现 (Phase 3: 改用 jmalloc/jmfree)
   ═══════════════════════════════════════════════════════════════════════ */

#include "arena.h"
#include "common/jm_alloc.h"

#include <cstdlib>
#include <new>
#include <utility>

namespace jiamiao {

// ── 块分配 (独立块, 用于大对象) ──
std::unique_ptr<Arena::Block> Arena::alloc_block(size_t cap) {
    // cap 向上对齐到 8 字节
    cap = (cap + 7) & ~size_t{7};
    // Phase 3: 走 jmalloc, 字节进 CurrentMemoryContext (由 MemTable 持有的 EngineContext)
    char* buf = static_cast<char*>(jmalloc(cap));
    if (!buf) {
        throw std::bad_alloc();
    }
    auto blk = std::make_unique<Block>();
    blk->data = buf;
    blk->cap  = cap;
    blk->used = 0;
    return blk;
}

// ── 块内分配 (假定能 fit) ──
void* Arena::alloc_in_block(Block& blk, size_t bytes, size_t align) {
    // 计算对齐后的起始地址
    uintptr_t start = reinterpret_cast<uintptr_t>(blk.data + blk.used);
    uintptr_t aligned = (start + align - 1) & ~(uintptr_t{align - 1});
    size_t pad = static_cast<size_t>(aligned - start);
    if (blk.used + pad + bytes > blk.cap) {
        return nullptr;  // 信号: 不够, 调用方开新块
    }
    blk.used += pad + bytes;
    return reinterpret_cast<void*>(aligned);
}

// ── 构造 ──
Arena::Arena() = default;

// ── 析构 ──
//   unique_ptr<Block> 链递归释放; 块内 char[] 走 jmfree.
Arena::~Arena() = default;

// ── allocate ──
void* Arena::allocate(size_t bytes) {
    return allocate_aligned(bytes, 8);
}

void* Arena::allocate_aligned(size_t bytes, size_t align) {
    if (bytes == 0) return nullptr;
    std::lock_guard<std::mutex> lk(mutex_);

    // 大对象: 走独立块, 不入 bump 链表
    if (bytes > kLargeAllocThreshold) {
        auto blk = alloc_block(bytes + align);
        void* p = alloc_in_block(*blk, bytes, align);
        bytes_used_ += bytes;
        ++block_count_;
        // 把独立块挂到链表头 (next 链), 不做 bump
        blk->next = std::move(head_);
        head_ = std::move(blk);
        return p;
    }

    // 小对象: 尝试在 current_ 块内分配; 失败则开新块
    if (current_) {
        void* p = alloc_in_block(*current_, bytes, align);
        if (p) {
            bytes_used_ += bytes;
            return p;
        }
    }
    // 找链表里第一个有空间的块 (极端: 之前 reset 过 head 但 current_ 还在)
    for (Block* b = head_.get(); b != current_; b = b->next.get()) {
        if (b->cap - b->used >= bytes + align) {
            void* p = alloc_in_block(*b, bytes, align);
            if (p) {
                bytes_used_ += bytes;
                return p;
            }
        }
    }

    // 开新块
    auto blk = alloc_block(kBlockSize);
    Block* raw = blk.get();
    blk->next = std::move(head_);
    head_ = std::move(blk);
    current_ = raw;
    ++block_count_;

    void* p = alloc_in_block(*current_, bytes, align);
    bytes_used_ += bytes;
    return p;
}

// ── reset ──
void Arena::reset() {
    std::lock_guard<std::mutex> lk(mutex_);
    // 释放每个块的 jmalloc 字节 (注意: jmfree 走 freelist 复用, 不立即归还 OS)
    for (Block* b = head_.get(); b != nullptr; b = b->next.get()) {
        if (b->data != nullptr) {
            jmfree(b->data);
        }
        b->data = nullptr;
        b->used = 0;
        b->cap  = 0;
    }
    // 释放块节点 (unique_ptr 链)
    head_.reset();
    current_ = nullptr;
    bytes_used_  = 0;
    // block_count_ 保留 (用于观察历史 peak); 如想清零也行
}

}  // namespace jiamiao
