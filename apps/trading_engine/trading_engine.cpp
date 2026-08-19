// ==========================================================================
// apps/trading_engine  --  composition-root demo
// ==========================================================================
// Wires every component via TradingEngine and drives a small scenario:
//   1. Seed the local book from the "feed".
//   2. Tick the strategy (feed -> book -> strategy -> risk -> OMS -> exchange).
//   3. Simulate an exchange fill to exercise the full fill path
//      (OMS -> FillsProcessor + PositionTracker + RiskEngine).
//
// Build (MSVC):
//   cl /EHsc /std:c++17 /Fe:trading_engine.exe ^
//     apps\trading_engine\trading_engine.cpp ^
//     src\risk\risk_engine.cpp ^
//     src\strategy\strategy_engine.cpp ^
//     src\marketdata\book_builder\order_book_builder.cpp ^
//     src\execution\order_manager\order_manager.cpp ^
//     src\execution\fills_processor\fills_processor.cpp ^
//     src\portfolio\position_tracker.cpp ^
//     src\core\matching_engine\matching_engine.cpp
//
// Build (g++ 7+):
//   g++ -std=c++17 apps/trading_engine/trading_engine.cpp \
//     src/risk/risk_engine.cpp src/strategy/strategy_engine.cpp \
//     src/marketdata/book_builder/order_book_builder.cpp \
//     src/execution/order_manager/order_manager.cpp \
//     src/execution/fills_processor/fills_processor.cpp \
//     src/portfolio/position_tracker.cpp \
//     src/core/matching_engine/matching_engine.cpp \
//     -o trading_engine && ./trading_engine

#include <iostream>
#include <memory>

#include "../../src/common/timing.h"
#include "trading_engine.h"

using namespace hft;

int main() {
    // Once, before anything timestamps. Until this runs, now_ns() returns a raw
    // cycle count rather than nanoseconds -- still monotonic, but wrong units.
    TscClock::calibrate();

    TradingEngineConfig cfg;
    cfg.symbol     = "AAPL";
    cfg.strategyId = "momentum1";

    // Heap-allocated: MatchingEngine embeds a large order slab (too big for the stack).
    auto engine = std::make_unique<TradingEngine>(cfg);

    std::cout << "=== Trading engine wired. Driving demo scenario ===\n\n";

    // 1) Seed a two-sided book from the "feed".
    engine->onFeedAdd(1, Side::BUY,  99.90, 500);
    engine->onFeedAdd(2, Side::SELL, 100.10, 300);

    // 2) Warm up + tick the strategy. Nudge the book each step so signals move.
    uint64_t lastClOrdId = 0;
    for (int i = 0; i < 40; ++i) {
        const double drift = 0.01 * i;
        engine->onFeedCancel(2, 0);  // no-op reduce keeps level; refreshes mid
        engine->onFeedAdd(100 + i, Side::BUY,  99.90 + drift, 400);
        engine->onFeedAdd(200 + i, Side::SELL, 100.10 + drift, 250);

        const uint64_t clOrdId = engine->onBookTick();
        if (clOrdId != 0) {
            lastClOrdId = clOrdId;
            std::cout << "[strategy] emitted + risk-approved order clOrdId="
                      << clOrdId << " (routed to exchange seam)\n";
        }
    }
    if (lastClOrdId == 0)
        std::cout << "[strategy] no signal fired this run (that is fine — "
                     "wiring is what we are demonstrating)\n";

    // 3) Demonstrate the full fill path with a simulated exchange fill.
    //    (Real fills will arrive here once MatchingEngine::submitOrder is done.)
    const uint64_t demoId = engine->oms().submit(
        OmsOrderRequest{cfg.symbol, true, 100.10, 100, TscClock::now_ns()});
    if (demoId != 0) {
        std::cout << "\n[demo] simulating exchange fill for clOrdId=" << demoId << "\n";
        engine->simulateFill(demoId, 100.10, 100, /*isFull=*/true);
    }

    // 4) Read the downstream state that the fill propagated to.
    const Position pos = engine->positions().getPosition(cfg.symbol);
    const StrategyPnL pnl = engine->fills().getPnL(cfg.strategyId);

    std::cout << "\n=== Post-fill state ===\n";
    std::cout << "position " << pos.symbol << " netQty=" << pos.netQty
              << " avgCost=" << pos.avgCostPx << "\n";
    std::cout << "strategy " << pnl.id << " fills=" << pnl.fillCount
              << " realisedPnL=" << pnl.realisedPnL
              << " fees=" << pnl.totalFees << "\n";

    const BookSnapshot snap = engine->exchange().getBook(cfg.symbol);
    std::cout << "exchange book levels: bids=" << snap.bids.size()
              << " asks=" << snap.asks.size()
              << "  (matching engine live; routed orders now rest/match here)\n";

    std::cout << "\n=== Done. All components constructed and connected. ===\n";
    return 0;
}
