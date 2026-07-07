#include <iostream>
#include <stdexcept>
#include <string>

#include "../../src/execution/order_manager/order_manager.h"

// =============================================================================
// Standalone unit tests for OrderManager (no external test framework).
//
// Build & run (one line):
//   g++ -std=c++17 -pthread tests/unit/test_order_manager.cpp
//       src/execution/order_manager/order_manager.cpp
//       src/risk/risk_engine.cpp -o test_oms && ./test_oms
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

// Convenience: default-configured risk engine (limits from RiskConfig defaults:
// positionLimit=10000, notionalLimit=1e6, ordersPerSec=100, maxDailyLoss=50k).
RiskEngine makeRisk() { return RiskEngine(RiskConfig{}); }

OmsOrderRequest req(const std::string& sym, bool buy, double px, long qty, int64_t ts) {
    return OmsOrderRequest{sym, buy, px, qty, ts};
}

// --- no-op callbacks used when the test doesn't inspect them -----------------
FillCallback   noFill   = [](const OrderRecord&, double, long) {};
RejectCallback noReject = [](const OrderRecord&, const std::string&) {};

// -----------------------------------------------------------------------------
// Example 1 from the spec: happy path submit -> ack -> full fill.
// -----------------------------------------------------------------------------
void test_happy_path() {
    RiskEngine risk = makeRisk();
    OrderManager oms(noFill, noReject, &risk);

    uint64_t id = oms.submit(req("AAPL", true, 100.0, 10, 1000));
    CHECK(id == 1);
    CHECK(oms.getOrder(id).state == OrderState::PENDING_NEW);

    oms.onAck(id, 2000);
    CHECK(oms.getOrder(id).state == OrderState::ACKNOWLEDGED);
    CHECK(oms.getOrder(id).ackNs == 2000);

    oms.onFill(id, 100.0, 10, /*isFull=*/true);
    const OrderRecord r = oms.getOrder(id);
    CHECK(r.state == OrderState::FILLED);
    CHECK(r.cumFillQty == 10);
    CHECK(r.avgFillPx == 100.0);
}

// -----------------------------------------------------------------------------
// Example 2 from the spec: partial fill, then cancel.
// -----------------------------------------------------------------------------
void test_partial_then_cancel() {
    RiskEngine risk = makeRisk();
    OrderManager oms(noFill, noReject, &risk);

    uint64_t id = oms.submit(req("MSFT", true, 99.5, 10, 1000));
    oms.onAck(id, 2000);
    oms.onFill(id, 99.5, 5, /*isFull=*/false);

    CHECK(oms.getOrder(id).state == OrderState::PARTIALLY_FILLED);
    CHECK(oms.getOrder(id).cumFillQty == 5);

    CHECK(oms.cancel(id) == true);
    oms.onCancelAck(id);
    CHECK(oms.getOrder(id).state == OrderState::CANCELLED);
}

// -----------------------------------------------------------------------------
// Example 3 from the spec: risk rejection returns 0 and creates no record.
// -----------------------------------------------------------------------------
void test_risk_reject() {
    RiskEngine risk = makeRisk();
    OrderManager oms(noFill, noReject, &risk);

    // notional = 100 * 999999 >> 1e6 limit -> NotionalCheck FAIL.
    uint64_t id = oms.submit(req("AAPL", true, 100.0, 999999, 1000));
    CHECK(id == 0);

    // The consumed id (1) must not have produced a stored record.
    bool threw = false;
    try { oms.getOrder(1); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);

    // A subsequent valid order still works (ids stay monotonic; 1 was consumed).
    uint64_t ok = oms.submit(req("AAPL", true, 100.0, 10, 2000));
    CHECK(ok == 2);
}

// -----------------------------------------------------------------------------
// No risk engine wired -> submit must fail closed (return 0).
// -----------------------------------------------------------------------------
void test_no_risk_engine() {
    OrderManager oms(noFill, noReject, nullptr);
    CHECK(oms.submit(req("AAPL", true, 100.0, 10, 1000)) == 0);
}

