#pragma once

// ==========================================================================
// ExchangeSim  --  a runnable trading venue built on the MatchingEngine
// ==========================================================================
// Wraps the authoritative MatchingEngine and gives it the two things a venue
// needs to close the loop with the trading engine:
//   1. an order-entry surface  (submit)      -- accept orders, return fills
//   2. a market-data surface   (topOfBook)   -- publish the resulting book
//
//   trading_engine  --orders-->  [ExchangeSim -> MatchingEngine]  --fills-->
//                   <--market data (topOfBook / BookUpdate)-------
//
// MatchingEngine embeds a large order slab, so it is heap-owned here.

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../../src/core/matching_engine/matching_engine.h"
#include "../../src/strategy/strategy_engine.h"  // hft::BookUpdate

namespace hft {

class ExchangeSim {
public:
    explicit ExchangeSim(std::string symbol)
        : symbol_(std::move(symbol)), me_(std::make_unique<MatchingEngine>()) {}

    // Order entry: forward to the matching engine, return its exec reports.
    std::vector<MatchExecReport> submit(const NewOrderRequest& req) {
        return me_->submitOrder(req);
    }

    // Venue-assigned order id (for synthetic flow).
    uint64_t nextId() { return nextId_++; }

    // Seed resting liquidity: `levels` bids below and `levels` asks above
    // centerPx, one tick apart, qtyPerLevel each. Gives aggressors something
    // to trade against.
    void seedBook(double centerPx, int levels, long qtyPerLevel, double tick) {
        for (int i = 1; i <= levels; ++i) {
            NewOrderRequest bid;
            bid.clOrdId = nextId_++;
            bid.symbol  = symbol_;
            bid.buy     = true;
            bid.type    = OrdType::LIMIT;
            bid.limitPx = centerPx - i * tick;
            bid.qty     = qtyPerLevel;
            me_->submitOrder(bid);

            NewOrderRequest ask;
            ask.clOrdId = nextId_++;
            ask.symbol  = symbol_;
            ask.buy     = false;
            ask.type    = OrdType::LIMIT;
            ask.limitPx = centerPx + i * tick;
            ask.qty     = qtyPerLevel;
            me_->submitOrder(ask);
        }
    }

    // Publish top-of-book as a strategy-consumable BookUpdate.
    BookUpdate topOfBook(int64_t tsNs) const {
        BookUpdate u;
        u.symbol = symbol_;
        u.tsNs   = tsNs;
        const BookSnapshot snap = me_->getBook(symbol_, 1);
        if (!snap.bids.empty()) { u.bidPx = snap.bids[0].first; u.bidQty = snap.bids[0].second; }
        if (!snap.asks.empty()) { u.askPx = snap.asks[0].first; u.askQty = snap.asks[0].second; }
        return u;
    }

    MatchingEngine&       engine()       { return *me_; }
    const MatchingEngine& engine() const { return *me_; }
    const std::string&    symbol() const { return symbol_; }

private:
    std::string                     symbol_;
    std::unique_ptr<MatchingEngine> me_;
    uint64_t                        nextId_ = 1;
};

}  // namespace hft
