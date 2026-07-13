// ==========================================================================
// apps/exchange_sim  --  runnable venue demo
// ==========================================================================
// Seeds a two-sided book, publishes top-of-book, then fires an aggressive order
// that crosses several levels and prints the resulting fills + book. This is the
// exchange side of the loop: real matching, real exec reports, real market data.

#include <iostream>
#include <string>

#include "exchange_sim.h"

using namespace hft;

namespace {

long countType(const std::vector<MatchExecReport>& reps, const std::string& type) {
    long n = 0;
    for (const auto& r : reps)
        if (r.type == type) ++n;
    return n;
}

void printTop(const ExchangeSim& sim, int64_t ts, const char* label) {
    const BookUpdate u = sim.topOfBook(ts);
    std::cout << label << " top-of-book: bid=" << u.bidPx << " x" << u.bidQty
              << "   ask=" << u.askPx << " x" << u.askQty
              << "   mid=" << u.mid() << "\n";
}

}  // namespace

int main() {
    ExchangeSim sim("AAPL");

    std::cout << "=== Exchange simulator (MatchingEngine venue) ===\n\n";

    // 1) Seed 5 levels of liquidity each side, 1c apart around 100.00.
    sim.seedBook(/*centerPx=*/100.00, /*levels=*/5, /*qtyPerLevel=*/200, /*tick=*/0.01);
    printTop(sim, 1, "seeded  ");

    // 2) Aggressive BUY that sweeps several ask levels (100.01..100.03).
    NewOrderRequest aggressor;
    aggressor.clOrdId = sim.nextId();
    aggressor.symbol  = "AAPL";
    aggressor.buy     = true;
    aggressor.type    = OrdType::LIMIT;
    aggressor.limitPx = 100.03;   // crosses asks at 100.01, 100.02, 100.03
    aggressor.qty     = 500;      // 200 + 200 + 100 -> sweeps 2.5 levels

    const auto reports = sim.submit(aggressor);

    std::cout << "\naggressive BUY 500 @ 100.03 ->\n";
    std::cout << "  fills:        " << countType(reports, "FILL")
              << "\n  partials:     " << countType(reports, "PARTIAL_FILL")
              << "\n  total reports: " << reports.size() << "\n\n";

    long makerFilled = 0, takerFilled = 0;
    for (const auto& r : reports) {
        if (r.type == "FILL" || r.type == "PARTIAL_FILL") {
            (r.isMaker ? makerFilled : takerFilled) += r.fillQty;
            if (r.isMaker)
                std::cout << "  maker fill: id=" << r.clOrdId
                          << " px=" << r.fillPx << " qty=" << r.fillQty << "\n";
        }
    }
    std::cout << "  taker filled qty total: " << takerFilled << "\n";

    // 3) Book after the sweep.
    printTop(sim, 2, "\nafter   ");

    std::cout << "\n=== Loop closed: orders matched, book updated, MD published ===\n";
    return 0;
}