// -----------------------------------------------------------------------------
// Running weighted-average fill price across two partials at different prices.
//   (100*4 + 110*6) / 10 = 106.0
// -----------------------------------------------------------------------------
void test_avg_fill_price() {
    RiskEngine risk = makeRisk();
    OrderManager oms(noFill, noReject, &risk);

    uint64_t id = oms.submit(req("AAPL", true, 105.0, 10, 1000));
    oms.onAck(id, 2000);
    oms.onFill(id, 100.0, 4, false);
    CHECK(oms.getOrder(id).avgFillPx == 100.0);

    oms.onFill(id, 110.0, 6, true);
    const OrderRecord r = oms.getOrder(id);
    CHECK(r.cumFillQty == 10);
    CHECK(r.state == OrderState::FILLED);
    // Compare with a tolerance for floating point.
    CHECK(r.avgFillPx > 105.999 && r.avgFillPx < 106.001);
}

// -----------------------------------------------------------------------------
// Invalid state transitions are silently ignored (no throw, no state change).
// -----------------------------------------------------------------------------
void test_invalid_transitions() {
    RiskEngine risk = makeRisk();
    OrderManager oms(noFill, noReject, &risk);

    uint64_t id = oms.submit(req("AAPL", true, 100.0, 10, 1000));

    // Fill before ack: still PENDING_NEW -> ignored.
    oms.onFill(id, 100.0, 5, false);
    CHECK(oms.getOrder(id).state == OrderState::PENDING_NEW);
    CHECK(oms.getOrder(id).cumFillQty == 0);

    // Ack, then ack again: second ack must be a no-op.
    oms.onAck(id, 2000);
    oms.onAck(id, 9999);
    CHECK(oms.getOrder(id).state == OrderState::ACKNOWLEDGED);
    CHECK(oms.getOrder(id).ackNs == 2000);

    // Reject after ack: onReject only fires from PENDING_NEW -> ignored.
    oms.onReject(id, "too late");
    CHECK(oms.getOrder(id).state == OrderState::ACKNOWLEDGED);

    // Operations on unknown ids are safe no-ops / false.
    oms.onAck(999, 1);
    oms.onFill(999, 1.0, 1, true);
    oms.onCancelAck(999);
    oms.onReject(999, "x");
    CHECK(oms.cancel(999) == false);
}

// -----------------------------------------------------------------------------
// cancel() returns false from terminal / non-working states.
// -----------------------------------------------------------------------------
void test_cancel_guards() {
    RiskEngine risk = makeRisk();
    OrderManager oms(noFill, noReject, &risk);

    uint64_t id = oms.submit(req("AAPL", true, 100.0, 10, 1000));

    // PENDING_NEW is not yet working -> cannot cancel.
    CHECK(oms.cancel(id) == false);

    oms.onAck(id, 2000);
    CHECK(oms.cancel(id) == true);   // ACKNOWLEDGED is cancellable

    oms.onFill(id, 100.0, 10, true); // -> FILLED (terminal)
    CHECK(oms.cancel(id) == false);  // terminal -> false
}

// -----------------------------------------------------------------------------
// onCancelAck can move a working order to CANCELLED but must not overturn a
// terminal FILLED / REJECTED order.
// -----------------------------------------------------------------------------
void test_cancel_ack_respects_terminal() {
    RiskEngine risk = makeRisk();
    OrderManager oms(noFill, noReject, &risk);

    uint64_t filled = oms.submit(req("AAPL", true, 100.0, 10, 1000));
    oms.onAck(filled, 2000);
    oms.onFill(filled, 100.0, 10, true);
    oms.onCancelAck(filled);  // race: cancel-ack after full fill
    CHECK(oms.getOrder(filled).state == OrderState::FILLED);
}

