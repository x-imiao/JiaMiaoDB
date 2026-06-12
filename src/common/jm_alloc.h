/* ═══════════════════════════════════════════════════════════════════════
   jm_alloc.h — JMAlloc<T> std::allocator + ScopeContext RAII

   包装 std::allocator 协议, 把 STL 容器的分配路由到 jmalloc/jmfree.
   通过 thread_local CurrentMemoryContext 工作 — 调用方用 ScopeContext 切.

   ScopeContext: RAII 切到子 context, 出 scope 时可选 reset 并切回.
   典型用法:
       {
           ScopeContext s(my_op_ctx, true);  // reset_on_destroy=true
           std::vector<Row, JMAlloc<Row>> rows;     // 走 my_op_ctx
           rows.push_back(...);
       }  // ← 出 scope 时 my_op_ctx reset, 然后切回原 current

   注意: JMAlloc<T> 不调用构造函数 (STL 自己做), 只管 raw bytes.
   析构函数不由 MC 触发 (走 STL 的元素析构, 与 system operator delete 一致).
   ═══════════════════════════════════════════════════════════════════════ */

#ifndef JIAMIAODB_COMMON_JM_ALLOC_H
#define JIAMIAODB_COMMON_JM_ALLOC_H

#include <cstddef>
#include <type_traits>

#include "common/memcontext.h"

namespace jiamiao {

// ── JMAlloc<T>: std::allocator-compatible 适配器 ──
template <typename T>
class JMAlloc {
public:
    using value_type                             = T;
    using size_type                              = std::size_t;
    using difference_type                        = std::ptrdiff_t;
    using propagate_on_container_move_assignment = std::true_type;
    using propagate_on_container_copy_assignment = std::false_type;
    using propagate_on_container_swap            = std::true_type;
    using is_always_equal                        = std::true_type;

    constexpr JMAlloc() noexcept = default;

    template <typename U>
    constexpr JMAlloc(const JMAlloc<U>&) noexcept {}

    T* allocate(std::size_t n) {
        if (n > (std::size_t(-1) / sizeof(T))) {
            throw std::bad_alloc();
        }
        // 用 jmalloc 走 EffectiveCurrent 兜底 (其它线程的 thread_local 可能 nullptr)
        return static_cast<T*>(jmalloc(n * sizeof(T)));
    }

    T* allocate(std::size_t n, std::size_t align) {
        if (n > (std::size_t(-1) / sizeof(T))) {
            throw std::bad_alloc();
        }
        return static_cast<T*>(jmalloc_aligned(n * sizeof(T), align));
    }

    void deallocate(T* p, std::size_t) noexcept {
        // MC 不支持 deallocate: reset 整 context 才能回收. 这里用 jmfree 走 freelist
        // (AllocSet 的 free 走 freelist, Bump 的 free 是 no-op, 都安全).
        MemoryContextFree(p);
    }
};

template <typename T, typename U>
bool operator==(const JMAlloc<T>&, const JMAlloc<U>&) noexcept { return true; }

template <typename T, typename U>
bool operator!=(const JMAlloc<T>&, const JMAlloc<U>&) noexcept { return false; }

// ── ScopeContext RAII ──
//   构造: 切到 ctx, 保存原 current
//   析构: 若 reset_on_destroy, 调 MemoryContextReset(ctx); 然后切回原 current
class ScopeContext {
public:
    explicit ScopeContext(MemoryContext ctx, bool reset_on_destroy = false)
        : saved_(MemoryContextSwitchTo(ctx)),
          ctx_(ctx),
          reset_(reset_on_destroy) {}

    ~ScopeContext() {
        if (reset_ && ctx_ != nullptr) {
            MemoryContextReset(ctx_);
        }
        CurrentMemoryContext = saved_;
    }

    ScopeContext(const ScopeContext&)            = delete;
    ScopeContext& operator=(const ScopeContext&) = delete;
    ScopeContext(ScopeContext&&)                 = delete;
    ScopeContext& operator=(ScopeContext&&)      = delete;

    MemoryContext saved() const { return saved_; }
    MemoryContext ctx()   const { return ctx_; }

private:
    MemoryContext saved_;
    MemoryContext ctx_;
    bool          reset_;
};

// ── ScopeChildContext RAII ──
//   在构造时自动创建 child context (父 = current), 析构时删除 child 并切回.
//   用于"per-call scratch"模式.
class ScopeChildContext {
public:
    explicit ScopeChildContext(const char* name,
                                Size init = kAllocSetSmallInitSize,
                                Size max  = kAllocSetDefaultMaxSize)
        : parent_(CurrentMemoryContext) {
        child_ = JMAllocSetContextCreate(parent_, name,
                                           kAllocSetSmallMinSize, init, max);
        saved_ = MemoryContextSwitchTo(child_);
    }

    ~ScopeChildContext() {
        // 切回父 (避免 child 删除时当前是 child 自己, 触发未定义行为)
        CurrentMemoryContext = parent_;
        MemoryContextDelete(child_);
    }

    ScopeChildContext(const ScopeChildContext&)            = delete;
    ScopeChildContext& operator=(const ScopeChildContext&) = delete;

    MemoryContext get() const { return child_; }

private:
    MemoryContext parent_;
    MemoryContext child_;
    MemoryContext saved_;
};

}  // namespace jiamiao

#endif  // JIAMIAODB_COMMON_JM_ALLOC_H
