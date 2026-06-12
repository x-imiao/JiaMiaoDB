/* ═══════════════════════════════════════════════════════════════════════
   memcontext.cpp — Memory Context 顶层实现 (mcxt.c port)
   ═══════════════════════════════════════════════════════════════════════ */

#include "common/memcontext.h"

#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <utility>

namespace jiamiao {

// ── 全局变量 ──
MemoryContext          TopMemoryContext  = nullptr;
MemoryContext          ErrorContext      = nullptr;
thread_local MemoryContext CurrentMemoryContext = nullptr;

// 前向: 底层 AllocSet / Bump 的 chunk header method_id
namespace internal {
    // AllocSet / Bump 都用 [method_id: 1 byte][extra: 7 bytes] 的 8B chunk header
    // AllocSet method_id = kMCTypeAllocSet, Bump method_id = kMCTypeBump.
    // 详细布局见 allocset.cpp / bump.cpp.
}

// ── 通用: 注册 callback 到 reset_cbs 链头 ──
void MemoryContextRegisterResetCallback(MemoryContext ctx, MemoryContextCallback* cb) {
    cb->next = ctx->reset_cbs;
    ctx->reset_cbs = cb;
    ctx->is_reset = false;
}

// ── 通用: 触发 reset_cbs (按注册反序) ──
static void MemoryContextCallResetCallbacks(MemoryContext ctx) {
    while (ctx->reset_cbs != nullptr) {
        MemoryContextCallback* cb = ctx->reset_cbs;
        ctx->reset_cbs = cb->next;
        cb->func(cb->arg);
    }
}

// ── 通用: 链接子到父的 firstchild 链 ──
static void MemoryContextLinkChild(MemoryContext parent, MemoryContext child) {
    child->parent = parent;
    if (parent->firstchild == nullptr) {
        child->prevchild = child;
        child->nextchild = nullptr;
        parent->firstchild = child;
    } else {
        child->prevchild = parent->firstchild->prevchild;
        child->nextchild = parent->firstchild;
        parent->firstchild->prevchild->nextchild = child;
        parent->firstchild->prevchild = child;
        parent->firstchild = child;
    }
}

// ── 通用: 从父的 firstchild 链摘除 ──
//   list 是双向循环 prev + 单向 next (PG 风格):
//     firstchild = 链头 (最新加的)
//     head->prevchild = tail (循环), tail->nextchild = nullptr
//     非 head 节点 prev/next 都是普通 prev/next
static void MemoryContextUnlinkChild(MemoryContext ctx) {
    if (ctx->parent == nullptr) return;
    MemoryContext parent = ctx->parent;
    if (parent->firstchild == nullptr) return;  // 已被摘除

    if (ctx->prevchild == ctx && parent->firstchild == ctx) {
        // 唯一节点
        parent->firstchild = nullptr;
        ctx->prevchild = nullptr;
        ctx->nextchild = nullptr;
        ctx->parent = nullptr;
        return;
    }

    if (parent->firstchild == ctx) {
        // ctx 是头: 新头 = ctx->nextchild; 新头的 prevchild 应继承原 tail (= ctx->prevchild)
        MemoryContext new_head = ctx->nextchild;
        if (new_head != nullptr) {
            new_head->prevchild = ctx->prevchild;
        }
        parent->firstchild = new_head;
    } else {
        // ctx 在中间或尾
        ctx->prevchild->nextchild = ctx->nextchild;
        if (ctx->nextchild != nullptr) {
            ctx->nextchild->prevchild = ctx->prevchild;
        } else {
            // ctx 是 tail: 更新头的 prevchild 指向新 tail
            parent->firstchild->prevchild = ctx->prevchild;
        }
    }
    ctx->prevchild = nullptr;
    ctx->nextchild = nullptr;
    ctx->parent = nullptr;
}

// ── Init: 在 main() 第一行调用, 创建 TopMemoryContext + ErrorContext ──
void MemoryContextInit() {
    if (TopMemoryContext != nullptr) return;  // 幂等

    TopMemoryContext = JMAllocSetContextCreate(nullptr, "TopMemoryContext",
                                                kAllocSetDefaultMinSize,
                                                kAllocSetDefaultInitSize,
                                                kAllocSetDefaultMaxSize);
    ErrorContext = JMAllocSetContextCreate(TopMemoryContext, "ErrorContext",
                                            0, 8 * 1024, 8 * 1024);
    CurrentMemoryContext = TopMemoryContext;
}

// ── Reset: 调用 reset callbacks + 子树 reset + 自身 reset ──
void MemoryContextReset(MemoryContext ctx) {
    // 1. 先 reset children (按 PG: 先 children 再自身)
    MemoryContextResetChildren(ctx);
    // 2. 触发本 context 的 reset callbacks
    MemoryContextCallResetCallbacks(ctx);
    // 3. 调用类型特定的 reset
    ctx->methods->reset(ctx);
}

void MemoryContextResetChildren(MemoryContext ctx) {
    MemoryContext child = ctx->firstchild;
    while (child != nullptr) {
        MemoryContext next = child->nextchild;
        // 递归 reset
        MemoryContextReset(child);
        child = next;
    }
}

// ── Delete: 等同于 reset + 自身 delete ──
void MemoryContextDelete(MemoryContext ctx) {
    if (ctx == nullptr) return;
    // 1. 触发 callbacks
    MemoryContextCallResetCallbacks(ctx);
    // 2. 递归 delete children
    MemoryContextDeleteChildren(ctx);
    // 3. 摘除自身
    MemoryContextUnlinkChild(ctx);
    // 4. 调类型特定 delete
    ctx->methods->delete_context(ctx);
}

void MemoryContextDeleteChildren(MemoryContext ctx) {
    while (ctx->firstchild != nullptr) {
        MemoryContext child = ctx->firstchild;
        // Unlink first to avoid recursion
        MemoryContextUnlinkChild(child);
        MemoryContextCallResetCallbacks(child);
        MemoryContextDeleteChildren(child);
        child->methods->delete_context(child);
    }
}

void MemoryContextSetParent(MemoryContext ctx, MemoryContext parent) {
    MemoryContextUnlinkChild(ctx);
    if (parent != nullptr) {
        MemoryContextLinkChild(parent, ctx);
    }
}

void MemoryContextSetIdentifier(MemoryContext ctx, const char* ident) {
    ctx->ident = ident;
}

MemoryContext MemoryContextGetParent(MemoryContext ctx) {
    return ctx->parent;
}

bool MemoryContextIsEmpty(MemoryContext ctx) {
    // 自己空 且 所有 children 都空
    if (!ctx->methods->is_empty(ctx)) return false;
    for (MemoryContext c = ctx->firstchild; c != nullptr; c = c->nextchild) {
        if (!MemoryContextIsEmpty(c)) return false;
    }
    return true;
}

Size MemoryContextMemAllocated(MemoryContext ctx, bool recurse) {
    Size total = ctx->mem_allocated;
    if (recurse) {
        for (MemoryContext c = ctx->firstchild; c != nullptr; c = c->nextchild) {
            total += MemoryContextMemAllocated(c, true);
        }
    }
    return total;
}

// ── 顶层分配 API (走 CurrentMemoryContext) ──
//   线程首调: 其它线程的 thread_local CurrentMemoryContext 默认 nullptr,
//   此时落到 TopMemoryContext (进程级).
static inline MemoryContext EffectiveCurrent() {
    if (CurrentMemoryContext == nullptr) {
        if (TopMemoryContext == nullptr) {
            // 首次访问 (静态初始化之前) — 同步建 TopMemoryContext
            MemoryContextInit();
        }
        CurrentMemoryContext = TopMemoryContext;
        return TopMemoryContext;
    }
    return CurrentMemoryContext;
}

void* jmalloc(Size size) {
    return MemoryContextAlloc(EffectiveCurrent(), size);
}

void* jmalloc0(Size size) {
    return MemoryContextAllocZero(EffectiveCurrent(), size);
}

void* jmalloc_aligned(Size size, Size align) {
    return MemoryContextAllocAligned(EffectiveCurrent(), size, align, 0);
}

void* jmalloc_extended(Size size, int flags) {
    return MemoryContextAllocExtended(EffectiveCurrent(), size, flags);
}

void* jrealloc(void* pointer, Size size) {
    return MemoryContextRealloc(pointer, size);
}

void* jrealloc0(void* pointer, Size old_size, Size new_size) {
    void* new_ptr = MemoryContextRealloc(pointer, new_size);
    if (new_size > old_size) {
        std::memset(static_cast<char*>(new_ptr) + old_size, 0, new_size - old_size);
    }
    return new_ptr;
}

void* jrealloc_array(void* pointer, Size old_count, Size new_count, Size elem_size) {
    Size old_size = old_count * elem_size;
    Size new_size = new_count * elem_size;
    return jrealloc0(pointer, old_size, new_size);
}

void jmfree(void* pointer) {
    MemoryContextFree(pointer);
}

char* jmstrdup(const char* s) {
    return MemoryContextStrdup(EffectiveCurrent(), s);
}

char* jmstrndup(const char* s, Size len) {
    return MemoryContextStrndup(EffectiveCurrent(), s, len);
}

char* jmsprintf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap_copy;
    va_copy(ap_copy, ap);
    int needed = std::vsnprintf(nullptr, 0, fmt, ap);
    va_end(ap);
    if (needed < 0) {
        va_end(ap_copy);
        return nullptr;
    }
    char* buf = static_cast<char*>(jmalloc(static_cast<Size>(needed) + 1));
    std::vsnprintf(buf, static_cast<Size>(needed) + 1, fmt, ap_copy);
    va_end(ap_copy);
    return buf;
}

