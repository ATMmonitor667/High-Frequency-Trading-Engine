#include "risk_engine.h"

#include <algorithm>
#include <cmath>

namespace hft {

// Project the net position after this order and reject if it breaches the limit
// on either side (long or short). Uses the live position map, so pending fills
// applied via onFill() are already reflected.
CheckResult PositionCheck::check(const OrderRequest& order) {
    long cur = 0;
    if (positions_) {
        auto it = positions_->find(order.symbol);
        if (it != positions_->end())
            cur = it->second;
    }

    const long delta     = (order.side == Side::BUY) ? order.qty : -order.qty;
    const long projected = cur + delta;

    if (std::abs(projected) > positionLimit_) {
        return {RiskResult::FAIL,
                "PositionLimit: " + std::to_string(projected) + " > " +
                    std::to_string(positionLimit_)};
    }
    return {RiskResult::PASS, ""};
}

// Reject when the order's USD value (price * qty) exceeds the per-order cap.
CheckResult NotionalCheck::check(const OrderRequest& order) {
    const double notional = order.px * static_cast<double>(order.qty);
    if (notional > notionalLimit_) {
        return {RiskResult::FAIL,
                "Notional: " + std::to_string(notional) + " > " +
                    std::to_string(notionalLimit_)};
    }
    return {RiskResult::PASS, ""};
}

// Token bucket: top up tokens for the time elapsed since the last order (capped
// at the bucket size), then spend one. An empty bucket means we are over the
// configured orders-per-second and the order is rejected.
CheckResult RateCheck::check(const OrderRequest& order) {
    if (lastRefillNs_ != 0) {
        const double elapsed =
            static_cast<double>(order.tsNs - lastRefillNs_) / 1'000'000'000.0;
        tokens_ = std::min(tokens_ + rate_ * elapsed, maxTokens_);
    } else {
        tokens_ = maxTokens_;  // first order ever: start with a full bucket
    }
    lastRefillNs_ = order.tsNs;

    if (tokens_ < 1.0)
        return {RiskResult::FAIL, "RateLimit: exceeded"};

    tokens_ -= 1.0;  // each order costs one token
    return {RiskResult::PASS, ""};
}

// Trip once the day's realized loss passes the threshold (dailyPnL is negative
// when losing). Once tripped it blocks every order until reset() is called.
CheckResult CircuitBreaker::check(const OrderRequest&) {
    if (dailyPnL_ && *dailyPnL_ < -maxDailyLoss_)
        tripped_ = true;

    if (tripped_)
        return {RiskResult::FAIL, "CircuitBreaker: halted"};

    return {RiskResult::PASS, ""};
}

void CircuitBreaker::reset() { tripped_ = false; }

// Expire ids older than the dedupe window, reject if this id is still live in
// the window, otherwise record it. The ring is fixed-size, so no allocation.
CheckResult DuplicateCheck::check(const OrderRequest& order) {
    const int64_t windowNs = static_cast<int64_t>(dedupeWindowMs_) * 1'000'000LL;

    // Drop entries that have aged out of the window (oldest first).
    while (count_ > 0) {
        const int oldest = (head_ - count_ + kRingSize) % kRingSize;
        if (order.tsNs - ring_[oldest].tsNs <= windowNs)
            break;
        --count_;
    }

    // Scan the live window for a matching id.
    for (int i = 0; i < count_; ++i) {
        const int idx = (head_ - count_ + i + kRingSize) % kRingSize;
        if (ring_[idx].id == order.clientOrderId) {
            return {RiskResult::FAIL,
                    "Duplicate: id=" + std::to_string(order.clientOrderId)};
        }
    }

    // Record this id and advance the ring head.
    ring_[head_] = {order.clientOrderId, order.tsNs};
    head_ = (head_ + 1) % kRingSize;
    if (count_ < kRingSize)
        ++count_;

    return {RiskResult::PASS, ""};
}

// Copy limits into each node, point the state-reading nodes at the engine's
// live position/pnl, then link the nodes into a single chain.
RiskEngine::RiskEngine(RiskConfig cfg) : cfg_(cfg) {
    pos_.positionLimit_      = cfg_.positionLimit;
    pos_.positions_          = &position_;
    notional_.notionalLimit_ = cfg_.notionalLimit;
    rate_.rate_              = cfg_.ordersPerSec;
    rate_.maxTokens_         = cfg_.ordersPerSec;
    rate_.tokens_            = cfg_.ordersPerSec;
    breaker_.maxDailyLoss_   = cfg_.maxDailyLoss;
    breaker_.dailyPnL_       = &dailyPnL_;
    dup_.dedupeWindowMs_     = cfg_.dedupeWindowMs;

    pos_.next_      = &notional_;
    notional_.next_ = &rate_;
    rate_.next_     = &breaker_;
    breaker_.next_  = &dup_;
    dup_.next_      = nullptr;
    head_           = &pos_;
}

// Walk the chain; the first node that fails short-circuits the rest.
CheckResult RiskEngine::check(const OrderRequest& order) {
    for (RiskCheck* c = head_; c != nullptr; c = c->next_) {
        CheckResult r = c->check(order);
        if (r.result == RiskResult::FAIL)
            return r;
    }
    return {RiskResult::PASS, ""};
}

// Update pnl (for the circuit breaker) and net position (for PositionCheck).
void RiskEngine::onFill(const ExecFill& f) {
    dailyPnL_ += f.pnl;
    const long delta = (f.side == Side::BUY) ? f.qty : -f.qty;
    position_[f.symbol] += delta;
}

void RiskEngine::resetCircuitBreaker() { breaker_.reset(); }

}  // namespace hft
