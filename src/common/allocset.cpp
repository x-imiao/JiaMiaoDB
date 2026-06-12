/* ═══════════════════════════════════════════════════════════════════════
   allocset.cpp — JMAllocSetContext 实现 (port of PG aset.c)
   ═══════════════════════════════════════════════════════════════════════ */

#include "common/allocset.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>

namespace jiamiao {

// ── Chunk 布局 (16B header before user_ptr) ──
//   chunk_start + 0..8   : 8B ctx pointer (when allocated) OR freelist next (when free)
//   chunk_start + 8..16  : 8B header [low8: method | bit8: external | bits9..15: freelist idx]
//   chunk_start + 16 ..  : user data (size bytes), user_ptr = chunk_start + 16
//
// External chunk: 整个 block 独享, free 整块释放; bit 8 of header = 1.
// Small chunk: 进 freelist[idx], idx 由 size 决定; bit 8 of header = 0.

constexpr Size kChunkHdrSize = 16;  // 8B ctx + 8B hdr

// hdr (low 8 bits = method id) 在 user_ptr - 8
static inline void WriteHdr(void* user_ptr, Size index, bool external) {
    uint64_t packed = (uint64_t)kAllocSetMethodId
                    | (external ? kChunkFlagExternal : 0)
                    | ((index << 9) & kChunkFlagFreeListIdx);
    void* slot = static_cast<char*>(user_ptr) - 8;
    *reinterpret_cast<uint64_t*>(slot) = packed;
}

static inline uint64_t ReadHdr(void* user_ptr) {
    void* slot = static_cast<char*>(user_ptr) - 8;
    return *reinterpret_cast<uint64_t*>(slot);
}

// ctx pointer 在 user_ptr - 16
static inline void WriteCtx(void* user_ptr, AllocSetContext* ctx) {
    void* slot = static_cast<char*>(user_ptr) - 16;
    *reinterpret_cast<AllocSetContext**>(slot) = ctx;
}

static inline AllocSetContext* ReadCtx(void* user_ptr) {
    void* slot = static_cast<char*>(user_ptr) - 16;
    return *reinterpret_cast<AllocSetContext**>(slot);
}

// ── freelist index ──
static inline int AllocSetFreeListIndex(Size size) {
    if (size <= 8)    return 0;
    if (size <= 16)   return 1;
    if (size <= 32)   return 2;
    if (size <= 64)   return 3;
    if (size <= 128)  return 4;
    if (size <= 256)  return 5;
    if (size <= 512)  return 6;
    if (size <= 1024) return 7;
    if (size <= 2048) return 8;
    if (size <= 4096) return 9;
    return 10;
}

static inline Size AllocSetFreeListSize(int index) {
    return static_cast<Size>(8) << index;
}

// ── 块 (BlockStorage = AllocBlock + data 指针) ──
struct BlockStorage {
    AllocBlock  hdr;
    char*       data;       // malloc'd buffer 起点
    Size        cap;
    AllocSetContext* owner;
};

// 找 BlockStorage 起点: hdr 在 BlockStorage 内的偏移
static inline BlockStorage* BlockOf(AllocBlock* hdr) {
    if (hdr == nullptr) return nullptr;
    return reinterpret_cast<BlockStorage*>(
        reinterpret_cast<char*>(hdr) - offsetof(BlockStorage, hdr));
}

static BlockStorage* AllocSetCreateBlock(Size cap) {
    cap = (cap + 7) & ~Size{7};
    BlockStorage* bs = static_cast<BlockStorage*>(std::malloc(sizeof(BlockStorage)));
    if (bs == nullptr) throw std::bad_alloc();
    bs->data = static_cast<char*>(std::malloc(cap));
    if (bs->data == nullptr) { std::free(bs); throw std::bad_alloc(); }
    bs->cap  = cap;
    bs->owner = nullptr;
    bs->hdr.freeptr = bs->data;
    bs->hdr.endptr  = bs->data + cap;
    return bs;
}

static void AllocSetDestroyBlock(BlockStorage* bs) {
    if (bs == nullptr) return;
    std::free(bs->data);
    std::free(bs);
}

static Size InitSizeMin() { return 1024; }

// ── vtable ──
static const MemoryContextMethods kAllocSetMethods = {
    /* .alloc          = */ [](MemoryContext ctx, Size size, int flags) -> void* {
        return AllocSetAlloc(static_cast<AllocSetContext*>(ctx), size, flags);
    },
    /* .free_p         = */ [](void* ptr) { AllocSetFree(ptr); },
    /* .realloc        = */ [](void* ptr, Size size, int flags) -> void* {
        return AllocSetRealloc(ptr, size, flags);
    },
    /* .reset          = */ [](MemoryContext ctx) { AllocSetReset(ctx); },
    /* .delete_context = */ [](MemoryContext ctx) { AllocSetDelete(ctx); },
    /* .get_chunk_context = */ [](void* ptr) -> MemoryContext {
        return AllocSetGetChunkContext(ptr);
    },
    /* .get_chunk_space = */ [](void* ptr) -> Size {
        return AllocSetGetChunkSpace(ptr);
    },
    /* .is_empty       = */ [](MemoryContext ctx) -> bool {
        return AllocSetIsEmpty(ctx);
    },
};

// ── 创建 ──
MemoryContext JMAllocSetContextCreate(MemoryContext parent, const char* name,
                                      Size min_size, Size init_size, Size max_size) {
    (void)min_size;
    AllocSetContext* ctx = static_cast<AllocSetContext*>(std::malloc(sizeof(AllocSetContext)));
    if (ctx == nullptr) throw std::bad_alloc();
    std::memset(ctx, 0, sizeof(AllocSetContext));
    ctx->type_tag        = kMCTypeAllocSet;
    ctx->is_reset        = true;
    ctx->methods         = &kAllocSetMethods;
    ctx->name            = name;
    ctx->init_block_size = init_size;
    ctx->max_block_size  = max_size;
    ctx->next_block_size = init_size;
    ctx->alloc_chunk_limit = kAllocChunkLimit;
    if (max_size < kAllocSetDefaultInitSize * 4) {
        ctx->alloc_chunk_limit = max_size / kAllocChunkFraction;
        if (ctx->alloc_chunk_limit < 64) ctx->alloc_chunk_limit = 64;
    }
    if (parent != nullptr) {
        MemoryContextSetParent(ctx, parent);
    }
    return ctx;
}

// ── Alloc ──
void* AllocSetAlloc(AllocSetContext* ctx, Size size, int flags) {
    (void)flags;
    if (size == 0) size = 1;
    Size alloc_size = size + kChunkHdrSize;

    // 大对象: 独立块
    if (alloc_size > ctx->alloc_chunk_limit) {
        Size cap = (alloc_size + 7) & ~Size{7};
        BlockStorage* bs = AllocSetCreateBlock(cap);
        bs->owner = ctx;
        bs->hdr.owner = ctx;
        bs->hdr.aset  = ctx;
        bs->hdr.prev  = nullptr;
        bs->hdr.next  = ctx->blocks;
        if (ctx->blocks != nullptr) ctx->blocks->prev = &bs->hdr;
        ctx->blocks = &bs->hdr;

        void* user_ptr = static_cast<char*>(bs->data) + kChunkHdrSize;
        WriteHdr(user_ptr, 0, true);
        WriteCtx(user_ptr, ctx);
        ctx->mem_allocated += cap + sizeof(BlockStorage);
        ctx->is_reset = false;
        return user_ptr;
    }

    // 小对象
    int idx = AllocSetFreeListIndex(alloc_size);
    Size bucket_size = AllocSetFreeListSize(idx);  // 实际占用 = 桶大小 (统一释放/复用)

    // 1. freelist
    if (ctx->freelist[idx] != nullptr) {
        void* chunk_hdr = ctx->freelist[idx];
        ctx->freelist[idx] = *static_cast<void**>(chunk_hdr);
        void* user_ptr = static_cast<char*>(chunk_hdr) + kChunkHdrSize;
        WriteHdr(user_ptr, idx, false);
        WriteCtx(user_ptr, ctx);
        ctx->mem_allocated += bucket_size;
        ctx->is_reset = false;
        return user_ptr;
    }

    // 2. 块内 bump
    if (ctx->freeptr != nullptr) {
        AllocBlock* blk = ctx->freeptr;
        if (blk->freeptr + bucket_size <= blk->endptr) {
            char* hdr = blk->freeptr;
            blk->freeptr += bucket_size;   // 按桶大小推进, 不是 alloc_size
            void* user_ptr = hdr + kChunkHdrSize;
            WriteHdr(user_ptr, idx, false);
            WriteCtx(user_ptr, ctx);
            ctx->mem_allocated += bucket_size;
            ctx->is_reset = false;
            return user_ptr;
        }
    }

    // 3. 开新块
    Size new_size = ctx->next_block_size;
    if (new_size < InitSizeMin()) new_size = InitSizeMin();
    if (bucket_size > new_size) new_size = bucket_size + sizeof(BlockStorage);
    new_size = (new_size + 7) & ~Size{7};

    BlockStorage* bs = AllocSetCreateBlock(new_size);
    bs->owner = ctx;
    bs->hdr.owner = ctx;
    bs->hdr.aset  = ctx;
    bs->hdr.prev  = nullptr;
    bs->hdr.next  = ctx->blocks;
    if (ctx->blocks != nullptr) ctx->blocks->prev = &bs->hdr;
    ctx->blocks    = &bs->hdr;
    ctx->freeptr   = &bs->hdr;

    ctx->next_block_size *= 2;
    if (ctx->next_block_size > ctx->max_block_size) {
        ctx->next_block_size = ctx->max_block_size;
    }

    char* hdr = bs->data;
    bs->hdr.freeptr = bs->data + bucket_size;   // 第一个 chunk 已占 bucket_size
    void* user_ptr = hdr + kChunkHdrSize;
    WriteHdr(user_ptr, idx, false);
    WriteCtx(user_ptr, ctx);
    ctx->mem_allocated += bucket_size;
    ctx->is_reset = false;
    return user_ptr;
}

// ── Free ──
void AllocSetFree(void* pointer) {
    if (pointer == nullptr) return;
    void* chunk_start = static_cast<char*>(pointer) - kChunkHdrSize;
    uint64_t hdr = ReadHdr(pointer);
    bool external = (hdr & kChunkFlagExternal) != 0;

    if (external) {
        // external chunk: 不支持单 free, 需 reset 整 context
        // 实现找回 block: ctx 已知, 扫描链表找含此 chunk 的 block
        AllocSetContext* ctx = static_cast<AllocSetContext*>(ReadCtx(pointer));
        if (ctx == nullptr) return;
        for (AllocBlock* b = ctx->blocks; b != nullptr; b = b->next) {
            BlockStorage* bs = BlockOf(b);
            if (reinterpret_cast<uintptr_t>(bs->data) <= reinterpret_cast<uintptr_t>(chunk_start) &&
                reinterpret_cast<uintptr_t>(chunk_start) < reinterpret_cast<uintptr_t>(bs->data + bs->cap)) {
                // 整块释放
                if (b->prev != nullptr) b->prev->next = b->next;
                else                    ctx->blocks = b->next;
                if (b->next != nullptr) b->next->prev = b->prev;
                if (ctx->freeptr == b) ctx->freeptr = ctx->blocks;
                ctx->mem_allocated -= (bs->cap + sizeof(BlockStorage));
                AllocSetDestroyBlock(bs);
                return;
            }
        }
        return;
    }

    int idx = static_cast<int>((hdr & kChunkFlagFreeListIdx) >> 9);
    AllocSetContext* ctx = static_cast<AllocSetContext*>(ReadCtx(pointer));
    if (ctx == nullptr) return;
    *static_cast<void**>(chunk_start) = ctx->freelist[idx];
    ctx->freelist[idx] = chunk_start;
    ctx->mem_allocated -= AllocSetFreeListSize(idx);
}

// ── Realloc ──
void* AllocSetRealloc(void* pointer, Size size, int flags) {
    if (pointer == nullptr) return jmalloc(size);
    if (size == 0) { AllocSetFree(pointer); return nullptr; }
    AllocSetContext* ctx = static_cast<AllocSetContext*>(AllocSetGetChunkContext(pointer));
    if (ctx == nullptr) return nullptr;
    void* new_ptr = AllocSetAlloc(ctx, size, flags);
    if (new_ptr != nullptr) {
        Size old_size = AllocSetGetChunkSpace(pointer);
        Size copy_size = (old_size < size) ? old_size : size;
        std::memcpy(new_ptr, pointer, copy_size);
        AllocSetFree(pointer);
    }
    return new_ptr;
}

// ── Reset ──
void AllocSetReset(MemoryContext ctx_raw) {
    AllocSetContext* ctx = static_cast<AllocSetContext*>(ctx_raw);
    if (ctx->blocks != nullptr) {
        // 保留 first 块, 释放后续
        AllocBlock* keep = ctx->blocks;
        if (keep->next != nullptr) {
            AllocBlock* p = keep->next;
            while (p != nullptr) {
                AllocBlock* next = p->next;
                AllocSetDestroyBlock(BlockOf(p));
                p = next;
            }
            keep->next = nullptr;
        }
        // 重置 keep 块: freeptr 回到 data 起点
        BlockStorage* bs = BlockOf(keep);
        keep->freeptr = bs->data;
    }
    // 清空 freelist
    for (Size i = 0; i < kAllocSetNumFreelists; ++i) {
        ctx->freelist[i] = nullptr;
    }
    ctx->mem_allocated = 0;
    ctx->is_reset = true;
    ctx->next_block_size = ctx->init_block_size;
}

// ── Delete ──
void AllocSetDelete(MemoryContext ctx_raw) {
    AllocSetContext* ctx = static_cast<AllocSetContext*>(ctx_raw);
    AllocBlock* p = ctx->blocks;
    while (p != nullptr) {
        AllocBlock* next = p->next;
        AllocSetDestroyBlock(BlockOf(p));
        p = next;
    }
    std::free(ctx);
}

// ── GetChunkContext ──
MemoryContext AllocSetGetChunkContext(void* pointer) {
    if (pointer == nullptr) return nullptr;
    return ReadCtx(pointer);
}

Size AllocSetGetChunkSpace(void* pointer) {
    if (pointer == nullptr) return 0;
    uint64_t hdr = ReadHdr(pointer);
    bool external = (hdr & kChunkFlagExternal) != 0;
    int idx = static_cast<int>((hdr & kChunkFlagFreeListIdx) >> 9);
    if (external) return 0;
    return AllocSetFreeListSize(idx) - kChunkHdrSize;
}

bool AllocSetIsEmpty(MemoryContext ctx_raw) {
    AllocSetContext* ctx = static_cast<AllocSetContext*>(ctx_raw);
    return ctx->mem_allocated == 0;
}

}  // namespace jiamiao
