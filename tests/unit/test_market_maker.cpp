#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "../../src/strategy/market_maker/market_maker.h"

// =============================================================================
// Standalone unit tests for MarketMaker (no external framework).
// Adapted from the original MARKET_MAKER_TEST harness; uses hft::BookUpdate and
// hft::PositionFill (the project's canonical types) with mock injection seams.
//
// Build & run (MSVC):
//   cl /EHsc /std:c++17 /Fe:test_market_maker.exe tests\unit\test_market_maker.cpp
//
// Build & run (g++ 7+):
//   g++ -std=c++17 tests/unit/test_market_maker.cpp -o test_mm && ./test_mm
// =============================================================================

using namespace hft;

namespace {

struct MockOms : IOrderManager {
    struct Sent { uint64_t id; MMOrder o; };
    std::vector<Sent>     submitted;
    std::vector<uint64_t> cancelled;
    uint64_t nextId = 1;

    uint64_t submitOrder(MMOrder o) override {
        const uint64_t id = nextId++;
        submitted.push_back({id, o});
        return id;
    }
    void cancelOrder(uint64_t id) override { cancelled.push_back(id); }
};

struct MockPos : IPositionTracker {
    long qty = 0;
    long getNetQty(const std::string&) const override { return qty; }
};

bool approx(double a, double b, double eps = 1e-6) {
    return (a > b ? a - b : b - a) <= eps;
}

BookUpdate book(double bid, double ask, int64_t ts = 0) {
    BookUpdate u;
    u.symbol = "AAPL";
    u.bidPx = bid;
    u.askPx = ask;
    u.tsNs = ts;
    return u;
}

PositionFill fill(bool buy, double price, long qty, int64_t ts) {
    PositionFill f;
    f.symbol = "AAPL";
    f.buy = buy;
    f.price = price;
    f.qty = qty;
    f.tsNs = ts;
    return f;
}

int passed = 0, total = 0;
void check(const char* name, bool ok) {
    ++total; if (ok) ++passed;
    std::cout << name << ": " << (ok ? "PASS" : "FAIL") << '\n';
}

}  // namespace

int main() {
    // Example 1: flat inventory, symmetric quote around mid.
    {
        MockOms oms; MockPos pos; pos.qty = 0;
        MarketMaker mm(MMConfig{}, &oms, &pos);
        mm.onBookUpdate(book(99.9, 100.1));
        const auto st = mm.getStatus();
        check("Example 1 (symmetric quote)",
              st.bidPx && st.askPx &&
              approx(*st.bidPx, 99.95) && approx(*st.askPx, 100.05) &&
              approx(st.spreadBps, 10.0));
    }

    // Example 2: long inventory shades both quotes down.
    {
        MockOms oms; MockPos pos; pos.qty = 500;
        MarketMaker mm(MMConfig{}, &oms, &pos);
        mm.onBookUpdate(book(99.9, 100.1));
        const auto st = mm.getStatus();
        check("Example 2 (long inventory skew)",
              st.bidPx && st.askPx &&
              approx(*st.bidPx, 99.94, 1e-3) && approx(*st.askPx, 100.04, 1e-3));
    }

    // Example 3: max long inventory suppresses the bid.
    {
        MockOms oms; MockPos pos; pos.qty = 1000;
        MarketMaker mm(MMConfig{}, &oms, &pos);
        mm.onBookUpdate(book(99.9, 100.1));
        const auto st = mm.getStatus();
        check("Example 3 (max inventory, no bid)", !st.bidPx && st.askPx.has_value());
    }

    // Max short inventory suppresses the ask.
    {
        MockOms oms; MockPos pos; pos.qty = -1000;
        MarketMaker mm(MMConfig{}, &oms, &pos);
        mm.onBookUpdate(book(99.9, 100.1));
        const auto st = mm.getStatus();
        check("Max short (no ask)", st.bidPx.has_value() && !st.askPx);
    }

    // Requote threshold: a tiny mid move should NOT resubmit.
    {
        MockOms oms; MockPos pos; pos.qty = 0;
        MarketMaker mm(MMConfig{}, &oms, &pos);
        mm.onBookUpdate(book(99.9, 100.1));
        const size_t afterFirst = oms.submitted.size();
        mm.onBookUpdate(book(99.9001, 100.1001));
        check("Requote threshold suppresses churn",
              afterFirst == 2 && oms.submitted.size() == 2);
    }

    // Larger drift triggers a requote (cancel + resubmit).
    {
        MockOms oms; MockPos pos; pos.qty = 0;
        MarketMaker mm(MMConfig{}, &oms, &pos);
        mm.onBookUpdate(book(99.9, 100.1));
        mm.onBookUpdate(book(100.9, 101.1));
        check("Large drift requotes",
              oms.submitted.size() == 4 && oms.cancelled.size() == 2);
    }

    // Adverse selection: buy fill then mid drops -> cooldown widens spread x3.
    {
        MockOms oms; MockPos pos; pos.qty = 0;
        MMConfig cfg; cfg.adverseThreshBps = 10.0; cfg.adverseCooldownMs = 500;
        MarketMaker mm(cfg, &oms, &pos);
        mm.onBookUpdate(book(99.9, 100.1, 0));
        mm.onFill(fill(true, 99.95, 100, 1'000'000));
        mm.onBookUpdate(book(99.7, 99.9, 2'000'000));
        const auto st = mm.getStatus();
        const double effSpreadBps = (st.bidPx && st.askPx)
            ? (*st.askPx - *st.bidPx) / 99.8 * 10000.0 : 0.0;
        check("Adverse cooldown widens spread x3",
              st.adverseDetected && approx(effSpreadBps, 30.0, 1e-2));
    }

    // Fill accrues captured-spread PnL.
    {
        MockOms oms; MockPos pos; pos.qty = 0;
        MarketMaker mm(MMConfig{}, &oms, &pos);
        mm.onBookUpdate(book(99.9, 100.1));
        mm.onFill(fill(true, 99.95, 100, 1));
        check("Captured spread PnL", approx(mm.getStatus().realisedSpreadPnL, 5.0));
    }

    std::cout << "\n" << passed << "/" << total << " tests passed\n";
    return passed == total ? 0 : 1;
}
