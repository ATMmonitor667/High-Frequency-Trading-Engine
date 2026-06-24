#pragma once

// ==========================================================================
// Order Book Builder  (incremental L2 reconstruction on the strategy side)
// ==========================================================================
// Concepts: incremental updates, orderId index, crossed-book detection, snapshots
//
// The Order Book Builder is the "memory" of the market on YOUR machine. It is
// the local MIRROR the FeedHandler feeds into (see PROJECT_PLAN Phase 4).
//
// PIPELINE (where this file sits):
//
//   Exchange Sim
//     -> UDP bytes
//         -> FeedHandler (marketdata/feed_handler)   decode + gap detect
//             -> OrderBookBuilder (HERE)             apply events -> L2 depth
//                 -> StrategyEngine (strategy/)      signals -> order intents
//
// The FeedHandler turns raw bytes into clean events (AddOrderMsg, CancelOrderMsg,
// TradeMsg) and hands them to a MessageCallback. The callback switches on the
// message type and calls onAdd / onCancel / onTrade here. This builder applies
// those events and maintains a live L2 book: for each price level, how much
// quantity rests on the bid and ask side.
//
// THREE DIFFERENT "ORDER BOOKS" IN THIS PROJECT (don't confuse them):
//
//   core/orderbook/LimitOrderBook   -- AUTHORITATIVE matching engine on the
//                                       exchange. Matches orders, owns FIFO
//                                       queues, produces fills.
//   book_builder/OrderBookBuilder   -- READ-ONLY REPLICA on the strategy side
//                                       (THIS FILE). Rebuilds depth from the
//                                       public feed. Never matches.
//
// This builder is simpler than LimitOrderBook: it stores TOTAL QTY PER PRICE
// LEVEL, not a FIFO queue of individual orders. The exchange already matched;
// we just mirror the published result.
//
// MENTAL PICTURE -- two parallel structures kept in sync:
//
//        orders_ (orderId index)              bids_ / asks_ (L2 depth)
//        ---------------------                -------------------------
//        id=1 -> {px=100, qty=10, buy}        bids_[100.0] = 10
//        id=2 -> {px=101, qty=5,  sell}       asks_[101.0] = 5
//        id=3 -> {px=100, qty=7,  buy}        bids_[100.0] = 17  (10+7)
//
//   onCancel(id=1, 10) -> erase id=1 from orders_, bids_[100] -= 10 -> erase
//
//   Why both?  orders_ gives O(1) lookup by orderId (cancel/modify know the
//   price without scanning the book). bids_/asks_ give O(1) best bid/ask and
//   O(N) depth.
//
// VOCABULARY:
//   INCREMENTAL  -- each message is a delta; events must be applied IN ORDER.
//   SNAPSHOT     -- full book dump from the exchange; used to reset after a gap.
//   CROSSED BOOK -- best_bid >= best_ask. Impossible in a healthy market; means
//                   stale data or a missed message.
//   STALE        -- flag set when the FeedHandler reports a gap. Do NOT trade
//                   until onSnapshot clears it.

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../../common/types.h"  // hft::Side

namespace hft {

// One resting order in the orderId index. Stored in orders_ so cancel/modify
// know the price without scanning the book. This is the builder's lightweight
// replica record -- NOT core/types.h's Order (which carries FIFO list pointers
// for the matching engine).
struct OrderEntry {
    uint64_t id;
    double   px;
    long     qty;
    bool     buy;  // true = bid, false = ask
};

class OrderBookBuilder {
public:
    explicit OrderBookBuilder(const std::string& symbol) : symbol_(symbol) {}

    // "A new resting order appeared on the feed." Index it, then add its qty to
    // the price level.
    void onAdd(uint64_t orderId, Side side, double px, long qty);

    // "An order was reduced or fully removed." Subtract cancelledQty from both
    // the orderId index and the price level; erase when either hits zero.
    void onCancel(uint64_t orderId, long cancelledQty);

    // "Order qty changed (same price/side)." Apply the delta to the level.
    void onModify(uint64_t orderId, long newQty);

    // "A match consumed liquidity at px." This feed variant carries no aggressor
    // flag, so the spec simplification is to reduce the ask side at px.
    void onTrade(double px, long qty);

    // "Full book reset after a gap." Clears all state, clears stale_, then
    // replays each entry through onAdd.
    void onSnapshot(const std::vector<OrderEntry>& orders);

    // Nth best level {price, totalQty}. n=0 is best (highest bid / lowest ask).
    // Returns {NaN, 0} when n is out of range.
    std::pair<double, long> getLevel(Side side, int n) const;

    // true iff BOTH sides non-empty AND best_bid >= best_ask.
    bool isCrossed() const;

    // FeedHandler gap signalling. Strategy checks isStale() and refuses to trade
    // until a fresh onSnapshot arrives.
    bool isStale() const { return stale_; }
    void markStale() { stale_ = true; }

private:
    std::string symbol_;

    // L2 depth: price -> aggregated resting qty at that level.
    std::map<double, long, std::greater<double>> bids_;  // best bid = begin()
    std::map<double, long, std::less<double>>    asks_;  // best ask = begin()

    // orderId -> full order record (for O(1) cancel/modify).
    std::unordered_map<uint64_t, OrderEntry> orders_;

    // true after a FeedHandler gap until onSnapshot recovers.
    bool stale_ = false;

    // Add delta qty to the price level on the given side; erase a level that
    // reaches zero. Shared by onAdd / onCancel / onModify / onTrade.
    void adjustLevel(Side side, double px, long delta);
};

}  // namespace hft
