#pragma once

// ==========================================================================
// Market Maker ↔ live system adapters
// ==========================================================================
// Concrete implementations of the MarketMaker's injection seams, backed by the
// real OrderManager and PositionTracker. This is what turns the MarketMaker
// from a unit-testable component into a live strategy:
//
//   MarketMaker --IOrderManager--> OmsQuoteRouter --> hft::OrderManager
//   MarketMaker --IPositionTracker--> TrackerInventory --> hft::PositionTracker
//
// Usage:
//   OmsQuoteRouter   router(&oms);
//   TrackerInventory inv(&positions);
//   MarketMaker mm(MMConfig{}, &router, &inv);
//   mm.onBookUpdate(bookUpdate);   // quotes flow through risk -> gateway

#include <cstdint>
#include <string>

#include "../../common/timing.h"
#include "../../execution/order_manager/order_manager.h"
#include "../../portfolio/position_tracker.h"
#include "market_maker.h"

namespace hft {

// Routes MarketMaker quotes into the real OMS (which runs risk + tracks state).
class OmsQuoteRouter : public IOrderManager {
public:
    explicit OmsQuoteRouter(OrderManager* oms) : oms_(oms) {}

    uint64_t submitOrder(MMOrder o) override {
        if (!oms_) return 0;
        // Stamp at the moment the quote decision leaves the strategy. The OMS
        // would stamp a zero for us, but this is the timestamp that actually
        // means something downstream -- it is what the risk engine's rate
        // limiter and dedupe window measure elapsed time against.
        return oms_->submit(
            OmsOrderRequest{o.symbol, o.buy, o.price, o.qty, TscClock::now_ns()});
    }
    void cancelOrder(uint64_t id) override {
        if (oms_ && id != 0) oms_->cancel(id);
    }

private:
    OrderManager* oms_ = nullptr;
};

// Feeds the MarketMaker live inventory from the real PositionTracker.
class TrackerInventory : public IPositionTracker {
public:
    explicit TrackerInventory(const PositionTracker* pos) : pos_(pos) {}

    long getNetQty(const std::string& sym) const override {
        return pos_ ? pos_->getPosition(sym).netQty : 0;
    }

private:
    const PositionTracker* pos_ = nullptr;
};

}  // namespace hft
