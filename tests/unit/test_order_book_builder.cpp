#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "../../src/marketdata/book_builder/order_book_builder.h"

// =============================================================================
// Standalone unit tests for OrderBookBuilder (no external test framework).
//
// Build & run:
//   g++ -std=c++17 tests/unit/test_order_book_builder.cpp
//       src/marketdata/book_builder/order_book_builder.cpp -o test_obb && ./test_obb
//
// Exit code is 0 only if every check passes (CI-friendly).
// (Mirrors the harness used by test_limit_order_book.cpp.)
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

bool is_nan(double x) { return std::isnan(x); }

#define CHECK(cond) check_true((cond), #cond, __LINE__)
#define CHECK_EQ(a, b) check_eq((a), (b), #a " == " #b, __LINE__)
#define CHECK_NAN(x) check_true(is_nan(x), #x " should be NaN", __LINE__)

#define RUN_TEST(fn)                       \
    do {                                   \
        g_current_test = #fn;              \
        int before = g_failures;           \
        fn();                              \
        if (g_failures == before)          \
            std::cout << "[PASS] " << #fn << '\n'; \
        else                               \
            std::cout << "[FAIL] " << #fn << '\n'; \
    } while (0)

using namespace hft;

// -----------------------------------------------------------------------------
// Empty-book behaviour
// -----------------------------------------------------------------------------
void test_empty_book() {
    OrderBookBuilder book("AAPL");
    CHECK_NAN(book.getLevel(Side::BUY, 0).first);
    CHECK_NAN(book.getLevel(Side::SELL, 0).first);
    CHECK(!book.isCrossed());            // empty book is not crossed
    CHECK(!book.isStale());
    // Operations on unknown ids must be safe no-ops.
    book.onCancel(999, 10);
    book.onModify(999, 5);
    CHECK_NAN(book.getLevel(Side::BUY, 0).first);
}

// -----------------------------------------------------------------------------
// Basic add / top-of-book  (Example 1 from the spec)
// -----------------------------------------------------------------------------
void test_basic_book() {
    OrderBookBuilder book("AAPL");
    book.onAdd(1, Side::BUY, 100.0, 10);
    book.onAdd(2, Side::SELL, 101.0, 5);

    CHECK_EQ(book.getLevel(Side::BUY, 0).first, 100.0);
    CHECK_EQ(book.getLevel(Side::BUY, 0).second, 10L);
    CHECK_EQ(book.getLevel(Side::SELL, 0).first, 101.0);
    CHECK_EQ(book.getLevel(Side::SELL, 0).second, 5L);
    CHECK(!book.isCrossed());
}

void test_level_aggregation() {
    OrderBookBuilder book("AAPL");
    book.onAdd(1, Side::BUY, 100.0, 10);
    book.onAdd(2, Side::BUY, 100.0, 7);   // same level aggregates
    CHECK_EQ(book.getLevel(Side::BUY, 0).second, 17L);
}

void test_level_ordering() {
    OrderBookBuilder book("AAPL");
    book.onAdd(1, Side::BUY, 100.0, 1);
    book.onAdd(2, Side::BUY, 101.0, 1);   // higher bid is better
    book.onAdd(3, Side::BUY, 99.0, 1);
    CHECK_EQ(book.getLevel(Side::BUY, 0).first, 101.0);
    CHECK_EQ(book.getLevel(Side::BUY, 1).first, 100.0);
    CHECK_EQ(book.getLevel(Side::BUY, 2).first, 99.0);

    book.onAdd(4, Side::SELL, 105.0, 1);
    book.onAdd(5, Side::SELL, 103.0, 1);  // lower ask is better
    book.onAdd(6, Side::SELL, 104.0, 1);
    CHECK_EQ(book.getLevel(Side::SELL, 0).first, 103.0);
    CHECK_EQ(book.getLevel(Side::SELL, 1).first, 104.0);
    CHECK_EQ(book.getLevel(Side::SELL, 2).first, 105.0);
    CHECK_NAN(book.getLevel(Side::SELL, 3).first);  // out of range
}

// -----------------------------------------------------------------------------
// Cancel  (Example 2 from the spec)
// -----------------------------------------------------------------------------
void test_partial_cancel() {
    OrderBookBuilder book("AAPL");
    book.onAdd(1, Side::BUY, 100.0, 10);
    book.onCancel(1, 4);
    CHECK_EQ(book.getLevel(Side::BUY, 0).first, 100.0);
    CHECK_EQ(book.getLevel(Side::BUY, 0).second, 6L);
}

