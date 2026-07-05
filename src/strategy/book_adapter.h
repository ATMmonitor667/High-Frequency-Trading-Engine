#pragma once

// ==========================================================================
// Book Adapter  (OrderBookBuilder -> StrategyEngine bridge)
// ==========================================================================
// The StrategyEngine consumes a BookUpdate (top-of-book snapshot), but the
// upstream OrderBookBuilder exposes getLevel()/isStale() instead. This header
// bridges the gap so apps/trading_engine can wire the two together without the
// engine depending on the builder's internals.
//
//   FeedHandler -> OrderBookBuilder -> [book_adapter] -> StrategyEngine
//
// Two responsibilities:
//   1. Read the best bid/ask into a BookUpdate.
//   2. Refuse to trade while the book is stale (FeedHandler reported a gap) or
//      one side is empty (getLevel returns NaN price).

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>

#include "strategy_engine.h"
#include "../marketdata/book_builder/order_book_builder.h"

namespace hft {

// Snapshot the best bid/ask from the builder into a BookUpdate. Returns nullopt
// when the book is stale or either side is empty (no tradeable top-of-book).
inline std::optional<BookUpdate> topOfBook(const OrderBookBuilder& book,
                                           const std::string& symbol,
                                           int64_t tsNs) {
    if (book.isStale())
        return std::nullopt;

    const auto [bidPx, bidQty] = book.getLevel(Side::BUY, 0);
    const auto [askPx, askQty] = book.getLevel(Side::SELL, 0);
    if (std::isnan(bidPx) || std::isnan(askPx))
        return std::nullopt;  // one-sided book, nothing to quote against

    BookUpdate u;
    u.symbol = symbol;
    u.bidPx  = bidPx;
    u.askPx  = askPx;
    u.bidQty = bidQty;
    u.askQty = askQty;
    u.tsNs   = tsNs;
    return u;
}

// Convenience driver: pull top-of-book, feed the engine, return any order intent.
// This is the single call apps/trading_engine makes per market-data event.
inline std::optional<OrderRequest> onBook(StrategyEngine& engine,
                                          const OrderBookBuilder& book,
                                          const std::string& symbol,
                                          int64_t tsNs) {
    auto u = topOfBook(book, symbol, tsNs);
    if (!u)
        return std::nullopt;
    return engine.onBookUpdate(*u);
}

}  // namespace hft
