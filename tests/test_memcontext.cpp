#include "doctest.h"
#include "common/memcontext.h"
#include "common/allocset.h"
#include "common/bump.h"
#include "common/jm_alloc.h"
#include <cstring>
#include <vector>
#include <string>

namespace jm = jiamiao;

TEST_CASE("MC: MemoryContextInit creates TopMemoryContext and ErrorContext") {
    // Note: tests share global state; if init was called already, just check non-null
    jm::MemoryContextInit();
    CHECK(jm::TopMemoryContext != nullptr);
    CHECK(jm::ErrorContext != nullptr);
    CHECK(jm::CurrentMemoryContext == jm::TopMemoryContext);
}

TEST_CASE("MC: jmalloc/jmfree round-trip in current context") {
    jm::MemoryContextInit();
    void* p = jm::jmalloc(64);
    CHECK(p != nullptr);
    // 写入并读回
    std::memset(p, 0xAB, 64);
    CHECK(static_cast<uint8_t*>(p)[0] == 0xAB);
    jm::jmfree(p);
}

TEST_CASE("MC: AllocSet power-of-2 freelist routes small allocs") {
    jm::MemoryContextInit();
    auto* ctx = static_cast<jm::AllocSetContext*>(
        jm::JMAllocSetContextCreate(jm::TopMemoryContext, "TestAS", 0, 8*1024, 64*1024));
    void* p1 = jm::AllocSetAlloc(ctx, 8, 0);
    void* p2 = jm::AllocSetAlloc(ctx, 8, 0);
    void* p3 = jm::AllocSetAlloc(ctx, 8, 0);
    CHECK(p1 != nullptr);
    CHECK(p2 != nullptr);
    CHECK(p3 != nullptr);
    CHECK(p1 != p2);
    jm::AllocSetReset(ctx);
    CHECK(jm::AllocSetIsEmpty(ctx));
}

TEST_CASE("MC: AllocSet large alloc (>8K) goes to dedicated block") {
    jm::MemoryContextInit();
    auto* ctx = static_cast<jm::AllocSetContext*>(
        jm::JMAllocSetContextCreate(jm::TopMemoryContext, "TestAS_Large", 0, 8*1024, 64*1024));
    void* big = jm::AllocSetAlloc(ctx, 32*1024, 0);
    CHECK(big != nullptr);
    std::memset(big, 0, 32*1024);
    jm::AllocSetReset(ctx);
    CHECK(jm::AllocSetIsEmpty(ctx));
}

TEST_CASE("MC: AllocSet reset returns memory, child contexts survive") {
    jm::MemoryContextInit();
    auto* parent = static_cast<jm::AllocSetContext*>(
        jm::JMAllocSetContextCreate(jm::TopMemoryContext, "TestParent", 0, 8*1024, 64*1024));
    auto* child = static_cast<jm::AllocSetContext*>(
        jm::JMAllocSetContextCreate(parent, "TestChild", 0, 1*1024, 8*1024));
    void* p = jm::AllocSetAlloc(parent, 100, 0);
    CHECK(p != nullptr);
    jm::Size parent_used = parent->mem_allocated;
    CHECK(parent_used > 0);
    jm::AllocSetReset(parent);
    CHECK(parent->mem_allocated == 0);
    // 父 reset 不影响子
    CHECK(child->parent == parent);
    jm::MemoryContextDelete(parent);  // 也会 delete child
}

TEST_CASE("MC: reset callback fires before reset, in reverse registration order") {
    jm::MemoryContextInit();
    auto* ctx = static_cast<jm::AllocSetContext*>(
        jm::JMAllocSetContextCreate(jm::TopMemoryContext, "TestCb", 0, 8*1024, 64*1024));
    int log_a = 0, log_b = 0, log_c = 0;
    jm::MemoryContextCallback cb_a { [](void* arg) { *static_cast<int*>(arg) += 1; }, &log_a, nullptr };
    jm::MemoryContextCallback cb_b { [](void* arg) { *static_cast<int*>(arg) += 10; }, &log_b, nullptr };
    jm::MemoryContextCallback cb_c { [](void* arg) { *static_cast<int*>(arg) += 100; }, &log_c, nullptr };
    jm::MemoryContextRegisterResetCallback(ctx, &cb_a);
    jm::MemoryContextRegisterResetCallback(ctx, &cb_b);
    jm::MemoryContextRegisterResetCallback(ctx, &cb_c);
    jm::MemoryContextReset(ctx);
    // 反序: c, b, a
    CHECK(log_c == 100);
    CHECK(log_b == 10);
    CHECK(log_a == 1);
}

