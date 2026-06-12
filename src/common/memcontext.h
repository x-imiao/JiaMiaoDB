/* ═══════════════════════════════════════════════════════════════════════
   memcontext.h — Phase 3: PostgreSQL 风格 Memory Context (jmalloc/jmfree)

   把 PG 的 memnodes.h + palloc.h + memutils.h 移植到 C++17.

   核心概念:
   - MemoryContext: 树形结构的内存分配上下文, 每次分配在某个 context 中.
   - Reset/Delete: 一次性回收整个子树的所有分配, O(1) 不遍历每个对象.
   - jmalloc/jmfree: 顶层 API, 分配到 CurrentMemoryContext (thread_local).
   - AllocSet / Bump: 两种 context 实现, 都在 allocset.{h,cpp} / bump.{h,cpp}.

   重命名对照 (用户指定):
     palloc         → jmalloc          (palloc.h)
     palloc0        → jmalloc0
     palloc_extended→ jmalloc_extended
     palloc_aligned → jmalloc_aligned
     repalloc       → jrealloc
     pfree          → jmfree
     pstrdup        → jmstrdup
     pnstrdup       → jmstrndup
     psprintf       → jmsprintf
   保留 (PG 风格):
     MemoryContext 类型名
     CurrentMemoryContext  / TopMemoryContext / ErrorContext
     AllocSetContextCreate (改名 JMAllocSetContextCreate)
     MemoryContextReset / MemoryContextDelete / MemoryContextSwitchTo
   ═══════════════════════════════════════════════════════════════════════ */

#ifndef JIAMIAODB_COMMON_MEMCONTEXT_H
#define JIAMIAODB_COMMON_MEMCONTEXT_H

#include <cstddef>
#include <cstdint>

