#pragma once

// ==========================================================================
// Order Management System (OMS)  --  central registry of all orders
// ==========================================================================
// The OMS assigns unique client order ids, routes orders through the pre-trade
// RiskEngine, tracks every state transition from creation to a terminal state,
// and fires callbacks to the strategy on fills and rejects:
//
//   StrategyEngine -> [OrderManager (HERE)] -> RiskEngine -> OrderGateway -> exchange
//                             ^                                     |
//                             \------ onAck / onFill / onCancelAck / onReject
//
// It is called from the strategy thread (submit/cancel) and the gateway thread
// (execution reports), so it is fully thread-safe: one mutex guards the order
// map and the outbound stub queues; the id counter is a lock-free atomic.
//
// ORDER STATE MACHINE
//   NEW -> PENDING_NEW -> ACKNOWLEDGED -> PARTIALLY_FILLED -> FILLED (terminal)
//                      \-> REJECTED (terminal)              \-> CANCELLED (terminal)
//   ACKNOWLEDGED / PARTIALLY_FILLED -> CANCELLED (when a cancel is acknowledged)

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "../../common/timing.h"     // hft::TscClock
#include "../../common/types.h"      // hft::Side
#include "../../risk/risk_engine.h"  // hft::RiskEngine, hft::OrderRequest, hft::RiskResult

namespace hft {

enum class OrderState {
    NEW,
    PENDING_NEW,
    ACKNOWLEDGED,
    PARTIALLY_FILLED,
    FILLED,
    CANCELLED,
    REJECTED
};

struct OrderRecord {
    uint64_t    clOrdId;
    OrderState  state;
    std::string symbol;
    bool        buy;
    double      px;
    long        qty;
    long        cumFillQty = 0;
    double      avgFillPx  = 0;
    int64_t     submitNs;
    int64_t     ackNs      = 0;
};

// Order intent handed to the OMS by the strategy. Kept distinct from
// hft::OrderRequest (the risk/gateway wire type) because the OMS assigns the
// clientOrderId itself; the strategy does not supply one.
// tsNs is the decision timestamp (TscClock::now_ns()). Leave it 0 and submit()
// stamps it at the order-entry boundary -- see the note there for why a
// permanently-zero tsNs breaks the risk engine's rate limiter and dedupe ring.
struct OmsOrderRequest {
    std::string symbol;
    bool        buy;
    double      px;
    long        qty;
    int64_t     tsNs = 0;
};

using FillCallback   = std::function<void(const OrderRecord&, double fillPx, long fillQty)>;
using RejectCallback = std::function<void(const OrderRecord&, const std::string& reason)>;

class OrderManager {
public:
    OrderManager(FillCallback onFill, RejectCallback onReject, RiskEngine* risk);

    // Assign a clOrdId, run risk checks, create a PENDING_NEW record and push to
    // the gateway queue. Returns 0 if risk rejects (or no risk engine wired).
    uint64_t submit(const OmsOrderRequest& req);

    // Request cancellation of a working order. Valid only from ACKNOWLEDGED or
    // PARTIALLY_FILLED. Returns false for terminal / non-cancellable states.
    bool cancel(uint64_t clOrdId);

    // Execution reports from the gateway thread.
    void onAck(uint64_t clOrdId, int64_t ackNs);   // PENDING_NEW -> ACKNOWLEDGED
    void onFill(uint64_t clOrdId, double fillPx, long fillQty, bool isFull);
    void onCancelAck(uint64_t clOrdId);            // any non-terminal -> CANCELLED
    void onReject(uint64_t clOrdId, const std::string& reason);  // PENDING_NEW -> REJECTED

    OrderRecord getOrder(uint64_t clOrdId) const;  // O(1); throws if not found

    // Called periodically; logs (does NOT cancel) orders stuck in ACKNOWLEDGED
    // for longer than staleThresholdMs without a fill.
    void runWatchdog(int64_t nowNs, int staleThresholdMs);

private:
    mutable std::mutex mu_;
    std::unordered_map<uint64_t, OrderRecord> orders_;
    std::atomic<uint64_t> nextId_{1};
    FillCallback   onFill_;
    RejectCallback onReject_;
    RiskEngine*    risk_;

    // Outbound work waiting for the gateway thread (stub queues for now).
    std::vector<OrderRecord> outbound_;
    std::vector<uint64_t>    cancelOutbound_;
};

}  // namespace hft
