#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "../../src/core/orderbook/limit_order_book.h"

// =============================================================================
// Standalone unit tests for LimitOrderBook (no external test framework).
//
// Build & run:
//   g++ -std=c++17 tests/unit/test_limit_order_book.cpp
//       src/core/orderbook/limit_order_book.cpp -o test_lob && ./test_lob
//
// Exit code is 0 only if every check passes (CI-friendly).
// (Later: port these cases to GoogleTest as part of Phase 1 completion.)
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

// Assert a container size and bail out of the test if it is wrong, so that a
// failed expectation can never lead to an out-of-bounds access / segfault.
#define REQUIRE_SIZE(container, n)                                          \
    do {                                                                    \
        check_eq((container).size(), size_t(n), #container ".size() == " #n, \
                 __LINE__);                                                 \
        if ((container).size() != size_t(n)) return;                        \
    } while (0)

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

// Sum of all quantities reported by getDepth for a side (helper).
long depth_total(const LimitOrderBook& book, Side side, int levels) {
    long total = 0;
    for (const auto& lvl : book.getDepth(side, levels)) total += lvl.second;
    return total;
}

// -----------------------------------------------------------------------------
// Empty-book behaviour
// -----------------------------------------------------------------------------
void test_empty_book() {
    LimitOrderBook book;
    CHECK_NAN(book.best_bid());
    CHECK_NAN(book.best_ask());
    CHECK(book.match().empty());
    CHECK(book.getDepth(Side::BUY, 5).empty());
    CHECK(book.getDepth(Side::SELL, 5).empty());
    // Operations on unknown ids must be safe no-ops.
    book.cancel(999);
    book.modify(999, 10);
    CHECK_NAN(book.best_bid());
    CHECK_NAN(book.best_ask());
}

// -----------------------------------------------------------------------------
// Basic insertion / top-of-book
// -----------------------------------------------------------------------------
void test_single_add() {
    LimitOrderBook book;
    book.add(1, Side::BUY, 100.0, 10);
    CHECK_EQ(book.best_bid(), 100.0);
    CHECK_NAN(book.best_ask());

    book.add(2, Side::SELL, 101.0, 5);
    CHECK_EQ(book.best_ask(), 101.0);
    CHECK_EQ(book.best_bid(), 100.0);
}

void test_best_bid_ask_ordering() {
    LimitOrderBook book;
    book.add(1, Side::BUY, 100.0, 1);
    book.add(2, Side::BUY, 101.0, 1);  // higher bid is better
    book.add(3, Side::BUY, 99.0, 1);
    CHECK_EQ(book.best_bid(), 101.0);

    book.add(4, Side::SELL, 105.0, 1);
    book.add(5, Side::SELL, 103.0, 1);  // lower ask is better
    book.add(6, Side::SELL, 104.0, 1);
    CHECK_EQ(book.best_ask(), 103.0);
}

void test_duplicate_id_ignored() {
    LimitOrderBook book;
    book.add(1, Side::BUY, 100.0, 10);
    book.add(1, Side::BUY, 200.0, 99);  // duplicate id -> ignored
    CHECK_EQ(book.best_bid(), 100.0);
    CHECK_EQ(depth_total(book, Side::BUY, 5), 10L);
}

// -----------------------------------------------------------------------------
// Matching: no cross, full fill, partial fills, price improvement
// -----------------------------------------------------------------------------
void test_no_cross() {
    LimitOrderBook book;
    book.add(1, Side::BUY, 99.0, 10);
    book.add(2, Side::SELL, 100.0, 10);
    auto fills = book.match();
    CHECK(fills.empty());
    CHECK_EQ(book.best_bid(), 99.0);
    CHECK_EQ(book.best_ask(), 100.0);
}

void test_exact_full_fill() {
    LimitOrderBook book;
    book.add(1, Side::BUY, 100.0, 10);
    book.add(2, Side::SELL, 100.0, 10);
    auto fills = book.match();
    REQUIRE_SIZE(fills, 1);
    CHECK_EQ(fills[0].makerOrderId, uint64_t(1));  // lower id rests => maker
    CHECK_EQ(fills[0].takerOrderId, uint64_t(2));
    CHECK_EQ(fills[0].price, 100.0);
    CHECK_EQ(fills[0].qty, 10L);
    CHECK_NAN(book.best_bid());
    CHECK_NAN(book.best_ask());
}