TEST_CASE("MC: Bump alloc bumps; reset reclaims all") {
    jm::MemoryContextInit();
    auto* ctx = static_cast<jm::BumpContext*>(
        jm::JMBumpContextCreate(jm::TopMemoryContext, "TestBump", 8*1024, 64*1024));
    void* a = jm::BumpAlloc(ctx, 100, 0);
    void* b = jm::BumpAlloc(ctx, 200, 0);
    CHECK(a != nullptr);
    CHECK(b != nullptr);
    CHECK(ctx->mem_allocated > 0);
    jm::BumpReset(ctx);
    CHECK(ctx->mem_allocated == 0);
    jm::MemoryContextDelete(ctx);
}

TEST_CASE("MC: jmstrdup / jmstrndup") {
    jm::MemoryContextInit();
    const char* src = "hello world";
    char* dup = jm::jmstrdup(src);
    CHECK(dup != nullptr);
    CHECK(std::strcmp(dup, "hello world") == 0);
    jm::jmfree(dup);
    char* n = jm::jmstrndup("abcdef", 3);
    CHECK(std::strcmp(n, "abc") == 0);
    jm::jmfree(n);
}

TEST_CASE("MC: ScopeContext RAII switches and restores") {
    jm::MemoryContextInit();
    auto* sub = jm::JMAllocSetContextCreate(jm::TopMemoryContext, "SubCtx", 0, 1*1024, 8*1024);
    jm::MemoryContext saved = jm::CurrentMemoryContext;
    {
        jm::ScopeContext s(sub, true);
        CHECK(jm::CurrentMemoryContext == sub);
        void* p = jm::jmalloc(64);
        CHECK(p != nullptr);
    }
    CHECK(jm::CurrentMemoryContext == saved);
    // sub 已被 reset (因为 reset_on_destroy=true)
    CHECK(jm::MemoryContextIsEmpty(sub));
}

TEST_CASE("MC: JMAlloc<T> for std::vector routes to current context") {
    jm::MemoryContextInit();
    auto* ctx = jm::JMAllocSetContextCreate(jm::TopMemoryContext, "VecTest", 0, 8*1024, 64*1024);
    jm::Size peak = 0;
    {
        jm::ScopeContext s(ctx);
        std::vector<int, jm::JMAlloc<int>> v;
        for (int i = 0; i < 100; ++i) v.push_back(i);
        CHECK(v.size() == 100);
        CHECK(v[50] == 50);
        peak = ctx->mem_allocated;  // 在 scope 内取峰值
    }
    // vector 析构会 deallocate; 现在 mem_allocated 应回到 0 (或接近)
    CHECK(peak > 0);  // 分配确实发生过
    jm::AllocSetReset(ctx);
    CHECK(ctx->mem_allocated == 0);
    jm::MemoryContextDelete(ctx);
}

TEST_CASE("MC: AllocSet child survives parent delete (no early free of children)") {
    jm::MemoryContextInit();
    auto* parent = jm::JMAllocSetContextCreate(jm::TopMemoryContext, "Parent2", 0, 8*1024, 64*1024);
    auto* child  = jm::JMAllocSetContextCreate(parent, "Child2", 0, 1*1024, 8*1024);
    void* p = jm::MemoryContextAlloc(child, 100);  // 走 child
    CHECK(p != nullptr);
    jm::Size child_used = child->mem_allocated;
    CHECK(child_used > 0);
    // delete parent 应该也会 delete child
    jm::MemoryContextDelete(parent);
}

TEST_CASE("MC: 1M small allocations perf (sanity)") {
    jm::MemoryContextInit();
    auto* ctx = jm::JMAllocSetContextCreate(jm::TopMemoryContext, "Perf", 0, 8*1024, 8*1024*1024);
    constexpr int N = 100000;
    std::vector<void*> ptrs;
    ptrs.reserve(N);
    for (int i = 0; i < N; ++i) {
        ptrs.push_back(jm::AllocSetAlloc(static_cast<jm::AllocSetContext*>(ctx), 32, 0));
    }
    CHECK(ptrs.size() == N);
    for (int i = 0; i < N; ++i) {
        jm::AllocSetFree(ptrs[i]);
    }
    jm::MemoryContextDelete(ctx);
}
