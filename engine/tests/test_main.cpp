#include <chrono>
#include <cstring>

#include "mf/common.hpp"
#include "mf/text.hpp"
#include "mftest.hpp"

namespace mftest {
std::vector<Case>& registry() {
    static std::vector<Case> r;
    return r;
}
int g_failures = 0;
const char* g_current = "";
std::string sourceRoot() { return MF_SOURCE_ROOT; }
std::string tmpDir() {
    std::string d = sourceRoot() + "/build-host/test-tmp";
    mf::makeDirs(d);
    return d;
}
}  // namespace mftest

int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    mf::setLogLevel(mf::LogLevel::Error);
    mf::FontManager::instance().registerFile(mftest::sourceRoot() + "/assets/fonts/DejaVuSans.ttf", "DejaVuSans");
    mf::FontManager::instance().registerFile(mftest::sourceRoot() + "/assets/fonts/DejaVuSans-Bold.ttf", "DejaVuSans-Bold");
    int run = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (auto& c : mftest::registry()) {
        if (filter && !std::strstr(c.name, filter)) continue;
        mftest::g_current = c.name;
        int before = mftest::g_failures;
        auto s = std::chrono::steady_clock::now();
        try {
            c.fn();
        } catch (std::exception& e) {
            std::printf("  FAIL [%s] exception: %s\n", c.name, e.what());
            ++mftest::g_failures;
        }
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s).count();
        std::printf("%s %-48s %8.1f ms\n", mftest::g_failures == before ? "PASS" : "FAIL", c.name, ms);
        ++run;
    }
    double total = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("\n%d tests, %d failures, %.1fs\n", run, mftest::g_failures, total);
    return mftest::g_failures == 0 ? 0 : 1;
}
