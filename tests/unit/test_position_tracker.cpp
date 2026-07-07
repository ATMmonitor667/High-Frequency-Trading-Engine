#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include "../../src/portfolio/position_tracker.h"

// =============================================================================
// Standalone unit tests for PositionTracker (no external test framework).
//
// Build & run (one line):
//   g++ -std=c++17 -pthread tests/unit/test_position_tracker.cpp
//       src/portfolio/position_tracker.cpp -o test_pt && ./test_pt
//
// Exit code is 0 only if every check passes (CI-friendly).
// (Mirrors the harness used by test_risk_engine.cpp.)
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

PositionFill buy(const std::string& sym, double px, long qty, int64_t ts = 0) {
    return PositionFill{sym, true, px, qty, ts};
}
PositionFill sell(const std::string& sym, double px, long qty, int64_t ts = 0) {
    return PositionFill{sym, false, px, qty, ts};
}

bool hasAlert(const std::vector<MonitoringAlert>& v, const std::string& type) {
    for (const auto& a : v)
        if (a.type == type) return true;
    return false;
}

// -----------------------------------------------------------------------------
// Spec Example 1: BUY 10@100 then SELL 10@110 -> realised 100, flat position.
// -----------------------------------------------------------------------------
void test_example1_buy_then_sell() {
    PositionTracker pt;
    pt.onFill(buy("AAPL", 100.0, 10));
    pt.onFill(sell("AAPL", 110.0, 10));

    CHECK(approx(pt.getRealisedPnL("AAPL"), 100.0));

    Position p = pt.getPosition("AAPL");
    CHECK(p.netQty == 0);
    CHECK(approx(p.avgCostPx, 0.0));
}

// -----------------------------------------------------------------------------
// Spec Example 2: partial close. BUY 10@100, SELL 4@105 -> realised 20,
// 6 lots remain @100.
// -----------------------------------------------------------------------------
void test_example2_partial_close() {
    PositionTracker pt;
    pt.onFill(buy("X", 100.0, 10));
    pt.onFill(sell("X", 105.0, 4));

    CHECK(approx(pt.getRealisedPnL("X"), 20.0));

    Position p = pt.getPosition("X");
    CHECK(p.netQty == 6);
    CHECK(approx(p.avgCostPx, 100.0));
}

// -----------------------------------------------------------------------------
// FIFO consumes oldest lots first, across lots at different prices.
//   BUY 5@100, BUY 5@110, SELL 8@120
//   -> (120-100)*5 + (120-110)*3 = 100 + 30 = 130 ; 2 remain @110
// -----------------------------------------------------------------------------
void test_fifo_matching() {
    PositionTracker pt;
    pt.onFill(buy("X", 100.0, 5));
    pt.onFill(buy("X", 110.0, 5));
    pt.onFill(sell("X", 120.0, 8));

    CHECK(approx(pt.getRealisedPnL("X"), 130.0));

    Position p = pt.getPosition("X");
    CHECK(p.netQty == 2);
    CHECK(approx(p.avgCostPx, 110.0));  // only the remaining lot's price
}

// -----------------------------------------------------------------------------
// Average cost is the quantity-weighted mean of open lots.
//   BUY 4@100, BUY 6@110 -> (400 + 660)/10 = 106
// -----------------------------------------------------------------------------
void test_weighted_avg_cost() {
    PositionTracker pt;
    pt.onFill(buy("X", 100.0, 4));
    pt.onFill(buy("X", 110.0, 6));

    Position p = pt.getPosition("X");
    CHECK(p.netQty == 10);
    CHECK(approx(p.avgCostPx, 106.0));
}

// -----------------------------------------------------------------------------
// Unrealised PnL = (markPx - avgCost) * netQty.
// -----------------------------------------------------------------------------
void test_unrealised_pnl() {
    PositionTracker pt;
    pt.onFill(buy("X", 100.0, 10));
    pt.onMark("X", 105.0, 1);

    CHECK(approx(pt.getUnrealisedPnL("X"), 50.0));   // (105-100)*10

    // Total PnL = realised (0) + unrealised (50).
    CHECK(approx(pt.getTotalPnL(), 50.0));
}

// -----------------------------------------------------------------------------
// Gross exposure = Σ|netQty*markPx| ; net delta = Σ netQty*markPx.
// (Long-only here, so both equal for a single long position.)
// -----------------------------------------------------------------------------
void test_risk_exposure() {
    PositionTracker pt;
    pt.onFill(buy("AAA", 100.0, 10));
    pt.onFill(buy("BBB", 50.0, 20));
    pt.onMark("AAA", 100.0, 1);
    pt.onMark("BBB", 50.0, 1);

    RiskMetrics rm = pt.getRisk();
    CHECK(approx(rm.grossExposure, 10 * 100.0 + 20 * 50.0));  // 1000 + 1000
    CHECK(approx(rm.netDelta, 2000.0));
}

