#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>

#include "../../src/execution/fills_processor/fills_processor.h"

// =============================================================================
// Standalone unit tests for FillsProcessor (no external test framework).
//
// Build & run (one line):
//   g++ -std=c++17 -pthread tests/unit/test_fills_processor.cpp
//       src/execution/fills_processor/fills_processor.cpp -o test_fp && ./test_fp
//
// Exit code is 0 only if every check passes (CI-friendly).
// (Mirrors the harness used by test_risk_engine.cpp.)
//
// Each test uses its own scratch journal file and deletes it on entry/exit so
// runs are isolated and repeatable.
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

// A scratch journal path in the system temp directory (writable and cleanly
// removable regardless of where the test binary is run from). Removed on
// construction and destruction so the test starts empty and leaves nothing
// behind -- never litters the source tree.
struct ScratchLog {
    std::string path;
    explicit ScratchLog(const char* name) {
        path = (std::filesystem::temp_directory_path() / name).string();
        std::remove(path.c_str());
    }
    ~ScratchLog() { std::remove(path.c_str()); }
};

ExecReport fill(uint64_t id, const std::string& strat, const std::string& sym,
                bool buy, ExecType type, double px, double arrivalMid, long qty,
                bool maker) {
    ExecReport r{};
    r.clOrdId    = id;
    r.strategyId = strat;
    r.symbol     = sym;
    r.buy        = buy;
    r.type       = type;
    r.fillPx     = px;
    r.arrivalMid = arrivalMid;
    r.fillQty    = qty;
    r.isMaker    = maker;
    r.tsNs       = 1000;
    return r;
}

// -----------------------------------------------------------------------------
// A single full fill produces the right per-order summary.
// -----------------------------------------------------------------------------
void test_single_fill_summary() {
    ScratchLog s("test_fp_single.tmp");
    FillsProcessor fp(s.path);

    fp.onExecReport(fill(1, "alpha", "AAPL", true, EXEC_FULL_FILL, 100.0, 100.0, 10, false));

    FillSummary sum = fp.getSummary(1);
    CHECK(sum.clOrdId == 1);
    CHECK(sum.cumQty == 10);
    CHECK(approx(sum.avgFillPx, 100.0));
    CHECK(approx(sum.slippage, 0.0));  // filled exactly at arrival mid
}

// -----------------------------------------------------------------------------
// Multiple partial fills accumulate into a quantity-weighted average price.
//   (100*4 + 110*6) / 10 = 106.0
// -----------------------------------------------------------------------------
void test_weighted_avg_price() {
    ScratchLog s("test_fp_wavg.tmp");
    FillsProcessor fp(s.path);

    fp.onExecReport(fill(7, "alpha", "AAPL", true, EXEC_PARTIAL_FILL, 100.0, 100.0, 4, false));
    fp.onExecReport(fill(7, "alpha", "AAPL", true, EXEC_FULL_FILL,    110.0, 100.0, 6, false));

    FillSummary sum = fp.getSummary(7);
    CHECK(sum.cumQty == 10);
    CHECK(approx(sum.avgFillPx, 106.0));
}

// -----------------------------------------------------------------------------
// Slippage is signed so positive is always adverse, for both sides.
//   buy filled above arrival mid -> positive cost
//   sell filled below arrival mid -> positive cost
// -----------------------------------------------------------------------------
void test_slippage_sign() {
    ScratchLog s("test_fp_slip.tmp");
    FillsProcessor fp(s.path);

    // Buy: arrival 100, filled 100.5 -> +50 bps
    fp.onExecReport(fill(1, "alpha", "AAPL", true, EXEC_FULL_FILL, 100.5, 100.0, 10, false));
    CHECK(approx(fp.getSummary(1).slippage, 50.0));

    // Sell: arrival 100, filled 99.5 -> +50 bps (adverse)
    fp.onExecReport(fill(2, "alpha", "AAPL", false, EXEC_FULL_FILL, 99.5, 100.0, 10, false));
    CHECK(approx(fp.getSummary(2).slippage, 50.0));

    // Buy filled below arrival mid -> negative (favorable) slippage
    fp.onExecReport(fill(3, "alpha", "AAPL", true, EXEC_FULL_FILL, 99.0, 100.0, 10, false));
    CHECK(approx(fp.getSummary(3).slippage, -100.0));
}

