/* ═══════════════════════════════════════════════════════════════════════
   bump.cpp — JMBumpContext 实现 (port of PG bump.c)
   ═══════════════════════════════════════════════════════════════════════ */

#include "common/bump.h"

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>

namespace jiamiao {

// ── Chunk header (8B): 低 8 位 = method id, 上 56 位 = ctx 指针 (64-bit) ──
static inline void BumpWriteHeader(void* user_ptr, BumpContext* ctx) {
    uint64_t* hdr = static_cast<uint64_t*>(user_ptr) - 1;
    uint64_t packed = (reinterpret_cast<uint64_t>(ctx) << 8) | (uint64_t)kMCTypeBump;
    *hdr = packed;
}

static inline void BumpReadHeader(void* user_ptr, BumpContext** ctx_out, uint64_t* method_out) {
    uint64_t packed = *(static_cast<uint64_t*>(user_ptr) - 1);
    *method_out = packed & 0xFFULL;
    *ctx_out    = reinterpret_cast<BumpContext*>(packed >> 8);
}

// ── 块内部: BumpBlock 实际持有 [header | data...] ──
//    data 起点 = BumpBlock + 1, 我们直接用 BumpBlock 自身 malloc, 然后在 BumpBlock 后拿 data.
//    但这要求 BumpBlock 大小 8 字节对齐 — 实际 sizeof(BumpBlock) 通常 < 8B, 不足一个指针.
//    简化: data 用独立 malloc, BumpBlock 持有指针.

namespace {
struct RawBlock {
    char*      data;
    Size       cap;
    RawBlock*  next;
    char*      freeptr;       // bump 游标
    char*      endptr;        // data + cap
};

RawBlock* BumpCreateBlock(Size cap) {
    RawBlock* blk = static_cast<RawBlock*>(std::malloc(sizeof(RawBlock)));
    if (blk == nullptr) throw std::bad_alloc();
    blk->data = static_cast<char*>(std::malloc(cap));
    if (blk->data == nullptr) { std::free(blk); throw std::bad_alloc(); }
    blk->cap     = cap;
    blk->next    = nullptr;
    blk->freeptr = blk->data;
    blk->endptr  = blk->data + cap;
    return blk;
}

void BumpFreeBlock(RawBlock* blk) {
    if (blk == nullptr) return;
    std::free(blk->data);
    std::free(blk);
}
}  // namespace

// 我们把 BumpContext 内部的 "current_block" 改为 RawBlock* (而非 BumpBlock*).
// 公开的 BumpContext 保留 BumpBlock* 字段但实际用 reinterpret 看待, 或:
// 干脆把 BumpContext 内部块指针改成兼容的.
// 简化方案: BumpContext 持 std::vector<RawBlock*> 或单链 RawBlock*. 但 BumpContext 自身是 C 风格 POD.
// 最终方案: BumpContext 持一个 "blocks" 指针 (RawBlock*), 整个链表.
//
// 我们把 BumpContext 的 firstchild 字段 (PG 风格) 重新解释为 "block list head".
// 不行, firstchild 是 parent link, 必须保留.
// 解决: 把 blocks 链放到 BumpContext 的扩展字段 (我们拥有的私有区域).

// 由于 BumpContext 现在定义为:
//   struct BumpContext : MemoryContextData {
//     BumpBlock*   current_block;   // 单链表头
//     Size         init_block_size;
//     Size         max_block_size;
//   };
// BumpBlock* 实际指向 RawBlock* (reinterpret_cast 互相转换).

// ── vtable ──
static const MemoryContextMethods kBumpMethods = {
    /* .alloc          = */ [](MemoryContext ctx, Size size, int flags) -> void* {
        return BumpAlloc(static_cast<BumpContext*>(ctx), size, flags);
    },
    /* .free_p         = */ [](void* /*ptr*/) {
        // Bump 不支持单 chunk free
    },
    /* .realloc        = */ [](void* ptr, Size size, int flags) -> void* {
        if (ptr == nullptr) return jmalloc(size);
        BumpContext* ctx;
        uint64_t method;
        BumpReadHeader(ptr, &ctx, &method);
        void* new_ptr = BumpAlloc(ctx, size, flags);
        if (new_ptr != nullptr && size > 0) {
            // 复制请求的 size 字节 (Bump 不追踪旧 size, 假定调用方 realloc 时 new_size ≤ old_size 即可)
            std::memcpy(new_ptr, ptr, size);
        }
        return new_ptr;
    },
    /* .reset          = */ [](MemoryContext ctx) { BumpReset(ctx); },
    /* .delete_context = */ [](MemoryContext ctx) { BumpDelete(ctx); },
    /* .get_chunk_context = */ [](void* ptr) -> MemoryContext {
        if (ptr == nullptr) return nullptr;
        BumpContext* ctx;
        uint64_t method;
        BumpReadHeader(ptr, &ctx, &method);
        return ctx;
    },
    /* .get_chunk_space = */ [](void* /*ptr*/) -> Size { return 0; },
    /* .is_empty       = */ [](MemoryContext ctx) -> bool { return BumpIsEmpty(ctx); },
};

// ── 创建 ──
MemoryContext JMBumpContextCreate(MemoryContext parent, const char* name,
                                  Size init_size, Size max_size) {
    BumpContext* ctx = static_cast<BumpContext*>(std::malloc(sizeof(BumpContext)));
    if (ctx == nullptr) throw std::bad_alloc();
    std::memset(ctx, 0, sizeof(BumpContext));
    ctx->type_tag        = kMCTypeBump;
    ctx->is_reset        = true;
    ctx->methods         = &kBumpMethods;
    ctx->name            = name;
    ctx->init_block_size = init_size;
    ctx->max_block_size  = max_size;
    if (parent != nullptr) {
        MemoryContextSetParent(ctx, parent);
    }
    return ctx;
}

// ── Alloc ──
void* BumpAlloc(BumpContext* ctx, Size size, int flags) {
    (void)flags;
    // 对齐到 8 字节 + 8B header
    Size aligned = (size + 7) & ~Size{7};
    Size total   = aligned + sizeof(uint64_t);

    auto head = reinterpret_cast<RawBlock**>(&ctx->current_block);

    // 大对象 (> max_block_size / 2): 独立 block, 整个走 freelist 路径 (Bump 暂不区分)
    if (total > ctx->max_block_size) {
        RawBlock* blk = BumpCreateBlock(total);
        blk->next    = *head;
        *head        = blk;
        void* user_ptr = blk->data + sizeof(uint64_t);
        BumpWriteHeader(user_ptr, ctx);
        ctx->mem_allocated += total;
        ctx->is_reset = false;
        return user_ptr;
    }

    // 尝试 current block
    if (*head != nullptr) {
        RawBlock* blk = *head;
        if (blk->freeptr + total <= blk->endptr) {
            void* user_ptr = blk->freeptr + sizeof(uint64_t);
            BumpWriteHeader(user_ptr, ctx);
            blk->freeptr += total;
            ctx->mem_allocated += total;
            ctx->is_reset = false;
            return user_ptr;
        }
    }

    // 开新 block, 大小按 total 倍增到 max
    Size new_cap = ctx->init_block_size;
    while (new_cap < total) new_cap *= 2;
    if (new_cap > ctx->max_block_size) new_cap = ctx->max_block_size;

    RawBlock* blk = BumpCreateBlock(new_cap);
    blk->next = *head;
    *head     = blk;

    void* user_ptr = blk->freeptr + sizeof(uint64_t);
    BumpWriteHeader(user_ptr, ctx);
    blk->freeptr += total;
    ctx->mem_allocated += total;
    ctx->is_reset = false;
    return user_ptr;
}

// ── Reset: 释放所有非首个块, 保留最大一块重置 freeptr ──
void BumpReset(MemoryContext ctx_raw) {
    BumpContext* ctx = static_cast<BumpContext*>(ctx_raw);
    auto head = reinterpret_cast<RawBlock**>(&ctx->current_block);
    if (*head == nullptr) {
        ctx->mem_allocated = 0;
        ctx->is_reset = true;
        return;
    }
    // 保留 first (最新开的, 最大 cap), 释放其余
    RawBlock* keep = *head;
    *head = keep->next;
    keep->next = nullptr;
    keep->freeptr = keep->data;  // 重置游标
    while (*head != nullptr) {
        RawBlock* next = (*head)->next;
        BumpFreeBlock(*head);
        *head = next;
    }
    // 重置后 mem_allocated = 0 (所有 user chunk 都失效)
    ctx->mem_allocated = 0;
    ctx->is_reset = true;
}

// ── Delete: 释放所有块, 释放 context 自身 ──
void BumpDelete(MemoryContext ctx_raw) {
    BumpContext* ctx = static_cast<BumpContext*>(ctx_raw);
    auto head = reinterpret_cast<RawBlock**>(&ctx->current_block);
    while (*head != nullptr) {
        RawBlock* next = (*head)->next;
        BumpFreeBlock(*head);
        *head = next;
    }
    std::free(ctx);
}

// ── IsEmpty ──
bool BumpIsEmpty(MemoryContext ctx_raw) {
    BumpContext* ctx = static_cast<BumpContext*>(ctx_raw);
    return ctx->mem_allocated == 0;
}

// ── Stats ──
void BumpStats(MemoryContext ctx_raw, Size* nblocks, Size* used) {
    BumpContext* ctx = static_cast<BumpContext*>(ctx_raw);
    auto head = reinterpret_cast<RawBlock**>(&ctx->current_block);
    Size nb = 0;
    for (RawBlock* p = *head; p != nullptr; p = p->next) ++nb;
    if (nblocks) *nblocks = nb;
    if (used)    *used    = ctx->mem_allocated;
}

}  // namespace jiamiao
