#pragma once

// ==========================================================================
// TradingEngine  --  the wiring that connects every component
// ==========================================================================
// This is the "apps/trading_engine" composition root: it owns one instance of
// each component and connects them into the full topology from PROJECT_PLAN.
//
//   FeedHandler bytes ─▶ OrderBookBuilder ─▶ book_adapter ─▶ StrategyEngine
//                                                               │ OrderRequest
//                                                               ▼
//                                                          RiskEngine (in OMS)
//                                                               │ pass
//                                                               ▼
//                                        OrderManager ──▶ [exchange seam] ──▶ MatchingEngine
//                                             ▲                                    │ MatchExecReport
//                                             │ onAck/onFill                       │
//                                             └────────────────────────────────────┘
//                                                               │ fill callback
//                                              ┌────────────────┼─────────────────┐
//                                              ▼                ▼                 ▼
//                                       FillsProcessor   PositionTracker     RiskEngine
//                                       (journal + PnL)  (positions + PnL)   (position/pnl)
//
// STATUS OF SEAMS:
//   * MatchingEngine::submitOrder / onTrade are now fully implemented, so
//     routeToExchange() really rests/matches routed orders and returns fills.
//   * There is no real network gateway yet; routeToExchange() calls the
//     in-process MatchingEngine directly (the exchange_sim role). Real fills
//     require two-sided flow on the exchange book (the exchange_sim's job).
//
// Everything is real: callbacks fire, state updates propagate, and the
// simulateFill() helper exercises the entire fill path end-to-end.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "../../src/common/types.h"
#include "../../src/risk/risk_engine.h"
#include "../../src/strategy/strategy_engine.h"
#include "../../src/strategy/book_adapter.h"
#include "../../src/marketdata/book_builder/order_book_builder.h"
#include "../../src/execution/order_manager/order_manager.h"
#include "../../src/execution/fills_processor/fills_processor.h"
#include "../../src/execution/twap_vwap/oms_adapter.h"
#include "../../src/portfolio/position_tracker.h"
#include "../../src/core/matching_engine/matching_engine.h"
#include "../../src/core/matching_engine/matching_adapter.h"

namespace hft {

struct TradingEngineConfig {
    std::string symbol      = "AAPL";
    std::string strategyId  = "strat1";
    std::string journalPath = "trading_engine_fills.jsonl";
    RiskConfig   risk{};
    EngineConfig strategy{};
};

class TradingEngine {
public:
    explicit TradingEngine(TradingEngineConfig cfg)
        : cfg_(std::move(cfg))
        , risk_(cfg_.risk)
        , fills_(cfg_.journalPath)
        , strategy_(cfg_.strategy)
        , builder_(cfg_.symbol)
        , oms_(makeFillCb(), makeRejectCb(), &risk_)
    {}

    // -------- market-data ingress (normally driven by FeedHandler callback) ---
    void onFeedAdd(uint64_t orderId, Side side, double px, long qty) {
        builder_.onAdd(orderId, side, px, qty);
        refreshMid_();
    }
    void onFeedCancel(uint64_t orderId, long cancelledQty) {
        builder_.onCancel(orderId, cancelledQty);
        refreshMid_();
    }
    void onFeedTrade(double px, long qty) {
        builder_.onTrade(px, qty);
        positions_.onMark(cfg_.symbol, px, nowNs_());
    }

    // -------- strategy tick: pull top-of-book, maybe emit + route an order -----
    // Returns the clOrdId if an order was accepted by risk, else 0.
    uint64_t onBookTick() {
        const int64_t ts = nowNs_();
        std::optional<OrderRequest> intent = onBook(strategy_, builder_, cfg_.symbol, ts);
        if (!intent)
            return 0;

        const uint64_t clOrdId = oms_.submit(toOmsRequest(*intent));
        if (clOrdId != 0)
            routeToExchange(clOrdId);
        return clOrdId;
    }