void test_full_cancel_erases_level() {
    OrderBookBuilder book("AAPL");
    book.onAdd(1, Side::BUY, 100.0, 10);
    book.onCancel(1, 10);
    CHECK_NAN(book.getLevel(Side::BUY, 0).first);  // level gone
    // Cancelling again is a safe no-op (order already erased).
    book.onCancel(1, 10);
    CHECK_NAN(book.getLevel(Side::BUY, 0).first);
}

void test_cancel_one_of_two_at_level() {
    OrderBookBuilder book("AAPL");
    book.onAdd(1, Side::BUY, 100.0, 10);
    book.onAdd(2, Side::BUY, 100.0, 7);
    book.onCancel(1, 10);                          // remove only id 1
    CHECK_EQ(book.getLevel(Side::BUY, 0).second, 7L);
}

// -----------------------------------------------------------------------------
// Modify
// -----------------------------------------------------------------------------
void test_modify_up_and_down() {
    OrderBookBuilder book("AAPL");
    book.onAdd(1, Side::SELL, 101.0, 5);
    book.onModify(1, 8);                           // +3
    CHECK_EQ(book.getLevel(Side::SELL, 0).second, 8L);
    book.onModify(1, 2);                           // -6
    CHECK_EQ(book.getLevel(Side::SELL, 0).second, 2L);
    book.onModify(1, 0);                           // to zero -> level erased
    CHECK_NAN(book.getLevel(Side::SELL, 0).first);
}

// -----------------------------------------------------------------------------
// Trade consumes ask-side liquidity at px
// -----------------------------------------------------------------------------
void test_trade_reduces_ask() {
    OrderBookBuilder book("AAPL");
    book.onAdd(1, Side::SELL, 101.0, 10);
    book.onTrade(101.0, 4);
    CHECK_EQ(book.getLevel(Side::SELL, 0).second, 6L);
    book.onTrade(101.0, 6);                        // fully consumed
    CHECK_NAN(book.getLevel(Side::SELL, 0).first);
}

// -----------------------------------------------------------------------------
// Crossed book detection  (Example 3 from the spec)
// -----------------------------------------------------------------------------
void test_crossed_book() {
    OrderBookBuilder book("AAPL");
    book.onAdd(1, Side::BUY, 102.0, 5);
    book.onAdd(2, Side::SELL, 101.0, 3);           // bid 102 >= ask 101
    CHECK(book.isCrossed());
}

void test_not_crossed_when_one_side_empty() {
    OrderBookBuilder book("AAPL");
    book.onAdd(1, Side::BUY, 102.0, 5);
    CHECK(!book.isCrossed());                       // no asks -> not crossed
}

// -----------------------------------------------------------------------------
// Gap + snapshot recovery  (Example 4 from the spec)
// -----------------------------------------------------------------------------
void test_stale_and_snapshot() {
    OrderBookBuilder book("AAPL");
    book.onAdd(1, Side::BUY, 100.0, 10);
    book.markStale();
    CHECK(book.isStale());

    std::vector<OrderEntry> snap = {{2, 101.0, 5, false}};  // fresh ask only
    book.onSnapshot(snap);
    CHECK(!book.isStale());
    CHECK_NAN(book.getLevel(Side::BUY, 0).first);   // old bid cleared
    CHECK_EQ(book.getLevel(Side::SELL, 0).first, 101.0);
    CHECK_EQ(book.getLevel(Side::SELL, 0).second, 5L);
}

}  // namespace

int main() {
    RUN_TEST(test_empty_book);
    RUN_TEST(test_basic_book);
    RUN_TEST(test_level_aggregation);
    RUN_TEST(test_level_ordering);

    RUN_TEST(test_partial_cancel);
    RUN_TEST(test_full_cancel_erases_level);
    RUN_TEST(test_cancel_one_of_two_at_level);

    RUN_TEST(test_modify_up_and_down);

    RUN_TEST(test_trade_reduces_ask);

    RUN_TEST(test_crossed_book);
    RUN_TEST(test_not_crossed_when_one_side_empty);

    RUN_TEST(test_stale_and_snapshot);

    std::cout << "\n--------------------------------------------\n";
    std::cout << "Checks: " << g_checks << "   Failures: " << g_failures << '\n';
    if (g_failures == 0) {
        std::cout << "ALL TESTS PASSED\n";
        return 0;
    }
    std::cout << "SOME TESTS FAILED\n";
    return 1;
}
