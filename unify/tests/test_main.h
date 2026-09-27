#pragma once
// Minimal test harness: TEST(name) registers a function; CHECK records failures with location.
#include <cstdio>
#include <cmath>
#include <functional>
#include <string>
#include <vector>

namespace unify_test {
struct Case { const char* name; std::function<void()> fn; };
inline std::vector<Case>& registry() { static std::vector<Case> r; return r; }
inline int& failures() { static int f = 0; return f; }
inline int& checks() { static int c = 0; return c; }
struct Reg { Reg(const char* n, std::function<void()> f) { registry().push_back({n, std::move(f)}); } };
}  // namespace unify_test

#define TEST(name) \
    static void test_##name(); \
    static unify_test::Reg reg_##name(#name, test_##name); \
    static void test_##name()

#define CHECK(cond) do { unify_test::checks()++; if (!(cond)) { unify_test::failures()++; \
    std::printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_NEAR(a, b, eps) do { unify_test::checks()++; double _a = (a), _b = (b); if (!(std::fabs(_a - _b) <= (eps))) { \
    unify_test::failures()++; std::printf("    FAIL %s:%d: %s = %g, expected %g (±%g)\n", __FILE__, __LINE__, #a, _a, _b, double(eps)); } } while (0)