// -----------------------------------------------------------------------------
// Fee model: taker pays, maker receives a rebate.
//   notional 1000, taker 0.03 bps -> +0.003 ; maker -0.01 bps -> -0.001
// -----------------------------------------------------------------------------
void test_fee_model() {
    ScratchLog s("test_fp_fee.tmp");
    FillsProcessor fp(s.path);

    fp.onExecReport(fill(1, "taker", "AAPL", true, EXEC_FULL_FILL, 100.0, 100.0, 10, false));
    CHECK(approx(fp.getPnL("taker").totalFees, 1000.0 * 0.03 / 10000.0));

    fp.onExecReport(fill(2, "maker", "AAPL", true, EXEC_FULL_FILL, 100.0, 100.0, 10, true));
    CHECK(approx(fp.getPnL("maker").totalFees, 1000.0 * -0.01 / 10000.0));
}

// -----------------------------------------------------------------------------
// PnL is signed cash flow: buys spend, sells receive. A flat round trip nets
// the realized profit.
// -----------------------------------------------------------------------------
void test_pnl_cashflow() {
    ScratchLog s("test_fp_pnl.tmp");
    FillsProcessor fp(s.path);

    fp.onExecReport(fill(1, "rt", "AAPL", true,  EXEC_FULL_FILL, 100.0, 100.0, 10, false)); // -1000
    fp.onExecReport(fill(2, "rt", "AAPL", false, EXEC_FULL_FILL, 101.0, 100.0, 10, false)); // +1010

    StrategyPnL p = fp.getPnL("rt");
    CHECK(p.fillCount == 2);
    CHECK(approx(p.realisedPnL, 10.0));  // flat round trip -> +10
}

// -----------------------------------------------------------------------------
// Non-fill reports (ack / cancel / reject) are journaled but do not move any
// statistics.
// -----------------------------------------------------------------------------
void test_non_fill_ignored() {
    ScratchLog s("test_fp_nofill.tmp");
    FillsProcessor fp(s.path);

    fp.onExecReport(fill(1, "alpha", "AAPL", true, EXEC_NEW_ACK,   0.0, 100.0, 0, false));
    fp.onExecReport(fill(1, "alpha", "AAPL", true, EXEC_CANCELLED, 0.0, 100.0, 0, false));
    fp.onExecReport(fill(2, "alpha", "AAPL", true, EXEC_REJECTED,  0.0, 100.0, 0, false));

    CHECK(fp.getSummary(1).cumQty == 0);
    CHECK(fp.getPnL("alpha").fillCount == 0);
    CHECK(approx(fp.getPnL("alpha").realisedPnL, 0.0));
}

// -----------------------------------------------------------------------------
// Unknown ids / strategies return zeroed records rather than throwing.
// -----------------------------------------------------------------------------
void test_unknown_lookups() {
    ScratchLog s("test_fp_unknown.tmp");
    FillsProcessor fp(s.path);

    FillSummary sum = fp.getSummary(999);
    CHECK(sum.clOrdId == 999);
    CHECK(sum.cumQty == 0);
    CHECK(approx(sum.avgFillPx, 0.0));

    StrategyPnL p = fp.getPnL("ghost");
    CHECK(p.id == "ghost");
    CHECK(p.fillCount == 0);
    CHECK(approx(p.realisedPnL, 0.0));
}

