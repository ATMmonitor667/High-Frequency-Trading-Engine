#include <atomic>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>

#include "../../src/storage/tick_store.h"

// =============================================================================
// Standalone unit tests for TickStore (no external test framework).
//
// Build & run (one line):
//   g++ -std=c++17 -pthread tests/unit/test_tick_store.cpp
//       src/storage/tick_store.cpp -o test_ts && ./test_ts
//
// Exit code is 0 only if every check passes (CI-friendly).
// (Mirrors the harness used by test_risk_engine.cpp.)
//
// Each test gets its own scratch directory under the system temp dir; it is
// removed on entry and exit so runs are isolated and never touch the repo.
// =============================================================================

namespace {

int g_checks = 0;
int g_failures = 0;
const char* g_current_test = "";

void report_fail(const std::string& msg, int line) {
    ++g_failures;
    std::cout << "  [FAIL] (" << g_current_test << ":" << line << ") " << msg << '\n';
}

void check_true(bool cond, const std::string& msg, int line) {
    ++g_checks;
    if (!cond) report_fail(msg, line);
}

#define CHECK(cond) check_true((cond), #cond, __LINE__)

#define RUN_TEST(fn)                                   \
    do {                                               \
        g_current_test = #fn;                          \
        int before = g_failures;                       \
        fn();                                          \
        if (g_failures == before)                      \
            std::cout << "[ OK ] " << #fn << '\n';      \
        else                                           \
            std::cout << "[FAIL] " << #fn << '\n';      \
    } while (0)

using namespace hft;

bool approx(double a, double b, double eps = 1e-9) {
    return std::fabs(a - b) <= eps * (1.0 + std::fabs(b));
}

// A unique scratch directory under the system temp dir, wiped on entry/exit so
// tests are isolated and leave nothing behind in the source tree.
struct ScratchDir {
    std::filesystem::path path;
    explicit ScratchDir(const char* name) {
        static std::atomic<int> counter{0};
        path = std::filesystem::temp_directory_path() /
               (std::string("tickstore_") + name + "_" + std::to_string(counter++));
        std::filesystem::remove_all(path);
    }
    ~ScratchDir() { std::filesystem::remove_all(path); }
    std::string str() const { return path.string(); }
};

Tick tick(int64_t ts, double px, long qty) {
    return Tick{ts, px, qty};
}

// -----------------------------------------------------------------------------
// Append then query the full range returns everything in timestamp order.
// -----------------------------------------------------------------------------
void test_append_and_query() {
    ScratchDir d("append");
    TickStore store(d.str());

    store.append("AAPL", tick(100, 10.0, 1));
    store.append("AAPL", tick(200, 11.0, 2));
    store.append("AAPL", tick(300, 12.0, 3));

    auto ticks = store.query("AAPL", 0, 1000);
    CHECK(ticks.size() == 3);
    CHECK(ticks[0].tsNs == 100 && approx(ticks[0].price, 10.0) && ticks[0].qty == 1);
    CHECK(ticks[1].tsNs == 200);
    CHECK(ticks[2].tsNs == 300);
}

// -----------------------------------------------------------------------------
// query is half-open [fromNs, toNs): the upper bound is exclusive.
// -----------------------------------------------------------------------------
void test_half_open_range() {
    ScratchDir d("halfopen");
    TickStore store(d.str());

    for (int64_t ts = 100; ts <= 500; ts += 100)
        store.append("X", tick(ts, 1.0, 1));

    // [200, 400) -> ts 200 and 300 only (400 excluded, 100 below).
    auto ticks = store.query("X", 200, 400);
    CHECK(ticks.size() == 2);
    CHECK(ticks.front().tsNs == 200);
    CHECK(ticks.back().tsNs == 300);

    // Exact-bound query where from == a tick and to == same tick -> empty.
    CHECK(store.query("X", 300, 300).empty());
}

// -----------------------------------------------------------------------------
// Out-of-range / unknown-symbol queries return empty, not garbage.
// -----------------------------------------------------------------------------
void test_empty_queries() {
    ScratchDir d("empty");
    TickStore store(d.str());

    store.append("X", tick(100, 1.0, 1));

    CHECK(store.query("X", 500, 600).empty());   // entirely after data
    CHECK(store.query("X", 0, 50).empty());       // entirely before data
    CHECK(store.query("UNKNOWN", 0, 1000).empty());
}

// -----------------------------------------------------------------------------
// Ticks appended out of order are sorted on flush so query stays correct.
// -----------------------------------------------------------------------------
void test_out_of_order_sorted() {
    ScratchDir d("ooo");
    TickStore store(d.str());

    store.append("X", tick(300, 3.0, 1));
    store.append("X", tick(100, 1.0, 1));
    store.append("X", tick(200, 2.0, 1));

    auto ticks = store.query("X", 0, 1000);
    CHECK(ticks.size() == 3);
    CHECK(ticks[0].tsNs == 100);
    CHECK(ticks[1].tsNs == 200);
    CHECK(ticks[2].tsNs == 300);
}

// -----------------------------------------------------------------------------
// query() flushes buffered data first, so an explicit flush isn't required to
// see freshly appended ticks.
// -----------------------------------------------------------------------------
void test_query_flushes_buffer() {
    ScratchDir d("qflush");
    TickStore store(d.str());

    store.append("X", tick(100, 1.0, 5));   // buffered, not explicitly flushed
    auto ticks = store.query("X", 0, 1000);
    CHECK(ticks.size() == 1);
    CHECK(ticks[0].qty == 5);
}

// -----------------------------------------------------------------------------
// Multiple flushes merge with existing on-disk data rather than overwriting.
// -----------------------------------------------------------------------------
void test_incremental_flush_merge() {
    ScratchDir d("merge");
    TickStore store(d.str());

    store.append("X", tick(100, 1.0, 1));
    store.flush("X");
    store.append("X", tick(200, 2.0, 1));
    store.flush("X");
    store.append("X", tick(300, 3.0, 1));

    auto ticks = store.query("X", 0, 1000);
    CHECK(ticks.size() == 3);
    CHECK(ticks[0].tsNs == 100 && ticks[2].tsNs == 300);
}

// -----------------------------------------------------------------------------
// Distinct symbols are stored independently.
// -----------------------------------------------------------------------------
void test_multi_symbol_isolation() {
    ScratchDir d("multi");
    TickStore store(d.str());

    store.append("AAA", tick(100, 1.0, 1));
    store.append("BBB", tick(100, 9.0, 7));

    auto a = store.query("AAA", 0, 1000);
    auto b = store.query("BBB", 0, 1000);
    CHECK(a.size() == 1 && approx(a[0].price, 1.0));
    CHECK(b.size() == 1 && approx(b[0].price, 9.0) && b[0].qty == 7);
}

// -----------------------------------------------------------------------------
// Persistence: data flushed by one store instance is readable by a fresh
// instance pointed at the same directory (simulates restart).
// -----------------------------------------------------------------------------
void test_persistence_across_reopen() {
    ScratchDir d("persist");

    {
        TickStore store(d.str());
        store.append("X", tick(100, 1.0, 1));
        store.append("X", tick(200, 2.0, 2));
        store.flush("X");
    }  // store destroyed; file remains on disk

    TickStore reopened(d.str());
    auto ticks = reopened.query("X", 0, 1000);
    CHECK(ticks.size() == 2);
    CHECK(ticks[0].tsNs == 100);
    CHECK(ticks[1].tsNs == 200 && ticks[1].qty == 2);

    // Appending after reopen continues the same segment.
    reopened.append("X", tick(300, 3.0, 3));
    CHECK(reopened.query("X", 0, 1000).size() == 3);
}

// -----------------------------------------------------------------------------
// resample aggregates a range into one OHLCV bar.
//   prices 10, 12, 9, 11 (in ts order) -> O=10 H=12 L=9 C=11, vol = sum(qty)
// -----------------------------------------------------------------------------
void test_resample_ohlcv() {
    ScratchDir d("ohlcv");
    TickStore store(d.str());

    store.append("X", tick(100, 10.0, 1));
    store.append("X", tick(200, 12.0, 2));
    store.append("X", tick(300,  9.0, 3));
    store.append("X", tick(400, 11.0, 4));

    OHLCVBar bar = store.resample("X", 0, 1000, 1000);
    CHECK(bar.tsNs == 0);            // bar stamped with fromNs
    CHECK(approx(bar.open, 10.0));
    CHECK(approx(bar.high, 12.0));
    CHECK(approx(bar.low, 9.0));
    CHECK(approx(bar.close, 11.0));  // last tick in range
    CHECK(bar.volume == 10);         // 1+2+3+4
}

// -----------------------------------------------------------------------------
// resample over an empty range returns a zeroed bar stamped with fromNs.
// -----------------------------------------------------------------------------
void test_resample_empty() {
    ScratchDir d("ohlcv_empty");
    TickStore store(d.str());

    store.append("X", tick(100, 10.0, 1));

    OHLCVBar bar = store.resample("X", 500, 600, 100);
    CHECK(bar.tsNs == 500);
    CHECK(approx(bar.open, 0.0));
    CHECK(approx(bar.close, 0.0));
    CHECK(bar.volume == 0);
}

}  // namespace

int main() {
    std::cout << "Running TickStore tests...\n";

    RUN_TEST(test_append_and_query);
    RUN_TEST(test_half_open_range);
    RUN_TEST(test_empty_queries);
    RUN_TEST(test_out_of_order_sorted);
    RUN_TEST(test_query_flushes_buffer);
    RUN_TEST(test_incremental_flush_merge);
    RUN_TEST(test_multi_symbol_isolation);
    RUN_TEST(test_persistence_across_reopen);
    RUN_TEST(test_resample_ohlcv);
    RUN_TEST(test_resample_empty);

    std::cout << "\n" << (g_checks - g_failures) << "/" << g_checks
              << " checks passed.\n";
    if (g_failures == 0) {
        std::cout << "ALL TESTS PASSED\n";
        return 0;
    }
    std::cout << g_failures << " CHECK(S) FAILED\n";
    return 1;
}
