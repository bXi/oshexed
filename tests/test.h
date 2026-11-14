#pragma once

// Small test harness with no dependencies, so the tests build wherever core builds.

#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace test {

struct Case {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

inline int& failures() {
    static int count = 0;
    return count;
}

struct Register {
    Register(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

struct Abort {};

inline void fail(const char* file, int line, const std::string& message) {
    std::printf("  %s:%d: %s\n", file, line, message.c_str());
    ++failures();
}

template <typename A, typename B>
std::string describe(const A& a, const B& b) {
    std::ostringstream s;
    s << "expected " << b << ", got " << a;
    return s.str();
}

} // namespace test

#define TEST_CAT2(a, b) a##b
#define TEST_CAT(a, b) TEST_CAT2(a, b)
#define TEST(name)                                                                            \
    static void TEST_CAT(test_fn_, __LINE__)();                                               \
    static test::Register TEST_CAT(test_reg_, __LINE__)(name, TEST_CAT(test_fn_, __LINE__)); \
    static void TEST_CAT(test_fn_, __LINE__)()

#define CHECK(expr) \
    do { if (!(expr)) test::fail(__FILE__, __LINE__, "CHECK(" #expr ") failed"); } while (0)

#define CHECK_EQ(actual, expected)                                                              \
    do {                                                                                        \
        auto&& a_ = (actual);                                                                   \
        auto&& e_ = (expected);                                                                 \
        if (!(a_ == e_)) test::fail(__FILE__, __LINE__, #actual ": " + test::describe(a_, e_)); \
    } while (0)

// Like CHECK, but stops the current test on failure.
#define REQUIRE(expr)                                                                                       \
    do {                                                                                                    \
        if (!(expr)) { test::fail(__FILE__, __LINE__, "REQUIRE(" #expr ") failed"); throw test::Abort{}; } \
    } while (0)