void test_partial_fill_taker_larger() {
    // Aggressive bid bigger than resting ask: ask fully filled, bid remainder rests.
    LimitOrderBook book;
    book.add(1, Side::SELL, 99.0, 5);
    book.add(2, Side::BUY, 100.0, 10);
    auto fills = book.match();
    REQUIRE_SIZE(fills, 1);
    CHECK_EQ(fills[0].makerOrderId, uint64_t(1));  // resting ask is maker
    CHECK_EQ(fills[0].takerOrderId, uint64_t(2));
    CHECK_EQ(fills[0].price, 99.0);                // fill at maker (ask) price
    CHECK_EQ(fills[0].qty, 5L);
    CHECK_NAN(book.best_ask());
    CHECK_EQ(book.best_bid(), 100.0);
    CHECK_EQ(depth_total(book, Side::BUY, 5), 5L);  // 10 - 5 remainder
}

void test_partial_fill_maker_larger() {
    // Resting ask bigger than aggressive bid: bid fully filled, ask remainder rests.
    LimitOrderBook book;
    book.add(1, Side::SELL, 100.0, 15);
    book.add(2, Side::BUY, 100.0, 10);
    auto fills = book.match();
    REQUIRE_SIZE(fills, 1);
    CHECK_EQ(fills[0].makerOrderId, uint64_t(1));
    CHECK_EQ(fills[0].takerOrderId, uint64_t(2));
    CHECK_EQ(fills[0].qty, 10L);
    CHECK_NAN(book.best_bid());
    CHECK_EQ(book.best_ask(), 100.0);
    CHECK_EQ(depth_total(book, Side::SELL, 5), 5L);
}

void test_price_improvement_at_maker_price() {
    // Aggressive buyer willing to pay 105 hits resting ask at 100 -> fills at 100.
    LimitOrderBook book;
    book.add(1, Side::SELL, 100.0, 10);
    book.add(2, Side::BUY, 105.0, 10);
    auto fills = book.match();
    REQUIRE_SIZE(fills, 1);
    CHECK_EQ(fills[0].price, 100.0);
    CHECK_EQ(fills[0].makerOrderId, uint64_t(1));
    CHECK_EQ(fills[0].takerOrderId, uint64_t(2));
}

// -----------------------------------------------------------------------------
// FIFO / price-time priority
// -----------------------------------------------------------------------------
void test_fifo_same_level() {
    // Two resting asks at the same price; head (first added) must fill first.
    LimitOrderBook book;
    book.add(10, Side::SELL, 100.0, 5);  // head
    book.add(11, Side::SELL, 100.0, 5);
    book.add(20, Side::BUY, 101.0, 7);   // aggressive, crosses
    auto fills = book.match();
    REQUIRE_SIZE(fills, 2);
    CHECK_EQ(fills[0].makerOrderId, uint64_t(10));  // earliest ask first
    CHECK_EQ(fills[0].qty, 5L);
    CHECK_EQ(fills[1].makerOrderId, uint64_t(11));
    CHECK_EQ(fills[1].qty, 2L);
    CHECK_EQ(book.best_ask(), 100.0);
    CHECK_EQ(depth_total(book, Side::SELL, 5), 3L);  // 5 - 2 remaining on id 11
    CHECK_NAN(book.best_bid());
}

void test_multi_level_sweep() {
    // Aggressive bid sweeps cheapest ask first, then the next level.
    LimitOrderBook book;
    book.add(1, Side::SELL, 100.0, 5);
    book.add(2, Side::SELL, 101.0, 5);
    book.add(3, Side::BUY, 101.0, 8);
    auto fills = book.match();
    REQUIRE_SIZE(fills, 2);
    CHECK_EQ(fills[0].makerOrderId, uint64_t(1));
    CHECK_EQ(fills[0].price, 100.0);
    CHECK_EQ(fills[0].qty, 5L);
    CHECK_EQ(fills[1].makerOrderId, uint64_t(2));
    CHECK_EQ(fills[1].price, 101.0);
    CHECK_EQ(fills[1].qty, 3L);
    CHECK_EQ(book.best_ask(), 101.0);
    CHECK_EQ(depth_total(book, Side::SELL, 5), 2L);
    CHECK_NAN(book.best_bid());
}