// -----------------------------------------------------------------------------
// A mark move > 5% within 60s fires a MarkJump alert; drainAlerts clears it.
// -----------------------------------------------------------------------------
void test_mark_jump_alert() {
    PositionTracker pt;
    pt.onFill(buy("X", 100.0, 1));
    pt.onMark("X", 100.0, 1'000'000'000LL);          // baseline mark
    pt.onMark("X", 107.0, 2'000'000'000LL);          // +7% within 1s -> alert

    auto alerts = pt.drainAlerts();
    CHECK(hasAlert(alerts, "MarkJump"));

    // drainAlerts cleared the queue.
    CHECK(pt.drainAlerts().empty());
}

// -----------------------------------------------------------------------------
// A small move (< 5%) does not fire MarkJump.
// -----------------------------------------------------------------------------
void test_no_mark_jump_small_move() {
    PositionTracker pt;
    pt.onFill(buy("X", 100.0, 1));
    pt.onMark("X", 100.0, 1'000'000'000LL);
    pt.onMark("X", 103.0, 2'000'000'000LL);  // +3% -> no alert

    CHECK(!hasAlert(pt.drainAlerts(), "MarkJump"));
}

// -----------------------------------------------------------------------------
// A move outside the 60s window does not fire MarkJump.
// -----------------------------------------------------------------------------
void test_no_mark_jump_outside_window() {
    PositionTracker pt;
    pt.onFill(buy("X", 100.0, 1));
    pt.onMark("X", 100.0, 0);
    pt.onMark("X", 200.0, 120LL * 1'000'000'000LL);  // +100% but 120s later

    CHECK(!hasAlert(pt.drainAlerts(), "MarkJump"));
}

// -----------------------------------------------------------------------------
// Gross exposure above 5,000,000 fires a GrossExposure alert.
// -----------------------------------------------------------------------------
void test_gross_exposure_alert() {
    PositionTracker pt;
    pt.onFill(buy("X", 100.0, 100000));   // notional 10,000,000
    pt.onMark("X", 100.0, 1);             // exposure = 100000 * 100 = 1e7 > 5e6

    CHECK(hasAlert(pt.drainAlerts(), "GrossExposure"));
}

// -----------------------------------------------------------------------------
// A drawdown > 10% from peak equity fires a Drawdown alert and updates
// max drawdown.
//   BUY 10@100 ; mark 110 -> equity 100 (peak) ; mark 105 -> equity 50
//   drawdown = (100-50)/100 = 0.5
// -----------------------------------------------------------------------------
void test_drawdown_alert() {
    PositionTracker pt;
    pt.onFill(buy("X", 100.0, 10));
    pt.onMark("X", 110.0, 1);   // equity = (110-100)*10 = 100 -> peak 100
    pt.onMark("X", 105.0, 2);   // equity = 50 -> drawdown 0.5

    auto alerts = pt.drainAlerts();
    CHECK(hasAlert(alerts, "Drawdown"));

    RiskMetrics rm = pt.getRisk();
    CHECK(approx(rm.peakEquity, 100.0));
    CHECK(rm.maxDrawdown > 0.10);
    CHECK(approx(rm.currentDrawdown, 0.5));
}

// -----------------------------------------------------------------------------
// Realised PnL is negative on a losing close; total across symbols sums.
// -----------------------------------------------------------------------------
void test_losing_close_and_total() {
    PositionTracker pt;
    pt.onFill(buy("X", 100.0, 10));
    pt.onFill(sell("X", 90.0, 10));   // realised = (90-100)*10 = -100

    CHECK(approx(pt.getRealisedPnL("X"), -100.0));
    CHECK(pt.getPosition("X").netQty == 0);
    CHECK(approx(pt.getTotalPnL(), -100.0));
}

// -----------------------------------------------------------------------------
// Unknown symbol lookups return zeroed values, no throw.
// -----------------------------------------------------------------------------
void test_unknown_symbol() {
    PositionTracker pt;

    Position p = pt.getPosition("GHOST");
    CHECK(p.netQty == 0);
    CHECK(approx(p.avgCostPx, 0.0));
    CHECK(approx(pt.getRealisedPnL("GHOST"), 0.0));
    CHECK(approx(pt.getUnrealisedPnL("GHOST"), 0.0));
}

}  // namespace

int main() {
    std::cout << "Running PositionTracker tests...\n";

    RUN_TEST(test_example1_buy_then_sell);
    RUN_TEST(test_example2_partial_close);
    RUN_TEST(test_fifo_matching);
    RUN_TEST(test_weighted_avg_cost);
    RUN_TEST(test_unrealised_pnl);
    RUN_TEST(test_risk_exposure);
    RUN_TEST(test_mark_jump_alert);
    RUN_TEST(test_no_mark_jump_small_move);
    RUN_TEST(test_no_mark_jump_outside_window);
    RUN_TEST(test_gross_exposure_alert);
    RUN_TEST(test_drawdown_alert);
    RUN_TEST(test_losing_close_and_total);
    RUN_TEST(test_unknown_symbol);

    std::cout << "\n" << (g_checks - g_failures) << "/" << g_checks
              << " checks passed.\n";
    if (g_failures == 0) {
        std::cout << "ALL TESTS PASSED\n";
        return 0;
    }
    std::cout << g_failures << " CHECK(S) FAILED\n";
    return 1;
}