// ── Direct context API ──
void* MemoryContextAlloc(MemoryContext ctx, Size size) {
    return MemoryContextAllocExtended(ctx, size, 0);
}

void* MemoryContextAllocZero(MemoryContext ctx, Size size) {
    return MemoryContextAllocExtended(ctx, size, kMCXT_ALLOC_ZERO);
}

void* MemoryContextAllocExtended(MemoryContext ctx, Size size, int flags) {
    // 检查 size 上限
    if ((flags & kMCXT_ALLOC_HUGE) == 0 && size > kMaxAllocSize) {
        throw std::bad_alloc();
    }
    void* ptr = ctx->methods->alloc(ctx, size, flags);
    if ((flags & kMCXT_ALLOC_ZERO) != 0 && ptr != nullptr && size > 0) {
        std::memset(ptr, 0, size);
    }
    ctx->is_reset = false;
    return ptr;
}

void* MemoryContextAllocAligned(MemoryContext ctx, Size size, Size align, int flags) {
    // 简单实现: 对齐 align ≤ 8 时走 alloc; > 8 时多分配 align - 8 字节然后调齐
    if (align <= 8) {
        return MemoryContextAllocExtended(ctx, size, flags);
    }
    // 申请 size + align - 8 字节, 找 8 字节对齐的起点
    Size pad_size = size + align - 8;
    char* raw = static_cast<char*>(MemoryContextAllocExtended(ctx, pad_size, flags));
    uintptr_t start = reinterpret_cast<uintptr_t>(raw);
    uintptr_t aligned = (start + align - 1) & ~(static_cast<uintptr_t>(align) - 1);
    Size offset = static_cast<Size>(aligned - start);
    return raw + offset;
}

