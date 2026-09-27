#include "memory.h"
#include <cstdlib>

namespace unify {
std::atomic<uint64_t> HeapStats::allocations{0};
std::atomic<uint64_t> HeapStats::bytes{0};
}  // namespace unify

#ifndef UNIFY_NO_GLOBAL_NEW
// Global allocation counting. Cheap (one relaxed atomic add) and always on, so the
// "no per-tick heap allocation" property can be measured in any build.
void* operator new(size_t size) {
    unify::HeapStats::allocations.fetch_add(1, std::memory_order_relaxed);
    unify::HeapStats::bytes.fetch_add(size, std::memory_order_relaxed);
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](size_t size) { return operator new(size); }
void* operator new(size_t size, const std::nothrow_t&) noexcept {
    unify::HeapStats::allocations.fetch_add(1, std::memory_order_relaxed);
    unify::HeapStats::bytes.fetch_add(size, std::memory_order_relaxed);
    return std::malloc(size ? size : 1);
}
void* operator new[](size_t size, const std::nothrow_t& t) noexcept { return operator new(size, t); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }
#endif  // UNIFY_NO_GLOBAL_NEW (hosts such as Unreal own operator new; counting is then disabled)