// -----------------------------------------------------------------------------
// Crash recovery: state written by one processor is rebuilt by a fresh one
// pointed at the same journal (simulates a restart after a crash).
// -----------------------------------------------------------------------------
void test_crash_recovery() {
    ScratchLog s("test_fp_recover.tmp");

    {
        FillsProcessor fp(s.path);
        fp.onExecReport(fill(1, "alpha", "AAPL", true,  EXEC_PARTIAL_FILL, 100.0, 100.0, 4, false));
        fp.onExecReport(fill(1, "alpha", "AAPL", true,  EXEC_FULL_FILL,    110.0, 100.0, 6, false));
        fp.onExecReport(fill(2, "beta",  "MSFT", false, EXEC_FULL_FILL,    200.0, 200.0, 5, true));
    }  // destructor closes/flushes the journal

    // Restart: a brand new processor replays the same file on construction.
    FillsProcessor recovered(s.path);

    FillSummary sum = recovered.getSummary(1);
    CHECK(sum.cumQty == 10);
    CHECK(approx(sum.avgFillPx, 106.0));

    StrategyPnL alpha = recovered.getPnL("alpha");
    CHECK(alpha.fillCount == 2);
    CHECK(approx(alpha.realisedPnL, -(100.0 * 4 + 110.0 * 6)));  // two buys

    StrategyPnL beta = recovered.getPnL("beta");
    CHECK(beta.fillCount == 1);
    CHECK(approx(beta.realisedPnL, +200.0 * 5));  // one sell
}

// -----------------------------------------------------------------------------
// replayLog is idempotent: calling it again does not double-count.
// -----------------------------------------------------------------------------
void test_replay_idempotent() {
    ScratchLog s("test_fp_idem.tmp");
    FillsProcessor fp(s.path);

    fp.onExecReport(fill(1, "alpha", "AAPL", true, EXEC_FULL_FILL, 100.0, 100.0, 10, false));

    const double pnlBefore = fp.getPnL("alpha").realisedPnL;
    const long   qtyBefore = fp.getSummary(1).cumQty;

    fp.replayLog(s.path);   // rebuild from scratch
    fp.replayLog(s.path);   // and again

    CHECK(fp.getSummary(1).cumQty == qtyBefore);
    CHECK(approx(fp.getPnL("alpha").realisedPnL, pnlBefore));
    CHECK(fp.getPnL("alpha").fillCount == 1);
}

// -----------------------------------------------------------------------------
// Precision round-trip: a price with many significant digits survives the
// journal write/replay exactly (this is what log_.precision(17) guarantees).
// -----------------------------------------------------------------------------
void test_precision_round_trip() {
    ScratchLog s("test_fp_prec.tmp");
    const double px = 12345.678901234567;

    {
        FillsProcessor fp(s.path);
        fp.onExecReport(fill(1, "alpha", "AAPL", true, EXEC_FULL_FILL, px, px, 1, false));
    }

    FillsProcessor recovered(s.path);
    FillSummary sum = recovered.getSummary(1);
    CHECK(sum.cumQty == 1);
    // Exact equality: full-precision output must reproduce the same double.
    CHECK(sum.avgFillPx == px);
}

}  // namespace

int main() {
    std::cout << "Running FillsProcessor tests...\n";

    RUN_TEST(test_single_fill_summary);
    RUN_TEST(test_weighted_avg_price);
    RUN_TEST(test_slippage_sign);
    RUN_TEST(test_fee_model);
    RUN_TEST(test_pnl_cashflow);
    RUN_TEST(test_non_fill_ignored);
    RUN_TEST(test_unknown_lookups);
    RUN_TEST(test_crash_recovery);
    RUN_TEST(test_replay_idempotent);
    RUN_TEST(test_precision_round_trip);

    std::cout << "\n" << (g_checks - g_failures) << "/" << g_checks
              << " checks passed.\n";
    if (g_failures == 0) {
        std::cout << "ALL TESTS PASSED\n";
        return 0;
    }
    std::cout << g_failures << " CHECK(S) FAILED\n";
    return 1;
}
