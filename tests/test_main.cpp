#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "common/memcontext.h"

// 在所有 test 之前初始化 MemoryContext (WAL encode 等已用 jmalloc)
// 用全局对象构造: C++ 保证 thread-safe init 早于 main(), 早于任何 test.
namespace {
struct McInit {
    McInit() { jiamiao::MemoryContextInit(); }
};
const McInit kMcInit;
}