    // -------- OMS -> exchange seam (in-process MatchingEngine plays venue) -----
    // Converts the working order and hands it to the matching engine, then feeds
    // any exec reports back into the OMS + downstream. submitOrder() is a stub
    // today, so this yields no fills until you implement it.
    void routeToExchange(uint64_t clOrdId) {
        OrderRecord rec;
        try {
            rec = oms_.getOrder(clOrdId);
        } catch (...) {
            return;
        }

        oms_.onAck(clOrdId, nowNs_());

        const NewOrderRequest n = fromOmsOrder(rec);
        const auto reports = exchange_.submitOrder(n);
        for (const auto& r : reports)
            applyExchangeReport_(rec, r);
    }

    // -------- test/demo helper: inject a fill as if the exchange sent one ------
    // Exercises the REAL downstream fill path (OMS -> fills/positions/risk).
    void simulateFill(uint64_t clOrdId, double fillPx, long fillQty, bool isFull) {
        oms_.onAck(clOrdId, nowNs_());
        oms_.onFill(clOrdId, fillPx, fillQty, isFull);
    }

    // -------- observability ---------------------------------------------------
    const PositionTracker& positions() const { return positions_; }
    FillsProcessor&        fills()            { return fills_; }
    MatchingEngine&        exchange()         { return exchange_; }
    OrderManager&          oms()              { return oms_; }

private:
    // Translate a matching-engine report into OMS + downstream updates.
    void applyExchangeReport_(const OrderRecord& rec, const MatchExecReport& r) {
        if (r.type == "FILL" || r.type == "PARTIAL_FILL")
            oms_.onFill(r.clOrdId, r.fillPx, r.fillQty, r.type == "FILL");
        else if (r.type == "CANCELLED")
            oms_.onCancelAck(r.clOrdId);
        else if (r.type == "REJECTED")
            oms_.onReject(r.clOrdId, "exchange reject");
        (void)rec;
    }

    // The OMS fires this on every fill. Fan out to the three consumers that
    // each keep their own slice of post-trade state.
    FillCallback makeFillCb() {
        return [this](const OrderRecord& rec, double fillPx, long fillQty) {
            const int64_t ts = nowNs_();

            // 1) post-trade journal + per-strategy PnL attribution
            ExecReport er;
            er.clOrdId    = rec.clOrdId;
            er.strategyId = cfg_.strategyId;
            er.symbol     = rec.symbol;
            er.buy        = rec.buy;
            er.type       = (rec.cumFillQty >= rec.qty) ? EXEC_FULL_FILL
                                                        : EXEC_PARTIAL_FILL;
            er.fillPx     = fillPx;
            er.arrivalMid = lastMid_;
            er.fillQty    = fillQty;
            er.isMaker    = false;
            er.tsNs       = ts;
            fills_.onExecReport(er);

            // 2) real-time position + PnL
            positions_.onFill(PositionFill{rec.symbol, rec.buy, fillPx, fillQty, ts});

            // 3) risk state (so later pre-trade checks see the new position/pnl)
            risk_.onFill(ExecFill{rec.symbol,
                                  rec.buy ? Side::BUY : Side::SELL,
                                  fillPx, fillQty, 0.0});
        };
    }

    RejectCallback makeRejectCb() {
        return [](const OrderRecord& rec, const std::string& reason) {
            (void)rec; (void)reason;  // hook: log / alert
        };
    }

    void refreshMid_() {
        const auto [bidPx, bidQty] = builder_.getLevel(Side::BUY, 0);
        const auto [askPx, askQty] = builder_.getLevel(Side::SELL, 0);
        (void)bidQty; (void)askQty;
        if (!std::isnan(bidPx) && !std::isnan(askPx)) {
            lastMid_ = (bidPx + askPx) * 0.5;
            positions_.onMark(cfg_.symbol, lastMid_, nowNs_());
        }
    }

    int64_t nowNs_() { return ++clock_; }  // monotonic logical clock for the demo

    TradingEngineConfig cfg_;
    RiskEngine          risk_;
    FillsProcessor      fills_;
    PositionTracker     positions_;
    StrategyEngine      strategy_;
    OrderBookBuilder    builder_;
    MatchingEngine      exchange_;
    OrderManager        oms_;   // constructed last: callbacks capture the above
    double              lastMid_ = 0.0;
    int64_t             clock_   = 0;
};

}  // namespace hft
