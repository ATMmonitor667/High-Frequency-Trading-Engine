#include <cstdint>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "../../src/execution/twap_vwap/twap_vwap.h"

// =============================================================================
// Standalone unit tests for TwapAlgo / VwapAlgo (no external test framework).
//
// Build & run (MSVC):
//   cl /EHsc /std:c++17 /Fe:test_twap_vwap.exe
//       tests\unit\test_twap_vwap.cpp src\execution\twap_vwap\twap_vwap.cpp
//
// Build & run (g++ 7+):
//   g++ -std=c++17 tests/unit/test_twap_vwap.cpp
//       src/execution/twap_vwap/twap_vwap.cpp -o test_twap_vwap && ./test_twap_vwap
//
// Exit code is 0 only if every check passes.
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

template <typename A, typename B>
void check_eq(const A& a, const B& b, const std::string& msg, int line) {
    ++g_checks;
    if (!(a == b)) {
        ++g_failures;
        std::cout << "  [FAIL] (" << g_current_test << ":" << line << ") " << msg
                  << "  (got " << a << ", expected " << b << ")\n";
    }
}

#define CHECK(cond) check_true((cond), #cond, __LINE__)
#define CHECK_EQ(a, b) check_eq((a), (b), #a " == " #b, __LINE__)

#define RUN_TEST(fn)                                   \
    do {                                               \
        g_current_test = #fn;                          \
        int before = g_failures;                       \
        fn();                                          \
        if (g_failures == before)                      \
            std::cout << "[PASS] " << #fn << '\n';      \
        else                                           \
            std::cout << "[FAIL] " << #fn << '\n';      \
    } while (0)

using namespace hft;

BookUpdate makeBook(double bid, double ask) {
    BookUpdate u;
    u.symbol = "AAPL";
    u.bidPx = bid;
    u.askPx = ask;
    u.bidQty = 500;
    u.askQty = 300;
    u.tsNs = 0;
    return u;
}

// -----------------------------------------------------------------------------
// TWAP: 3 equal slices; first fire at t=0, second within [0.9, 1.1) * interval
// -----------------------------------------------------------------------------
void test_twap_three_slices() {
    TwapAlgo algo("AAPL", Side::BUY, 300, 0, 3000, 3);
    const BookUpdate book = makeBook(99.9, 100.1);

    auto o0 = algo.tick(0, book);
    // Half-interval is always before the min jitter (0.9 * baseInterval).
    auto early = algo.tick(500, book);
    // 1.1 * baseInterval is the max jitter; always enough for the next slice.
    auto o1 = algo.tick(1100, book);

    CHECK(o0.has_value());
    CHECK_EQ(o0->qty, 100L);
    CHECK_EQ(o0->px, 99.9);
    CHECK(o0->side == Side::BUY);
    CHECK(!early.has_value());
    CHECK(o1.has_value());
    CHECK_EQ(o1->qty, 100L);
    CHECK(!algo.isDone());
}

void test_twap_sell_passive_uses_ask() {
    TwapAlgo algo("AAPL", Side::SELL, 100, 0, 1000, 2);
    const BookUpdate book = makeBook(99.9, 100.1);
    auto o = algo.tick(0, book);
    CHECK(o.has_value());
    CHECK_EQ(o->px, 100.1);
    CHECK(o->side == Side::SELL);
}

// -----------------------------------------------------------------------------
// VWAP: 40% then 60%
// -----------------------------------------------------------------------------
void test_vwap_two_buckets() {
    VwapAlgo algo("AAPL", Side::BUY, 100, 0, 5000, {{0, 0.4}, {1000, 0.6}});
    const BookUpdate book = makeBook(50.0, 50.02);

    auto o0 = algo.tick(0, book);
    auto early = algo.tick(500, book);
    auto o1 = algo.tick(1000, book);

    CHECK(o0.has_value());
    CHECK_EQ(o0->qty, 40L);
    CHECK(!early.has_value());
    CHECK(o1.has_value());
    CHECK_EQ(o1->qty, 60L);
    CHECK(algo.isDone());
}

void test_vwap_last_bucket_drains_remaining() {
    VwapAlgo algo("X", Side::BUY, 10, 0, 1000,
                  {{0, 0.33}, {100, 0.33}, {200, 0.34}});
    const BookUpdate book = makeBook(10.0, 10.02);
    algo.tick(0, book);
    algo.tick(100, book);
    auto last = algo.tick(200, book);
    CHECK(last.has_value());
    CHECK_EQ(last->qty, 4L);
    CHECK(algo.isDone());
}

void test_monotonic_child_order_ids() {
    TwapAlgo algo("AAPL", Side::BUY, 200, 0, 2000, 2);
    const BookUpdate book = makeBook(1.0, 1.02);
    auto a = algo.tick(0, book);
    // Past max jitter (1.1 * 1000) so the second child always fires.
    auto b = algo.tick(1100, book);
    CHECK(a.has_value());
    CHECK(b.has_value());
    CHECK_EQ(b->clientOrderId, a->clientOrderId + 1);
}

}  // namespace

int main() {
    RUN_TEST(test_twap_three_slices);
    RUN_TEST(test_twap_sell_passive_uses_ask);
    RUN_TEST(test_vwap_two_buckets);
    RUN_TEST(test_vwap_last_bucket_drains_remaining);
    RUN_TEST(test_monotonic_child_order_ids);

    std::cout << "\n--------------------------------------------\n";
    std::cout << "Checks: " << g_checks << "   Failures: " << g_failures << '\n';
    if (g_failures == 0) {
        std::cout << "ALL TESTS PASSED\n";
        return 0;
    }
    std::cout << "SOME TESTS FAILED\n";
    return 1;
}
