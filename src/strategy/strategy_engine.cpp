#include "strategy_engine.h"

#include <cassert>
#include <cmath>

namespace hft {

StrategyEngine::StrategyEngine(EngineConfig cfg) : cfg_(cfg) {
    assert(cfg_.lookback > 0 && cfg_.lookback <= kMaxLookback);

    // Signal weights must sum to 1.0 so the combined score stays in [-1, 1].
    const double wSum = cfg_.wMomentum + cfg_.wImbalance + cfg_.wVwapDev + cfg_.wSpread;
    assert(std::abs(wSum - 1.0) < 1e-9);
    (void)wSum;  // silence unused warning in NDEBUG builds

    midBuf_.setCapacity(cfg_.lookback);
    bidQtyBuf_.setCapacity(cfg_.lookback);
    askQtyBuf_.setCapacity(cfg_.lookback);
}

// Maintain the VWAP sums incrementally: on each push, subtract the entry that is
// about to be evicted and add the new one. O(1) per tick.
void StrategyEngine::pushTick_(const BookUpdate& u) {
    if (midBuf_.full()) {
        const double oldMid = midBuf_[0];
        const double oldVol = static_cast<double>(bidQtyBuf_[0] + askQtyBuf_[0]);
        vwapNum_ -= oldMid * oldVol;
        vwapDen_ -= oldVol;
    }

    const double mid = u.mid();
    const double vol = static_cast<double>(u.bidQty + u.askQty);
    midBuf_.push(mid);
    bidQtyBuf_.push(u.bidQty);
    askQtyBuf_.push(u.askQty);
    vwapNum_ += mid * vol;
    vwapDen_ += vol;
}

std::optional<OrderRequest> StrategyEngine::onBookUpdate(const BookUpdate& u) {
    lastUpdate_ = u;
    pushTick_(u);

    if (!midBuf_.full())
        return std::nullopt;  // warming up

    const double score =
        cfg_.wMomentum  * computeMomentum()  +
        cfg_.wImbalance * computeImbalance() +
        cfg_.wVwapDev   * computeVwapDev()   +
        cfg_.wSpread    * computeSpread();

    if (std::abs(score) <= cfg_.threshold)
        return std::nullopt;

    const int64_t cooldownNs = static_cast<int64_t>(cfg_.cooldownMs) * 1'000'000LL;
    if (lastOrderTsNs_ != 0 && u.tsNs - lastOrderTsNs_ < cooldownNs)
        return std::nullopt;  // cooldown not yet expired

    lastOrderTsNs_ = u.tsNs;
    const bool buy = score > cfg_.threshold;
    return OrderRequest{
        nextClientOrderId_++,             // unique id for dedupe downstream
        u.symbol,
        buy ? Side::BUY : Side::SELL,
        buy ? u.askPx : u.bidPx,          // cross the spread to take liquidity
        cfg_.orderQty,
        u.tsNs};
}

double StrategyEngine::computeMomentum() const {
    if (midBuf_.size() < 2)
        return 0.0;
    return midBuf_[midBuf_.size() - 1] - midBuf_[0];
}

double StrategyEngine::computeImbalance() const {
    const long sum = lastUpdate_.bidQty + lastUpdate_.askQty;
    if (sum == 0)
        return 0.0;
    return static_cast<double>(lastUpdate_.bidQty - lastUpdate_.askQty) /
           static_cast<double>(sum);
}

double StrategyEngine::computeVwapDev() const {
    if (vwapDen_ == 0.0)
        return 0.0;
    const double vwap = vwapNum_ / vwapDen_;
    const double mid  = midBuf_[midBuf_.size() - 1];
    return (mid - vwap) / vwap;
}

double StrategyEngine::computeSpread() const {
    const double mid = lastUpdate_.mid();
    if (mid == 0.0)
        return 0.0;  // divide-by-zero guard
    return (lastUpdate_.askPx - lastUpdate_.bidPx) / mid * 10000.0;
}

}  // namespace hft
