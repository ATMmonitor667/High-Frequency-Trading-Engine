#include <iostream>
#include <string>

#include "../../src/risk/risk_engine.h"

// =============================================================================
// Standalone unit tests for RiskEngine (no external test framework).
//
// Build & run:
//   g++ -std=c++17 tests/unit/test_risk_engine.cpp
//       src/risk/risk_engine.cpp -o test_risk && ./test_risk
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

#define CHECK(cond) check_true((cond), #cond, __LINE__)

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

OrderRequest makeOrder(uint64_t id, const char* sym, Side side, double px,
                       long qty, int64_t tsNs) {
    OrderRequest o;
    o.clientOrderId = id;
    o.symbol = sym;
    o.side = side;
    o.px = px;
    o.qty = qty;
    o.tsNs = tsNs;
    return o;
}

bool failedWith(const CheckResult& r, const char* needle) {
    return r.result == RiskResult::FAIL &&
           r.reason.find(needle) != std::string::npos;
}

// -----------------------------------------------------------------------------
// Example 1: position-limit breach (95 + 10 = 105 > 100)
// -----------------------------------------------------------------------------
void test_position_limit_breach() {
    RiskConfig cfg;
    cfg.positionLimit = 100;
    RiskEngine engine(cfg);
    engine.onFill({"AAPL", Side::BUY, 100.0, 95, 0.0});
    auto r = engine.check(makeOrder(1, "AAPL", Side::BUY, 100.0, 10, 1));
    CHECK(failedWith(r, "PositionLimit"));
}

// -----------------------------------------------------------------------------
// Example 2: circuit breaker trips on daily loss and halts all orders
// -----------------------------------------------------------------------------
void test_circuit_breaker_halts() {
    RiskConfig cfg;
    cfg.maxDailyLoss = 50000.0;
    RiskEngine engine(cfg);
    engine.onFill({"AAPL", Side::BUY, 100.0, 100, -51000.0});
    auto r = engine.check(makeOrder(1, "AAPL", Side::BUY, 100.0, 1, 1));
    CHECK(failedWith(r, "CircuitBreaker"));
}

// -----------------------------------------------------------------------------
// The breaker latches: it stays tripped even after pnl recovers, until reset()
// is called. Only then does trading resume.
// -----------------------------------------------------------------------------
void test_circuit_breaker_latch_and_reset() {
    RiskConfig cfg;
    cfg.maxDailyLoss = 50000.0;
    RiskEngine engine(cfg);

    engine.onFill({"AAPL", Side::BUY, 100.0, 100, -60000.0});  // breach -> trips
    CHECK(failedWith(engine.check(makeOrder(1, "AAPL", Side::BUY, 100.0, 1, 1)),
                     "CircuitBreaker"));

    engine.onFill({"AAPL", Side::SELL, 100.0, 100, 25000.0});  // pnl now -35000
    // Still latched despite the recovery.
    CHECK(failedWith(engine.check(makeOrder(2, "AAPL", Side::BUY, 100.0, 1, 2)),
                     "CircuitBreaker"));

    engine.resetCircuitBreaker();  // operator clears the halt
    CHECK(engine.check(makeOrder(3, "AAPL", Side::BUY, 100.0, 1, 3)).result ==
          RiskResult::PASS);
}

// -----------------------------------------------------------------------------
// Example 3: duplicate clientOrderId within the dedupe window is rejected
// -----------------------------------------------------------------------------
void test_duplicate_within_window() {
    RiskConfig cfg;
    cfg.dedupeWindowMs = 5000;
    RiskEngine engine(cfg);
    engine.check(makeOrder(42, "AAPL", Side::BUY, 100.0, 10, 1'000'000'000LL));
    auto r = engine.check(makeOrder(42, "AAPL", Side::BUY, 100.0, 10, 1'100'000'000LL));
    CHECK(failedWith(r, "Duplicate"));
}

// -----------------------------------------------------------------------------
// Duplicate id is allowed again once the window has expired
// -----------------------------------------------------------------------------
void test_duplicate_expires() {
    RiskConfig cfg;
    cfg.dedupeWindowMs = 100;
    RiskEngine engine(cfg);
    engine.check(makeOrder(7, "AAPL", Side::BUY, 100.0, 10, 1'000'000'000LL));
    auto r = engine.check(makeOrder(7, "AAPL", Side::BUY, 100.0, 10, 1'200'000'000LL));
    CHECK(r.result == RiskResult::PASS);
}

// -----------------------------------------------------------------------------
// Notional cap: 100 * 20 = 2000 > 1000
// -----------------------------------------------------------------------------
void test_notional_limit() {
    RiskConfig cfg;
    cfg.notionalLimit = 1000.0;
    RiskEngine engine(cfg);
    auto r = engine.check(makeOrder(1, "AAPL", Side::BUY, 100.0, 20, 1));
    CHECK(failedWith(r, "Notional"));
}

// -----------------------------------------------------------------------------
// Rate limiter: a full bucket allows a short burst, then rejects until it
// refills. Timestamps are nonzero (real wall-clock ns) and spaced by 1us so the
// refill within the burst is negligible.
// -----------------------------------------------------------------------------
void test_rate_limit() {
    RiskConfig cfg;
    cfg.ordersPerSec = 2.0;   // bucket holds 2 tokens
    RiskEngine engine(cfg);
    const int64_t t0 = 1'000'000'000LL;  // 1 second, nonzero
    CHECK(engine.check(makeOrder(1, "AAPL", Side::BUY, 1.0, 1, t0)).result == RiskResult::PASS);
    CHECK(engine.check(makeOrder(2, "AAPL", Side::BUY, 1.0, 1, t0 + 1000)).result == RiskResult::PASS);
    CHECK(failedWith(engine.check(makeOrder(3, "AAPL", Side::BUY, 1.0, 1, t0 + 2000)), "RateLimit"));
    // One second later a token has refilled, so the next order passes.
    CHECK(engine.check(makeOrder(4, "AAPL", Side::BUY, 1.0, 1, t0 + 1'000'000'000LL)).result == RiskResult::PASS);
}

// -----------------------------------------------------------------------------
// A clean order passes the whole chain with an empty reason
// -----------------------------------------------------------------------------
void test_clean_order_passes() {
    RiskConfig cfg;
    RiskEngine engine(cfg);
    auto r = engine.check(makeOrder(1, "AAPL", Side::BUY, 100.0, 10, 1));
    CHECK(r.result == RiskResult::PASS);
    CHECK(r.reason.empty());
}

}  // namespace

int main() {
    RUN_TEST(test_position_limit_breach);
    RUN_TEST(test_circuit_breaker_halts);
    RUN_TEST(test_circuit_breaker_latch_and_reset);
    RUN_TEST(test_duplicate_within_window);
    RUN_TEST(test_duplicate_expires);
    RUN_TEST(test_notional_limit);
    RUN_TEST(test_rate_limit);
    RUN_TEST(test_clean_order_passes);

    std::cout << "\n--------------------------------------------\n";
    std::cout << "Checks: " << g_checks << "   Failures: " << g_failures << '\n';
    if (g_failures == 0) {
        std::cout << "ALL TESTS PASSED\n";
        return 0;
    }
    std::cout << "SOME TESTS FAILED\n";
    return 1;
}