void test_equal_price_maker_is_lower_id() {
    // At equal price, the earlier order (lower id) is treated as the maker.
    {
        LimitOrderBook book;
        book.add(5, Side::BUY, 100.0, 5);   // rests first
        book.add(6, Side::SELL, 100.0, 5);
        auto fills = book.match();
        REQUIRE_SIZE(fills, 1);
        CHECK_EQ(fills[0].makerOrderId, uint64_t(5));
        CHECK_EQ(fills[0].takerOrderId, uint64_t(6));
    }
    {
        LimitOrderBook book;
        book.add(5, Side::SELL, 100.0, 5);  // rests first
        book.add(6, Side::BUY, 100.0, 5);
        auto fills = book.match();
        REQUIRE_SIZE(fills, 1);
        CHECK_EQ(fills[0].makerOrderId, uint64_t(5));
        CHECK_EQ(fills[0].takerOrderId, uint64_t(6));
    }
}

// -----------------------------------------------------------------------------
// Cancel
// -----------------------------------------------------------------------------
void test_cancel_clears_level() {
    LimitOrderBook book;
    book.add(1, Side::BUY, 100.0, 10);
    book.cancel(1);
    CHECK_NAN(book.best_bid());
    CHECK(book.getDepth(Side::BUY, 5).empty());
    // Cancelling again is a safe no-op.
    book.cancel(1);
    CHECK_NAN(book.best_bid());
}

void test_cancel_updates_best() {
    LimitOrderBook book;
    book.add(1, Side::BUY, 100.0, 1);
    book.add(2, Side::BUY, 101.0, 1);  // best
    book.cancel(2);
    CHECK_EQ(book.best_bid(), 100.0);
}

void test_cancel_middle_of_fifo() {
    // Cancel the middle order; the surviving FIFO order must be preserved.
    LimitOrderBook book;
    book.add(1, Side::SELL, 100.0, 4);  // head
    book.add(2, Side::SELL, 100.0, 5);  // middle -> cancelled
    book.add(3, Side::SELL, 100.0, 6);  // tail
    book.cancel(2);
    CHECK_EQ(depth_total(book, Side::SELL, 5), 10L);  // 4 + 6

    book.add(20, Side::BUY, 100.0, 4);
    auto fills = book.match();
    REQUIRE_SIZE(fills, 1);
    CHECK_EQ(fills[0].makerOrderId, uint64_t(1));  // head still id 1
    CHECK_EQ(fills[0].qty, 4L);
    CHECK_EQ(depth_total(book, Side::SELL, 5), 6L);  // only id 3 left
}

void test_cancel_then_no_match() {
    LimitOrderBook book;
    book.add(1, Side::BUY, 100.0, 10);
    book.add(2, Side::SELL, 100.0, 10);
    book.cancel(1);  // remove the bid before matching
    auto fills = book.match();
    CHECK(fills.empty());
    CHECK_EQ(book.best_ask(), 100.0);
    CHECK_NAN(book.best_bid());
}

// -----------------------------------------------------------------------------
// Modify
// -----------------------------------------------------------------------------
void test_modify_decrease_keeps_priority() {
    // Decreasing qty must keep the order at the head of the FIFO queue.
    LimitOrderBook book;
    book.add(1, Side::SELL, 100.0, 5);  // head
    book.add(2, Side::SELL, 100.0, 5);
    book.modify(1, 3);  // decrease
    CHECK_EQ(depth_total(book, Side::SELL, 5), 8L);  // 3 + 5

    book.add(20, Side::BUY, 100.0, 3);
    auto fills = book.match();
    REQUIRE_SIZE(fills, 1);
    CHECK_EQ(fills[0].makerOrderId, uint64_t(1));  // id 1 still head
    CHECK_EQ(fills[0].qty, 3L);
}

void test_modify_increase_loses_priority() {
    // Increasing qty must re-queue the order at the tail (loses time priority).
    LimitOrderBook book;
    book.add(1, Side::SELL, 100.0, 5);  // head
    book.add(2, Side::SELL, 100.0, 5);
    book.modify(1, 8);  // increase -> moves to tail; head becomes id 2
    CHECK_EQ(depth_total(book, Side::SELL, 5), 13L);  // 8 + 5

    book.add(20, Side::BUY, 101.0, 5);  // crosses (bid price > ask): maker=ask
    auto fills = book.match();
    REQUIRE_SIZE(fills, 1);
    CHECK_EQ(fills[0].makerOrderId, uint64_t(2));  // id 2 is the new head
    CHECK_EQ(fills[0].qty, 5L);
}

void test_modify_to_zero_cancels() {
    LimitOrderBook book;
    book.add(1, Side::BUY, 100.0, 10);
    book.modify(1, 0);  // qty <= 0 cancels
    CHECK_NAN(book.best_bid());

    book.add(2, Side::BUY, 100.0, 10);
    book.modify(2, -5);  // negative also cancels
    CHECK_NAN(book.best_bid());
}

