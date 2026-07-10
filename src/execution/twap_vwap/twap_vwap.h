#pragma once

// ==========================================================================
// TWAP / VWAP Execution Algorithms
// ==========================================================================
// Parent-order slicers that sit between a strategy decision ("buy 10k AAPL")
// and the OMS. They emit child OrderRequest intents over time:
//
//   StrategyEngine  ->  TwapAlgo / VwapAlgo  ->  RiskEngine  ->  OrderManager
//                            (HERE)
//
// TWAP: equal-sized slices on a jittered time grid (anti-detection).
// VWAP: size each slice by a normalized historical volume profile.
//
// Both quote passively (buy at bid, sell at ask) so they rest on the book
// rather than crossing the spread.

#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "../../common/types.h"           // hft::OrderRequest, hft::Side
#include "../../strategy/strategy_engine.h"  // hft::BookUpdate

namespace hft {

class TwapAlgo {
public:
    // Slice `totalQty` into `slices` child orders between [startNs, endNs).
    TwapAlgo(std::string sym, Side side, long totalQty,
             int64_t startNs, int64_t endNs, int slices);

    // Fire the next child order when nowNs >= nextFireNs_. Returns nullopt
    // while waiting or when the parent is fully sliced.
    std::optional<OrderRequest> tick(int64_t nowNs, const BookUpdate& book);

    bool isDone() const;
    long remaining() const { return remaining_; }

private:
    int64_t jitteredInterval();

    std::string   sym_;
    Side          side_;
    long          remaining_;
    long          sliceQty_;
    int64_t       nextFireNs_;
    int64_t       baseInterval_;
    uint64_t      nextId_;
    std::mt19937_64 rng_;
};

class VwapAlgo {
public:
    // volumeProfile: {bucketStartNs, weight}. Weights are normalized in the
    // constructor so they need not sum to 1.0. The last bucket always drains
    // whatever quantity remains (avoids rounding leftover).
    VwapAlgo(std::string sym, Side side, long totalQty,
             int64_t startNs, int64_t endNs,
             std::vector<std::pair<int64_t, double>> volumeProfile);

    std::optional<OrderRequest> tick(int64_t nowNs, const BookUpdate& book);

    bool isDone() const;
    long remaining() const { return remaining_; }

private:
    std::string sym_;
    Side        side_;
    long        totalQty_;
    long        remaining_;
    int         curBucket_;
    uint64_t    nextId_;
    std::vector<std::pair<int64_t, double>> volProfile_;
};

}  // namespace hft
