/* ═══════════════════════════════════════════════════════════════════════
   bump.h — JMBumpContext (port of PG bump.c)

   BumpContext: pure bump allocator, no per-chunk free.
   - BumpAlloc: 走当前 block 末尾, 不够就开新 block (倍增到 max_block_size)
   - BumpFree: no-op (chunk 不可单独 free, 等 reset 整 context 一起释放)
   - BumpReset: free 所有 block, 重置 first block

   用途: SkipList 节点 (Phase 2 Arena 等价物), WAL bytes scratch, 任何 "分配后从不释放" 的场景.

   Chunk header layout (8B, sizeof(uint64_t)):
       [63:8] unused / set to 0
       [7:0]  method_id (low 8 bits, = kMCTypeBump)
   详细: allocset.cpp / bump.cpp 各自实现.
   ═══════════════════════════════════════════════════════════════════════ */

#ifndef JIAMIAODB_COMMON_BUMP_H
#define JIAMIAODB_COMMON_BUMP_H

#include "common/memcontext.h"

namespace jiamiao {

// ── BumpBlock: 块头 (放在 block 起始, 后跟 user data) ──
struct BumpBlock {
    BumpBlock*   next        = nullptr;   // 单链表, 仅由 BumpContext 内部维护
    char*        freeptr     = nullptr;   // 下一个 free byte 起点
    char*        endptr      = nullptr;   // block 末尾 (exclusive)
    MemoryContext owner       = nullptr;   // 反向指针
};

// ── BumpContext ──
struct BumpContext : MemoryContextData {
    BumpBlock*   current_block = nullptr;  // 单链表头
    Size         init_block_size = 0;
    Size         max_block_size  = 0;
};

// Chunk header 编码: 8B uint64, low 8 bits = method id
constexpr uint64_t kBumpMethodId = static_cast<uint64_t>(kMCTypeBump);

// Bump chunk header 写入
inline void BumpSetChunkHeader(void* chunk_data_ptr) {
    uint64_t* hdr = static_cast<uint64_t*>(chunk_data_ptr) - 1;
    *hdr = kBumpMethodId;
}

// 从 chunk 头读 method id (for jmfree 调度)
inline uint64_t BumpGetChunkHeader(void* chunk_data_ptr) {
    uint64_t* hdr = static_cast<uint64_t*>(chunk_data_ptr) - 1;
    return *hdr;
}

// 公开 API (实现见 bump.cpp)
void* BumpAlloc(BumpContext* ctx, Size size, int flags);
void  BumpFree(void* pointer);            // no-op
void  BumpReset(MemoryContext ctx);
void  BumpDelete(MemoryContext ctx);
bool  BumpIsEmpty(MemoryContext ctx);
void  BumpStats(MemoryContext ctx, Size* nblocks, Size* used);

}  // namespace jiamiao

#endif  // JIAMIAODB_COMMON_BUMP_H
