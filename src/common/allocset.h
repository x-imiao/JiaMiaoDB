/* ═══════════════════════════════════════════════════════════════════════
   allocset.h — JMAllocSetContext (port of PG aset.h / aset.c)

   AllocSet 是 PG 的"通用" MemoryContext 实现:
     - 块链表 (BlockList): 大块由 malloc, 块内切 chunk
     - 11 个 power-of-2 freelist: 8/16/32/64/128/256/512/1K/2K/4K/8K
     - 请求 > 8K 走 AllocSetLarge: 独立块, free 时整块归还 malloc
     - Reset: 整块 free 掉, keeper block 留下
     - Delete: 释放所有块 + 释放 context 自身

   块头 (AllocBlock):
     [prev | next | freeptr | endptr | owner | set]
   keeper block 嵌在 AllocSetContext 结构尾部 (PG 同款).

   Chunk header (8B):
     [7:0]   method id (kMCTypeAllocSet = 1)
     [8]     external flag (1 = chunk 在自己独占的块上, free 时整块 ::free)
     [15:9]  freelist index (when external=0, 表示 chunk 大小属哪个 freelist)
     [63:16] unused / context-specific
   ═══════════════════════════════════════════════════════════════════════ */

#ifndef JIAMIAODB_COMMON_ALLOCSET_H
#define JIAMIAODB_COMMON_ALLOCSET_H

#include "common/memcontext.h"

namespace jiamiao {

// ── AllocSetContext 前向 ──
struct AllocSetContext;

// ── AllocBlock (公开) ──
//   块用独立 malloc, 头部 32B 上下, 后面是 user data 区.
struct AllocBlock {
    AllocBlock*    prev      = nullptr;
    AllocBlock*    next      = nullptr;
    char*          freeptr   = nullptr;   // 下一个可分配起点
    char*          endptr    = nullptr;   // data 末尾
    void*          owner     = nullptr;   // 指向 AllocSetContext
    AllocSetContext* aset    = nullptr;   // alias for owner
};

// ── AllocSetContext ──
struct AllocSetContext : MemoryContextData {
    AllocBlock*        blocks            = nullptr;  // 块链表头
    AllocBlock*        freeptr           = nullptr;  // 当前 active 块
    void*              freelist[kAllocSetNumFreelists] = {nullptr};  // 11 power-of-2 空闲链表头
    Size               init_block_size   = 0;
    Size               max_block_size    = 0;
    Size               next_block_size   = 0;        // 下一个新块大小 (倍增)
    Size               alloc_chunk_limit = kAllocChunkLimit;
    int                free_list_index   = -1;
};

// Chunk header flags / method id
constexpr uint64_t kAllocSetMethodId  = static_cast<uint64_t>(kMCTypeAllocSet);
constexpr uint64_t kChunkFlagExternal = (1ULL << 8);
constexpr uint64_t kChunkFlagFreeListIdx = (0xFULL << 9);  // 4 bits, 0-10
constexpr uint64_t kChunkHeaderMask   = 0xFFULL;

// 公开 API
void* AllocSetAlloc(AllocSetContext* ctx, Size size, int flags);
void  AllocSetFree(void* pointer);
void* AllocSetRealloc(void* pointer, Size size, int flags);
void  AllocSetReset(MemoryContext ctx);
void  AllocSetDelete(MemoryContext ctx);
MemoryContext AllocSetGetChunkContext(void* pointer);
Size   AllocSetGetChunkSpace(void* pointer);
bool   AllocSetIsEmpty(MemoryContext ctx);

}  // namespace jiamiao

#endif  // JIAMIAODB_COMMON_ALLOCSET_H
