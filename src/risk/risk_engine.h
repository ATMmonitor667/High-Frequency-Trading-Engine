#pragma once

// ==========================================================================
// Pre-Trade Risk Engine  (last gate before the exchange)
// ==========================================================================
// Every order the strategy emits must pass ALL checks synchronously before the
// OrderGateway sends it (PROJECT_PLAN Phase 5):
//
//   StrategyEngine -> [RiskEngine (HERE)] -> OrderGateway -> exchange
//
// Built as a Chain of Responsibility: each RiskCheck is a node that either
// passes the order to the next node or returns the first rejection. The chain
// is a raw-pointer linked list wired up in the RiskEngine constructor -- no
// std::list, no per-call allocation. check() returns PASS with an empty reason
// when every node approves.

#include <cstdint>
#include <string>
#include <unordered_map>

#include "../common/types.h"  // hft::OrderRequest, hft::Side

namespace hft {

// All risk limits, loaded once and handed to the chain nodes in the constructor.
struct RiskConfig {
    long   positionLimit  = 10000;
    double notionalLimit  = 1'000'000.0;
    double ordersPerSec   = 100.0;
    double maxDailyLoss   = 50'000.0;
    int    dedupeWindowMs = 5000;
};

// Execution-side fill reported back by the OrderGateway. Distinct from the
// matching-engine hft::Fill in types.h (maker/taker ids): this one carries the
// symbol, side and realized pnl the risk engine needs to update position and the
// daily loss used by the circuit breaker.
struct ExecFill {
    std::string symbol;
    Side        side;
    double      price;
    long        qty;
    double      pnl;
};

enum class RiskResult { PASS, FAIL };

// Result of a check. reason is only populated on FAIL, so the PASS path never
// constructs a string (keeps the hot path allocation-free).
struct CheckResult {
    RiskResult  result;
    std::string reason;
};

// Abstract chain node. next_ == nullptr marks the end of the chain.
struct RiskCheck {
    RiskCheck* next_ = nullptr;
    virtual CheckResult check(const OrderRequest&) = 0;
    virtual ~RiskCheck() = default;
};

// Rejects if the order would push net position past +/- positionLimit_. Reads
// live position from a map owned by RiskEngine (pointer, so it always sees the
// latest onFill updates). O(1).
struct PositionCheck : RiskCheck {
    long positionLimit_ = 0;
    const std::unordered_map<std::string, long>* positions_ = nullptr;
    CheckResult check(const OrderRequest&) override;
};

// Rejects if px * qty exceeds the per-order USD cap. O(1).
struct NotionalCheck : RiskCheck {
    double notionalLimit_ = 0.0;
    CheckResult check(const OrderRequest&) override;
};

// Token-bucket rate limiter. Refills tokens at rate_ per second based on the
// order timestamp, caps at maxTokens_, and spends one token per order. O(1).
struct RateCheck : RiskCheck {
    double  rate_         = 0.0;  // tokens per second (from ordersPerSec)
    double  maxTokens_    = 0.0;  // bucket capacity
    double  tokens_       = 0.0;  // tokens currently available
    int64_t lastRefillNs_ = 0;    // timestamp of the last refill
    CheckResult check(const OrderRequest&) override;
};

// Kill switch: once the day's loss exceeds maxDailyLoss_ it trips and rejects
// EVERY order until reset() is called manually. Reads pnl via a pointer into
// RiskEngine so it sees fills as they land. O(1).
struct CircuitBreaker : RiskCheck {
    double        maxDailyLoss_ = 0.0;
    const double* dailyPnL_     = nullptr;  // -> RiskEngine::dailyPnL_
    bool          tripped_      = false;
    CheckResult check(const OrderRequest&) override;
    void        reset();
};

// Rejects a clientOrderId already seen within dedupeWindowMs_. Recent ids live
// in a fixed ring of (id, timestamp); expired entries are dropped each call.
// O(W) where W is the number of live ids in the window (small constant).
struct DuplicateCheck : RiskCheck {
    static constexpr int kRingSize = 256;
    struct Entry { uint64_t id = 0; int64_t tsNs = 0; };

    int   dedupeWindowMs_ = 5000;
    Entry ring_[kRingSize]{};
    int   head_  = 0;
    int   count_ = 0;
    CheckResult check(const OrderRequest&) override;
};

class RiskEngine {
public:
    explicit RiskEngine(RiskConfig cfg);

    // Run the chain in order; return the first FAIL, or PASS if all approve.
    CheckResult check(const OrderRequest& order);

    // Apply an execution: update daily pnl and per-symbol position so later
    // checks see the new state.
    void onFill(const ExecFill& f);

    // Manually clear a tripped circuit breaker (operator intervention).
    void resetCircuitBreaker();

private:
    RiskConfig cfg_;
    std::unordered_map<std::string, long> position_;  // symbol -> net qty (+long/-short)
    double dailyPnL_ = 0.0;                            // running pnl for the day

    // Chain nodes, owned by value; wired into a linked list in the constructor.
    PositionCheck  pos_;
    NotionalCheck  notional_;
    RateCheck      rate_;
    CircuitBreaker breaker_;
    DuplicateCheck dup_;
    RiskCheck*     head_ = nullptr;  // first node in the chain
};

}  // namespace hft
