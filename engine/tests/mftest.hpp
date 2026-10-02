// Tiny test framework for engine host tests.
#pragma once
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace mftest {
struct Case {
    const char* name;
    std::function<void()> fn;
};
std::vector<Case>& registry();
extern int g_failures;
extern const char* g_current;
struct Reg {
    Reg(const char* n, std::function<void()> f) { registry().push_back({n, std::move(f)}); }
};
std::string sourceRoot();
std::string tmpDir();
}  // namespace mftest

#define MF_CAT2(a, b) a##b
#define MF_CAT(a, b) MF_CAT2(a, b)
#define TEST(name)                                                    \
    static void MF_CAT(test_, name)();                                \
    static mftest::Reg MF_CAT(reg_, name)(#name, MF_CAT(test_, name)); \
    static void MF_CAT(test_, name)()

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::printf("  FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, mftest::g_current, #cond);   \
            ++mftest::g_failures;                                                                  \
        }                                                                                          \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                                                          \
    do {                                                                                                               \
        double _a = (a), _b = (b);                                                                                     \
        if (!(std::fabs(_a - _b) <= (eps))) {                                                                          \
            std::printf("  FAIL %s:%d [%s] %s=%g vs %s=%g (eps %g)\n", __FILE__, __LINE__, mftest::g_current, #a, _a, #b, _b, \
                        (double)(eps));                                                                                \
            ++mftest::g_failures;                                                                                      \
        }                                                                                                              \
    } while (0)

#define REQUIRE(cond)                                                                              \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::printf("  FAIL %s:%d [%s] %s (fatal)\n", __FILE__, __LINE__, mftest::g_current, #cond); \
            ++mftest::g_failures;                                                                  \
            return;                                                                                \
        }                                                                                          \
    } while (0)
