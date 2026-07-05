#pragma once



#include <cstdint>
#include <optional>
#include <string>

#include "../common/types.h"  // hft::Side

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

// An order intent emitted by the engine. side==BUY when score crosses +threshold,
// side==SELL when it crosses -threshold. Consumed by the OrderGateway.
struct OrderRequest {
    std::string symbol;
    Side        side;
    double      px;
    long        qty;
};

// Fixed-size circular array. push() overwrites the oldest entry once full;
// operator[](0) is the oldest, operator[](count-1) the newest. No heap.
template <typename T, int N>
struct RingBuffer {
    T   data[N]{};
    int head     = 0;
    int count    = 0;
    int capacity = N;

    void setCapacity(int cap) {
        // assert lives in the header so template stays inline; see <cassert>
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
};

}  // namespace hft
