#include "test_main.h"
#include <cstring>

int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int ran = 0, failed_cases = 0;
    for (auto& c : unify_test::registry()) {
        if (filter && !std::strstr(c.name, filter)) continue;
        int before = unify_test::failures();
        c.fn();
        ran++;
        bool ok = unify_test::failures() == before;
        if (!ok) failed_cases++;
        std::printf("%s %s\n", ok ? "  ok  " : "  FAIL", c.name);
    }
    std::printf("\n%d tests, %d checks, %d failed\n", ran, unify_test::checks(), failed_cases);
    return failed_cases == 0 ? 0 : 1;
}
