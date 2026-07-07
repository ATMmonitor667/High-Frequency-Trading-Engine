#include "order_manager.h"

#include <iostream>
#include <stdexcept>

namespace hft {

OrderManager::OrderManager(FillCallback onFill, RejectCallback onReject, RiskEngine* risk)
    : onFill_(std::move(onFill))
    , onReject_(std::move(onReject))
    , risk_(risk)
{}

// --------------------------------------------------------------------------
// submit -- assign id, gate through risk, create PENDING_NEW record, enqueue.
// The atomic fetch_add is done without the mutex; the map/queue mutation is
// guarded. clOrdId is consumed even on rejection so ids stay monotonic.
// --------------------------------------------------------------------------
uint64_t OrderManager::submit(const OmsOrderRequest& req)
{
    const uint64_t clOrdId = nextId_.fetch_add(1);

    if (!risk_)
        return 0;

    const OrderRequest riskReq{
        clOrdId,
        req.symbol,
        req.buy ? Side::BUY : Side::SELL,
        req.px,
        req.qty,
        req.tsNs
    };

    const CheckResult riskResult = risk_->check(riskReq);
    if (riskResult.result == RiskResult::FAIL)
        return 0;

    const OrderRecord newRecord{
        clOrdId,
        OrderState::PENDING_NEW,
        req.symbol,
        req.buy,
        req.px,
        req.qty,
        0,
        0.0,
        req.tsNs,
        0
    };

    {
        std::lock_guard<std::mutex> lock(mu_);
        orders_[clOrdId] = newRecord;
        outbound_.push_back(newRecord);
    }

    return clOrdId;
}

// cancel -- request an exchange cancel; the actual state change lands in
// onCancelAck(). Returns false from terminal / non-working states.
bool OrderManager::cancel(uint64_t clOrdId)
{
    std::lock_guard<std::mutex> lock(mu_);

    auto it = orders_.find(clOrdId);
    if (it == orders_.end())
        return false;

    const OrderState s = it->second.state;
    if (s != OrderState::ACKNOWLEDGED && s != OrderState::PARTIALLY_FILLED)
        return false;

    cancelOutbound_.push_back(clOrdId);
    return true;
}

// onAck -- PENDING_NEW -> ACKNOWLEDGED. Records the ack timestamp so the
// watchdog can measure how long the order has been working.
void OrderManager::onAck(uint64_t clOrdId, int64_t ackNs)
{
    std::lock_guard<std::mutex> lock(mu_);

    auto it = orders_.find(clOrdId);
    if (it == orders_.end())
        return;

    if (it->second.state != OrderState::PENDING_NEW)
        return;

    it->second.state = OrderState::ACKNOWLEDGED;
    it->second.ackNs = ackNs;
}

// onFill -- update cumulative qty and running weighted-average fill price, then
// move to PARTIALLY_FILLED or FILLED. The callback fires while the lock is held
// (per spec); callbacks must not re-enter the OMS.
void OrderManager::onFill(uint64_t clOrdId, double fillPx, long fillQty, bool isFull)
{
    std::lock_guard<std::mutex> lock(mu_);

    auto it = orders_.find(clOrdId);
    if (it == orders_.end())
        return;

    const OrderState s = it->second.state;
    if (s != OrderState::ACKNOWLEDGED && s != OrderState::PARTIALLY_FILLED)
        return;

    const long   oldCum = it->second.cumFillQty;
    const double oldAvg = it->second.avgFillPx;

    it->second.cumFillQty = oldCum + fillQty;

    if (oldCum == 0)
        it->second.avgFillPx = fillPx;
    else
        it->second.avgFillPx =
            (oldAvg * static_cast<double>(oldCum) + fillPx * static_cast<double>(fillQty))
            / static_cast<double>(it->second.cumFillQty);

    it->second.state = isFull ? OrderState::FILLED : OrderState::PARTIALLY_FILLED;

    if (onFill_)
        onFill_(it->second, fillPx, fillQty);
}

// onCancelAck -- move any non-terminal order to CANCELLED.
void OrderManager::onCancelAck(uint64_t clOrdId)
{
    std::lock_guard<std::mutex> lock(mu_);

    auto it = orders_.find(clOrdId);
    if (it == orders_.end())
        return;

    const OrderState s = it->second.state;
    if (s == OrderState::FILLED || s == OrderState::CANCELLED || s == OrderState::REJECTED)
        return;

    it->second.state = OrderState::CANCELLED;
}

// onReject -- PENDING_NEW -> REJECTED and notify the strategy.
void OrderManager::onReject(uint64_t clOrdId, const std::string& reason)
{
    std::lock_guard<std::mutex> lock(mu_);

    auto it = orders_.find(clOrdId);
    if (it == orders_.end())
        return;

    if (it->second.state != OrderState::PENDING_NEW)
        return;

    it->second.state = OrderState::REJECTED;

    if (onReject_)
        onReject_(it->second, reason);
}

OrderRecord OrderManager::getOrder(uint64_t clOrdId) const
{
    std::lock_guard<std::mutex> lock(mu_);
    auto it = orders_.find(clOrdId);
    if (it == orders_.end())
        throw std::runtime_error("order not found");
    return it->second;
}

// runWatchdog -- alert (never cancel) on orders parked in ACKNOWLEDGED past the
// stale threshold. O(N) over all orders; called infrequently.
void OrderManager::runWatchdog(int64_t nowNs, int staleThresholdMs)
{
    std::lock_guard<std::mutex> lock(mu_);
    const int64_t thresholdNs = static_cast<int64_t>(staleThresholdMs) * 1'000'000LL;

    for (auto& kv : orders_) {
        OrderRecord& rec = kv.second;
        if (rec.state != OrderState::ACKNOWLEDGED)
            continue;
        if ((nowNs - rec.ackNs) > thresholdNs) {
            std::cerr << "WATCHDOG: stale order clOrdId=" << rec.clOrdId << '\n';
        }
    }
}

}  // namespace hft