namespace jiamiao {

// ── 基础类型 ──
using Size = size_t;

// 分配大小上限 (PG MaxAllocSize, 1GB-1, 对应 varlena 上限)
constexpr Size kMaxAllocSize     = 0x3FFFFFFF;
constexpr Size kMaxAllocHugeSize = SIZE_MAX / 2;

// ── AllocSet 块参数 (PG 同名宏) ──
constexpr Size kAllocMinBits           = 3;                                    // 最小 chunk 8B
constexpr Size kAllocSetNumFreelists   = 11;                                   // 11 个 freelist
constexpr Size kAllocChunkLimit        = 1 << (kAllocSetNumFreelists - 1 + kAllocMinBits);  // 8KB
constexpr Size kAllocChunkFraction     = 4;                                    // chunk 不超过 max_block_size / 4

// AllocSet default/small size presets (PG memutils.h)
constexpr Size kAllocSetDefaultMinSize  = 0;
constexpr Size kAllocSetDefaultInitSize = 8 * 1024;
constexpr Size kAllocSetDefaultMaxSize  = 8 * 1024 * 1024;
constexpr Size kAllocSetSmallMinSize    = 0;
constexpr Size kAllocSetSmallInitSize   = 1 * 1024;
constexpr Size kAllocSetSmallMaxSize    = 8 * 1024;

// Threshold above which an AllocSet request is allocated separately
// (PG ALLOCSET_SEPARATE_THRESHOLD)
constexpr Size kAllocSetSeparateThreshold = 8 * 1024;

// ── 分配 flags (PG MCXT_ALLOC_*) ──
constexpr int kMCXT_ALLOC_HUGE   = 0x01;  // > 1GB
constexpr int kMCXT_ALLOC_NO_OOM = 0x02;  // JMDB 不实现, 一律 throw
constexpr int kMCXT_ALLOC_ZERO   = 0x04;

// ── context 类型 tag ──
enum MCTypeTag {
    kMCTypeUnknown   = 0,
    kMCTypeAllocSet  = 1,
    kMCTypeBump      = 2,
};

// ── 回调函数 ──
struct MemoryContextData;
using MemoryContext = MemoryContextData*;

struct MemoryContextCallback {
    using Func = void (*)(void* arg);
    Func                          func = nullptr;
    void*                         arg  = nullptr;
    MemoryContextCallback*        next = nullptr;
};

// ── 虚函数表 ──
struct MemoryContextMethods {
    void*          (*alloc)           (MemoryContext ctx, Size size, int flags);
    void           (*free_p)          (void* pointer);
    void*          (*realloc)         (void* pointer, Size size, int flags);
    void           (*reset)           (MemoryContext ctx);
    void           (*delete_context)  (MemoryContext ctx);
    MemoryContext  (*get_chunk_context)(void* pointer);
    Size           (*get_chunk_space) (void* pointer);
    bool           (*is_empty)        (MemoryContext ctx);
};

// ── MemoryContextData ──
struct MemoryContextData {
    int                          type_tag              = kMCTypeUnknown;
    bool                         is_reset              = true;   // 自上次 reset 以来无分配
    bool                         allow_in_crit_section = false;
    Size                         mem_allocated         = 0;
    const MemoryContextMethods*  methods               = nullptr;
    MemoryContext                parent                = nullptr;
    MemoryContext                firstchild            = nullptr;
    MemoryContext                prevchild             = nullptr;
    MemoryContext                nextchild             = nullptr;
    const char*                  name                  = "";
    const char*                  ident                 = nullptr;
    MemoryContextCallback*       reset_cbs             = nullptr;
};

// ── 全局 context ──
extern MemoryContext TopMemoryContext;       // 进程级, 从不释放
extern MemoryContext ErrorContext;          // 错误恢复用
extern thread_local MemoryContext CurrentMemoryContext;  // 每个线程一个

// ── 上下文切换 (内联, 关键路径) ──
inline MemoryContext MemoryContextSwitchTo(MemoryContext ctx) {
    MemoryContext old = CurrentMemoryContext;
    CurrentMemoryContext = ctx;
    return old;
}

// ── 顶层 API ──
void  MemoryContextInit();
void  MemoryContextReset(MemoryContext ctx);
void  MemoryContextResetChildren(MemoryContext ctx);
void  MemoryContextDelete(MemoryContext ctx);
void  MemoryContextDeleteChildren(MemoryContext ctx);
void  MemoryContextSetParent(MemoryContext ctx, MemoryContext parent);
void  MemoryContextSetIdentifier(MemoryContext ctx, const char* ident);
void  MemoryContextRegisterResetCallback(MemoryContext ctx, MemoryContextCallback* cb);

MemoryContext GetMemoryChunkContext(void* pointer);
Size          GetMemoryChunkSpace(void* pointer);
MemoryContext MemoryContextGetParent(MemoryContext ctx);
bool          MemoryContextIsEmpty(MemoryContext ctx);
Size          MemoryContextMemAllocated(MemoryContext ctx, bool recurse);

// 直接 context API (不走 CurrentMemoryContext)
void*  MemoryContextAlloc(MemoryContext ctx, Size size);
void*  MemoryContextAllocZero(MemoryContext ctx, Size size);
void*  MemoryContextAllocExtended(MemoryContext ctx, Size size, int flags);
void*  MemoryContextAllocAligned(MemoryContext ctx, Size size, Size align, int flags);
void*  MemoryContextRealloc(void* pointer, Size size);
void   MemoryContextFree(void* pointer);
char*  MemoryContextStrdup(MemoryContext ctx, const char* s);
char*  MemoryContextStrndup(MemoryContext ctx, const char* s, Size len);
char*  MemoryContextSprintf(MemoryContext ctx, const char* fmt, ...);

// jmalloc family — 走 CurrentMemoryContext
void*  jmalloc(Size size);
void*  jmalloc0(Size size);
void*  jmalloc_aligned(Size size, Size align);
void*  jmalloc_extended(Size size, int flags);
void*  jrealloc(void* pointer, Size size);
void*  jrealloc0(void* pointer, Size old_size, Size new_size);
void*  jrealloc_array(void* pointer, Size old_count, Size new_count, Size elem_size);
void   jmfree(void* pointer);
char*  jmstrdup(const char* s);
char*  jmstrndup(const char* s, Size len);
char*  jmsprintf(const char* fmt, ...);

// ── AllocSet + Bump 构造函数 (实现分别在 allocset.cpp / bump.cpp) ──
MemoryContext JMAllocSetContextCreate(MemoryContext parent, const char* name,
                                      Size min_size, Size init_size, Size max_size);
MemoryContext JMBumpContextCreate(MemoryContext parent, const char* name,
                                  Size init_size, Size max_size);

}  // namespace jiamiao

#endif  // JIAMIAODB_COMMON_MEMCONTEXT_H