void test_modify_unknown_and_noop() {
    LimitOrderBook book;
    book.add(1, Side::BUY, 100.0, 10);
    book.modify(999, 5);   // unknown id -> no-op
    book.modify(1, 10);    // same qty -> no-op
    CHECK_EQ(book.best_bid(), 100.0);
    CHECK_EQ(depth_total(book, Side::BUY, 5), 10L);
}

// -----------------------------------------------------------------------------
// Depth
// -----------------------------------------------------------------------------
void test_depth_ordering_and_cap() {
    LimitOrderBook book;
    book.add(1, Side::BUY, 100.0, 1);
    book.add(2, Side::BUY, 101.0, 1);
    book.add(3, Side::BUY, 99.0, 1);
    auto bids = book.getDepth(Side::BUY, 2);  // cap at 2 levels
    REQUIRE_SIZE(bids, 2);
    CHECK_EQ(bids[0].first, 101.0);  // best first, descending
    CHECK_EQ(bids[1].first, 100.0);

    book.add(4, Side::SELL, 105.0, 1);
    book.add(5, Side::SELL, 103.0, 1);
    book.add(6, Side::SELL, 104.0, 1);
    auto asks = book.getDepth(Side::SELL, 10);  // request more than exist
    REQUIRE_SIZE(asks, 3);
    CHECK_EQ(asks[0].first, 103.0);  // best first, ascending
    CHECK_EQ(asks[1].first, 104.0);
    CHECK_EQ(asks[2].first, 105.0);
}

void test_depth_aggregates_quantity() {
    LimitOrderBook book;
    book.add(1, Side::BUY, 100.0, 5);
    book.add(2, Side::BUY, 100.0, 7);  // same level
    auto bids = book.getDepth(Side::BUY, 5);
    REQUIRE_SIZE(bids, 1);
    CHECK_EQ(bids[0].first, 100.0);
    CHECK_EQ(bids[0].second, 12L);  // aggregated qty
}

// -----------------------------------------------------------------------------
// Re-matching after refill
// -----------------------------------------------------------------------------
void test_match_idempotent_then_refill() {
    LimitOrderBook book;
    book.add(1, Side::BUY, 100.0, 10);
    book.add(2, Side::SELL, 100.0, 10);
    CHECK_EQ(book.match().size(), size_t(1));
    CHECK(book.match().empty());  // nothing left to cross

    // Book is empty; adding a new crossing pair matches again.
    book.add(3, Side::BUY, 50.0, 4);
    book.add(4, Side::SELL, 50.0, 4);
    auto fills = book.match();
    REQUIRE_SIZE(fills, 1);
    CHECK_EQ(fills[0].qty, 4L);
    CHECK_NAN(book.best_bid());
    CHECK_NAN(book.best_ask());
}

}  // namespace

int main() {
    RUN_TEST(test_empty_book);
    RUN_TEST(test_single_add);
    RUN_TEST(test_best_bid_ask_ordering);
    RUN_TEST(test_duplicate_id_ignored);

    RUN_TEST(test_no_cross);
    RUN_TEST(test_exact_full_fill);
    RUN_TEST(test_partial_fill_taker_larger);
    RUN_TEST(test_partial_fill_maker_larger);
    RUN_TEST(test_price_improvement_at_maker_price);

    RUN_TEST(test_fifo_same_level);
    RUN_TEST(test_multi_level_sweep);
    RUN_TEST(test_equal_price_maker_is_lower_id);

    RUN_TEST(test_cancel_clears_level);
    RUN_TEST(test_cancel_updates_best);
    RUN_TEST(test_cancel_middle_of_fifo);
    RUN_TEST(test_cancel_then_no_match);

    RUN_TEST(test_modify_decrease_keeps_priority);
    RUN_TEST(test_modify_increase_loses_priority);
    RUN_TEST(test_modify_to_zero_cancels);
    RUN_TEST(test_modify_unknown_and_noop);

    RUN_TEST(test_depth_ordering_and_cap);
    RUN_TEST(test_depth_aggregates_quantity);

    RUN_TEST(test_match_idempotent_then_refill);

    std::cout << "\n--------------------------------------------\n";
    std::cout << "Checks: " << g_checks << "   Failures: " << g_failures << '\n';
    if (g_failures == 0) {
        std::cout << "ALL TESTS PASSED\n";
        return 0;
    }
    std::cout << "SOME TESTS FAILED\n";
    return 1;
}