void* MemoryContextRealloc(void* pointer, Size size) {
    if (pointer == nullptr) return MemoryContextAlloc(CurrentMemoryContext, size);
    if (size == 0) { jmfree(pointer); return nullptr; }
    MemoryContext ctx = GetMemoryChunkContext(pointer);
    return ctx->methods->realloc(pointer, size, 0);
}

void MemoryContextFree(void* pointer) {
    if (pointer == nullptr) return;
    MemoryContext ctx = GetMemoryChunkContext(pointer);
    if (ctx == nullptr) return;
    ctx->methods->free_p(pointer);
}

char* MemoryContextStrdup(MemoryContext ctx, const char* s) {
    if (s == nullptr) return nullptr;
    Size len = std::strlen(s);
    char* buf = static_cast<char*>(MemoryContextAlloc(ctx, len + 1));
    std::memcpy(buf, s, len + 1);
    return buf;
}

char* MemoryContextStrndup(MemoryContext ctx, const char* s, Size len) {
    if (s == nullptr) return nullptr;
    char* buf = static_cast<char*>(MemoryContextAlloc(ctx, len + 1));
    std::memcpy(buf, s, len);
    buf[len] = '\0';
    return buf;
}

char* MemoryContextSprintf(MemoryContext ctx, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap_copy;
    va_copy(ap_copy, ap);
    int needed = std::vsnprintf(nullptr, 0, fmt, ap);
    va_end(ap);
    if (needed < 0) {
        va_end(ap_copy);
        return nullptr;
    }
    char* buf = static_cast<char*>(MemoryContextAlloc(ctx, static_cast<Size>(needed) + 1));
    std::vsnprintf(buf, static_cast<Size>(needed) + 1, fmt, ap_copy);
    va_end(ap_copy);
    return buf;
}

// ── GetMemoryChunkContext / GetMemoryChunkSpace ──
//   两种 MC 类型的 chunk 都用统一约定: chunk 头 8B 在 user_ptr - 8, 低 8 位 = method id
//   - AllocSet (id=1): chunk_start + 0..8 = ctx 指针, +8..16 = hdr; ctx 在 user_ptr - 16
//   - Bump     (id=2): chunk 头 (user_ptr - 8) 高 56 位 = ctx 指针; 低 8 位 = method id
MemoryContext GetMemoryChunkContext(void* pointer) {
    if (pointer == nullptr) return nullptr;
    uint64_t hdr = *(static_cast<uint64_t*>(pointer) - 1);  // 总在 user_ptr - 8
    uint64_t method_id = hdr & 0xFFULL;
    if (method_id == kMCTypeAllocSet) {
        // ctx 指针在 user_ptr - 16
        void* slot = static_cast<char*>(pointer) - 16;
        return *reinterpret_cast<MemoryContext*>(slot);
    } else if (method_id == kMCTypeBump) {
        // ctx 指针 = hdr >> 8 (高 56 位)
        return reinterpret_cast<MemoryContext>(static_cast<uintptr_t>(hdr >> 8));
    }
    return nullptr;
}

Size GetMemoryChunkSpace(void* pointer) {
    if (pointer == nullptr) return 0;
    MemoryContext ctx = GetMemoryChunkContext(pointer);
    if (ctx == nullptr) return 0;
    return ctx->methods->get_chunk_space(pointer);
}

// ── AllocSizeIsValid (PG 宏) ──
bool AllocSizeIsValid(Size size) {
    return size <= kMaxAllocSize;
}

}  // namespace jiamiao
