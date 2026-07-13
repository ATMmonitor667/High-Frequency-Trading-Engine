// ==========================================================================
// benchmarks/latency_bench  --  tick-to-trade latency of the wired hot path
// ==========================================================================
// Wires the ALREADY-COMPLETED components (no new logic) and measures them:
//   * Bench A: MatchingEngine order-entry latency (submitOrder, resting add)
//   * Bench B: tick-to-trade = StrategyEngine -> RiskEngine -> MatchingEngine
//              (market-data tick in  ->  order handed to the venue)
//
// Timestamps come from the TSC (src/common/latency.h); results are reported as
// p50/p90/p99/p99.9/max nanoseconds -- the percentiles HFT latency is judged on.

#include <iostream>
#include <memory>
#include <optional>

#include "../src/common/latency.h"
#include "../src/strategy/strategy_engine.h"
#include "../src/risk/risk_engine.h"
#include "../src/core/matching_engine/matching_engine.h"
#include "../src/core/matching_engine/matching_adapter.h"

using namespace hft;

int main() {
    TscClock::calibrate();
    std::cout << "=== HFT latency benchmark ===\n";
    std::cout << "TSC calibration: " << TscClock::cyclesPerNs() << " cycles/ns\n\n";

    // -----------------------------------------------------------------------
    // Bench A: MatchingEngine order-entry (resting LIMIT add, no cross).
    // -----------------------------------------------------------------------
    {
        auto engine = std::make_unique<MatchingEngine>();
        const int N = 50000;  // < slab capacity
        LatencyHistogram h("MatchingEngine.submitOrder (resting add)");
        h.reserve(N);

        for (int i = 0; i < N; ++i) {
            NewOrderRequest o;
            o.clOrdId = static_cast<uint64_t>(i + 1);
            o.symbol  = "AAPL";
            o.buy     = (i & 1) == 0;
            o.type    = OrdType::LIMIT;
            // Unique non-crossing prices: bids well below, asks well above.
            o.limitPx = o.buy ? (50.0 + (i % 1000) * 0.01)
                              : (150.0 + (i % 1000) * 0.01);
            o.qty     = 100;

            const uint64_t t0 = TscClock::rdtsc();
            volatile auto reports = engine->submitOrder(o);
            const uint64_t t1 = TscClock::rdtsc();
            (void)reports;
            h.record(TscClock::toNs(t1 - t0));
        }
        h.report();
        std::cout << "\n";
    }

    // -----------------------------------------------------------------------
    // Bench B: tick-to-trade (strategy decision -> risk gate -> venue submit).
    // -----------------------------------------------------------------------
    {
        EngineConfig cfg;      // weights sum to 1.0 by default
        cfg.threshold  = 0.0;  // fire on any non-zero score
        cfg.cooldownMs = 0;    // no cooldown suppression in the benchmark
        StrategyEngine strat(cfg);

        RiskConfig rc;
        rc.ordersPerSec = 1e12;   // don't let the rate limiter reject in-bench
        RiskEngine risk(rc);

        auto venue = std::make_unique<MatchingEngine>();

        // Warm up the strategy ring buffers.
        int64_t ts = 0;
        for (int i = 0; i < cfg.lookback + 2; ++i) {
            BookUpdate u;
            u.symbol = "AAPL";
            u.bidPx = 99.9 + (i % 3) * 0.01;
            u.askPx = 100.1 + (i % 3) * 0.01;
            u.bidQty = 400; u.askQty = 300;
            u.tsNs = (ts += 1'000'000);
            strat.onBookUpdate(u);
        }

        const int M = 100000;
        LatencyHistogram h("tick-to-trade (strategy -> risk -> venue)");
        h.reserve(M);

        long sent = 0;
        for (int i = 0; i < M; ++i) {
            BookUpdate u;
            u.symbol = "AAPL";
            // Oscillate the book so signals stay non-zero.
            const double drift = ((i % 7) - 3) * 0.01;
            u.bidPx = 99.90 + drift;
            u.askPx = 100.10 + drift;
            u.bidQty = 300 + (i % 5) * 20;
            u.askQty = 300 + (i % 3) * 20;
            u.tsNs = (ts += 1'000'000);

            const uint64_t t0 = TscClock::rdtsc();
            std::optional<OrderRequest> intent = strat.onBookUpdate(u);
            if (intent) {
                const CheckResult rr = risk.check(*intent);
                if (rr.result == RiskResult::PASS) {
                    // IOC so the venue book does not grow unbounded in the loop.
                    venue->submitOrder(fromOrderRequest(*intent, OrdType::IOC));
                    const uint64_t t1 = TscClock::rdtsc();
                    h.record(TscClock::toNs(t1 - t0));
                    ++sent;
                }
            }
        }

        std::cout << "orders sent: " << sent << " / " << M << " ticks\n";
        h.report();
    }

    std::cout << "\n=== done ===\n";
    return 0;
}
