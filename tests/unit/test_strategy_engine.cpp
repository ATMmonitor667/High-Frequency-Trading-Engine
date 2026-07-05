#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>

#include "../../src/strategy/strategy_engine.h"
#include "../../src/strategy/book_adapter.h"

// =============================================================================
// Standalone unit tests for StrategyEngine + book_adapter (no test framework).
//
// Build & run:
//   g++ -std=c++17 tests/unit/test_strategy_engine.cpp \
//       src/strategy/strategy_engine.cpp \
//       src/marketdata/book_builder/order_book_builder.cpp -o test_strat && ./test_strat
//
// Exit code is 0 only if every check passes (CI-friendly).
// (Mirrors the harness used by test_order_book_builder.cpp.)
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

BookUpdate makeUpdate(const char* sym, double bid, double ask,
                      long bidQty, long askQty, int64_t tsNs) {
    BookUpdate u;
    u.symbol = sym;
    u.bidPx = bid; u.askPx = ask;
    u.bidQty = bidQty; u.askQty = askQty;
    u.tsNs = tsNs;
    return u;
}

// -----------------------------------------------------------------------------
// Example 1: warming up -- 5 ticks, lookback 20 -> nullopt
// -----------------------------------------------------------------------------
void test_warmup_returns_nullopt() {
    EngineConfig cfg;
    cfg.lookback = 20;
    StrategyEngine engine(cfg);
    std::optional<OrderRequest> last;
    for (int i = 0; i < 5; ++i)
        last = engine.onBookUpdate(makeUpdate("AAPL", 100.0, 100.02, 500, 300, i * 1'000'000LL));
    CHECK(!last.has_value());
}

// -----------------------------------------------------------------------------
// Ring buffer overwrites oldest when full (lookback 3, momentum settles to 2)
// -----------------------------------------------------------------------------
void test_ring_overwrite_momentum() {
    EngineConfig cfg;
    cfg.lookback = 3;
    StrategyEngine engine(cfg);
    for (int i = 1; i <= 5; ++i)
        engine.onBookUpdate(makeUpdate("X", 100.0 + i, 100.0 + i, 10, 10, i));
    CHECK(std::abs(engine.computeMomentum() - 2.0) < 1e-9);
}

// -----------------------------------------------------------------------------
// computeSpread divide-by-zero guard (mid == 0 -> 0)
// -----------------------------------------------------------------------------
void test_spread_zero_guard() {
    EngineConfig cfg;
    StrategyEngine engine(cfg);
    engine.onBookUpdate(makeUpdate("X", 0.0, 0.0, 10, 10, 0));
    CHECK_EQ(engine.computeSpread(), 0.0);
}

// -----------------------------------------------------------------------------
// Example 3: cooldown suppresses the second order
// -----------------------------------------------------------------------------
void test_cooldown_suppresses_second() {
    EngineConfig cfg;
    cfg.lookback   = 2;
    cfg.threshold  = 0.01;
    cfg.cooldownMs = 100;
    StrategyEngine engine(cfg);

    engine.onBookUpdate(makeUpdate("AAPL", 100.0, 100.02, 1000, 100, 0));
    const auto first  = engine.onBookUpdate(makeUpdate("AAPL", 101.0, 101.02, 1000, 100, 50'000'000LL));
    const auto second = engine.onBookUpdate(makeUpdate("AAPL", 102.0, 102.02, 1000, 100, 100'000'000LL));

    CHECK(first.has_value());
    CHECK(!second.has_value());
}

// -----------------------------------------------------------------------------
// Example 2: strong buy signal fires with BUY side and configured qty
// -----------------------------------------------------------------------------
void test_buy_signal_fires() {
    EngineConfig cfg;
    cfg.lookback  = 2;
    cfg.threshold = 0.2;
    StrategyEngine engine(cfg);
    engine.onBookUpdate(makeUpdate("AAPL", 100.0, 100.02, 900, 100, 0));
    const auto order = engine.onBookUpdate(makeUpdate("AAPL", 102.0, 102.02, 900, 100, 1'000'000'000LL));
    CHECK(order.has_value());
    if (order.has_value()) {
        CHECK(order->side == Side::BUY);
        CHECK_EQ(order->qty, 100L);
        CHECK_EQ(order->px, 102.02);  // takes the ask
    }
}

// -----------------------------------------------------------------------------
// Adapter: stale book -> no update; empty side -> no update; healthy -> update
// -----------------------------------------------------------------------------
void test_adapter_respects_book_state() {
    OrderBookBuilder book("AAPL");

    // One-sided book -> adapter yields nothing.
    book.onAdd(1, Side::BUY, 100.0, 500);
    CHECK(!topOfBook(book, "AAPL", 0).has_value());

    // Two-sided book -> adapter yields a BookUpdate with the top of book.
    book.onAdd(2, Side::SELL, 100.04, 300);
    auto u = topOfBook(book, "AAPL", 42);
    CHECK(u.has_value());
    if (u.has_value()) {
        CHECK_EQ(u->bidPx, 100.0);
        CHECK_EQ(u->askPx, 100.04);
        CHECK_EQ(u->bidQty, 500L);
        CHECK_EQ(u->askQty, 300L);
    }

    // Stale book -> adapter refuses to trade until a snapshot recovers it.
    book.markStale();
    CHECK(!topOfBook(book, "AAPL", 1).has_value());
}

}  // namespace

int main() {
    RUN_TEST(test_warmup_returns_nullopt);
    RUN_TEST(test_ring_overwrite_momentum);
    RUN_TEST(test_spread_zero_guard);
    RUN_TEST(test_cooldown_suppresses_second);
    RUN_TEST(test_buy_signal_fires);
    RUN_TEST(test_adapter_respects_book_state);

    std::cout << "\n--------------------------------------------\n";
    std::cout << "Checks: " << g_checks << "   Failures: " << g_failures << '\n';
    if (g_failures == 0) {
        std::cout << "ALL TESTS PASSED\n";
        return 0;
    }
    std::cout << "SOME TESTS FAILED\n";
    return 1;
}