// -----------------------------------------------------------------------------
// Reject callback fires with the reason; fill callback fires with px/qty.
// -----------------------------------------------------------------------------
void test_callbacks_fire() {
    RiskEngine risk = makeRisk();

    int    fillCalls = 0;
    double lastFillPx = 0.0;
    long   lastFillQty = 0;
    uint64_t lastFillId = 0;

    int         rejectCalls = 0;
    std::string lastReason;

    FillCallback onFill = [&](const OrderRecord& r, double px, long q) {
        ++fillCalls; lastFillPx = px; lastFillQty = q; lastFillId = r.clOrdId;
    };
    RejectCallback onReject = [&](const OrderRecord&, const std::string& why) {
        ++rejectCalls; lastReason = why;
    };

    OrderManager oms(onFill, onReject, &risk);

    uint64_t id = oms.submit(req("AAPL", true, 100.0, 10, 1000));
    oms.onAck(id, 2000);
    oms.onFill(id, 101.5, 4, false);
    CHECK(fillCalls == 1);
    CHECK(lastFillPx == 101.5);
    CHECK(lastFillQty == 4);
    CHECK(lastFillId == id);

    uint64_t id2 = oms.submit(req("AAPL", true, 100.0, 10, 3000));
    oms.onReject(id2, "risk override");
    CHECK(rejectCalls == 1);
    CHECK(lastReason == "risk override");
    CHECK(oms.getOrder(id2).state == OrderState::REJECTED);
}

// -----------------------------------------------------------------------------
// Watchdog alerts on stale ACKNOWLEDGED orders but never changes their state
// (requirement: it logs/alerts only, does NOT cancel).
// -----------------------------------------------------------------------------
void test_watchdog_never_cancels() {
    RiskEngine risk = makeRisk();
    OrderManager oms(noFill, noReject, &risk);

    uint64_t id = oms.submit(req("AAPL", true, 100.0, 10, 0));
    oms.onAck(id, /*ackNs=*/0);

    // 1s stale threshold; now is 5s later -> stale. (Logs to stderr.)
    oms.runWatchdog(/*nowNs=*/5'000'000'000LL, /*staleThresholdMs=*/1000);
    CHECK(oms.getOrder(id).state == OrderState::ACKNOWLEDGED);

    // A fresh order within the threshold is fine and also untouched.
    uint64_t id2 = oms.submit(req("AAPL", true, 100.0, 10, 0));
    oms.onAck(id2, 4'900'000'000LL);
    oms.runWatchdog(5'000'000'000LL, 1000);  // only 100ms old -> not stale
    CHECK(oms.getOrder(id2).state == OrderState::ACKNOWLEDGED);
}

// -----------------------------------------------------------------------------
// getOrder throws on an unknown id.
// -----------------------------------------------------------------------------
void test_get_order_throws() {
    RiskEngine risk = makeRisk();
    OrderManager oms(noFill, noReject, &risk);
    bool threw = false;
    try { oms.getOrder(12345); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
}

}  // namespace

int main() {
    std::cout << "Running OrderManager tests...\n";

    RUN_TEST(test_happy_path);
    RUN_TEST(test_partial_then_cancel);
    RUN_TEST(test_risk_reject);
    RUN_TEST(test_no_risk_engine);
    RUN_TEST(test_avg_fill_price);
    RUN_TEST(test_invalid_transitions);
    RUN_TEST(test_cancel_guards);
    RUN_TEST(test_cancel_ack_respects_terminal);
    RUN_TEST(test_callbacks_fire);
    RUN_TEST(test_watchdog_never_cancels);
    RUN_TEST(test_get_order_throws);

    std::cout << "\n" << (g_checks - g_failures) << "/" << g_checks
              << " checks passed.\n";
    if (g_failures == 0) {
        std::cout << "ALL TESTS PASSED\n";
        return 0;
    }
    std::cout << g_failures << " CHECK(S) FAILED\n";
    return 1;
}
