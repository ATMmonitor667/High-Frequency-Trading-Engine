#pragma once

// ==========================================================================
// Strategy Engine  (alpha signals -> order intents, zero-alloc hot path)
// ==========================================================================
// Concepts: ring buffers, no-alloc hot path, alpha signals, feature engineering
//
// The Strategy Engine is the "brain" of the trading system (PROJECT_PLAN
// Phase 5). On every top-of-book update it recomputes a set of alpha signals,
// combines them into a directional score in [-1, +1], and when the score
// crosses a threshold emits an OrderRequest for the Order Gateway.
//
// PIPELINE (where this file sits):
//
//   Exchange Sim
//     -> UDP bytes
//         -> FeedHandler (marketdata/feed_handler)     decode + gap detect
//             -> OrderBookBuilder (marketdata/book_builder)   apply -> L2 depth
//                 -> StrategyEngine (HERE)             signals -> order intents
//                     -> OrderGateway (execution/)     send + acks/fills
//
// The engine is zero-allocation in the hot path -- no heap operations after
// construction. All state lives in pre-allocated ring buffers.
//
// FEEDING THE ENGINE:
//   The engine consumes a BookUpdate (top-of-book snapshot). The upstream
//   OrderBookBuilder exposes getLevel()/isStale() rather than a BookUpdate, so
//   book_adapter.h bridges the two: it reads the best bid/ask into a BookUpdate
//   and refuses to trade while the book is stale. See book_adapter.h.

#include <cstdint>
#include <optional>
#include <string>

#include "../common/types.h"  // hft::Side, hft::OrderRequest

namespace hft {

static constexpr int kMaxLookback = 256;

// Signal weights + firing parameters. Weights must sum to 1.0 (asserted in the
// constructor).
struct EngineConfig {
    int    lookback   = 20;    // ring buffer depth
    double wMomentum  = 0.4;
    double wImbalance = 0.3;
    double wVwapDev   = 0.2;
    double wSpread    = 0.1;
    double threshold  = 0.3;   // abs(score) required to fire
    int    cooldownMs = 100;   // min ms between orders
    long   orderQty   = 100;
};

// One top-of-book snapshot handed to the engine. Built either directly (tests)
// or from an OrderBookBuilder via book_adapter.h.
struct BookUpdate {
    std::string symbol;
    double  bidPx = 0.0, askPx = 0.0;
    long    bidQty = 0, askQty = 0;
    int64_t tsNs = 0;
    double  mid() const { return (bidPx + askPx) * 0.5; }
};

// OrderRequest (the emitted order intent) is the shared type from common/types.h
// so it can flow unchanged into the RiskEngine and OrderGateway. side==BUY when
// the score crosses +threshold, side==SELL when it crosses -threshold.

// Fixed-size circular array. push() overwrites the oldest entry once full;
// operator[](0) is the oldest, operator[](count-1) the newest. No heap.
template <typename T, int N>
struct RingBuffer {
    T   data[N]{};
    int head     = 0;
    int count    = 0;
    int capacity = N;

    void setCapacity(int cap) {
        capacity = cap;
    }

    void push(T v) {
        data[head] = v;
        head = (head + 1) % N;
        if (count < capacity)
            ++count;
    }

    T operator[](int i) const {
        // 0 = oldest, count-1 = newest
        return data[(head - count + i + N) % N];
    }

    bool full() const { return count >= capacity; }
    int  size() const { return count; }
};

class StrategyEngine {
public:
    explicit StrategyEngine(EngineConfig cfg);

    // Push the update into the ring buffers, recompute signals, and return an
    // OrderRequest iff |score| > threshold AND cooldown has expired. Returns
    // nullopt while warming up (buffers not yet full) or on no-trade.
    std::optional<OrderRequest> onBookUpdate(const BookUpdate& u);

    double computeMomentum()  const;  // mid[now] - mid[now-N]
    double computeImbalance() const;  // (bidQty - askQty) / (bidQty + askQty)
    double computeVwapDev()   const;  // (mid - vwap) / vwap
    double computeSpread()    const;  // (ask - bid) / mid * 10000  (bps)

private:
    void pushTick_(const BookUpdate& u);

    EngineConfig cfg_;
    RingBuffer<double, kMaxLookback> midBuf_;
    RingBuffer<long, kMaxLookback>   bidQtyBuf_;
    RingBuffer<long, kMaxLookback>   askQtyBuf_;
    BookUpdate lastUpdate_{};
    double     vwapNum_       = 0.0;  // running Sum(mid * vol) over the window
    double     vwapDen_       = 0.0;  // running Sum(vol) over the window
    int64_t    lastOrderTsNs_ = 0;
    uint64_t   nextClientOrderId_ = 1;  // stamped onto each emitted OrderRequest
};

}  // namespace hft
