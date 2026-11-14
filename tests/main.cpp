#include "test.h"

#include <chrono>
#include <cstring>
#include <exception>

// Usage: core_tests [substring]  runs only the tests whose name contains the substring.
int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int run = 0, failed = 0;
    for (auto& c : test::registry()) {
        if (filter && !std::strstr(c.name, filter)) continue;
        const int before = test::failures();
        const auto t0 = std::chrono::steady_clock::now();
        try {
            c.fn();
        } catch (const test::Abort&) {
        } catch (const std::exception& e) {
            test::fail(__FILE__, __LINE__, std::string("exception: ") + e.what());
        }
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        ++run;
        const bool ok = test::failures() == before;
        if (!ok) ++failed;
        std::printf("%s %s (%lld ms)\n", ok ? "[ ok ]" : "[FAIL]", c.name, (long long)ms);
    }
    std::printf("\n%d/%d passed\n", run - failed, run);
    return failed == 0 ? 0 : 1;
}
