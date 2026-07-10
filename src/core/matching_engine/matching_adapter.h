#pragma once

// ==========================================================================
// Matching Engine ↔ Trading Engine adapters
// ==========================================================================
// The matching engine speaks MatchExecReport / NewOrderRequest.
// The trading engine speaks OrderRequest / fills_processor::ExecReport /
// OrderManager callbacks. Convert at the exchange_sim / gateway boundary.
//
// Typical wiring (apps/exchange_sim):
//
//   // inbound from trading engine gateway
//   NewOrderRequest n = fromOmsOrder(omsRecord);
//   auto reports = engine.submitOrder(n);
//   for (const auto& r : reports)
//       sendToGateway(toFillsExecReport(r, symbol, buy, strategyId, mid, now));
//
//   // outbound market data
//   BookSnapshot snap = engine.getBook(symbol);
//   // encode snap as PITCH/ITCH add/cancel/trade messages → FeedHandler

#include <string>

#include "../../common/types.h"
#include "../../execution/fills_processor/fills_processor.h"
#include "../../execution/order_manager/order_manager.h"
#include "matching_engine.h"

namespace hft {

// OMS working order → matching-engine new-order (LIMIT by default).
inline NewOrderRequest fromOmsOrder(const OrderRecord& rec,
                                    OrdType type = OrdType::LIMIT,
                                    double stopPx = 0.0) {
    NewOrderRequest n;
    n.clOrdId = rec.clOrdId;
    n.symbol  = rec.symbol;
    n.buy     = rec.buy;
    n.type    = type;
    n.limitPx = rec.px;
    n.stopPx  = stopPx;
    n.qty     = rec.qty - rec.cumFillQty;
    return n;
}

// Strategy/risk OrderRequest → matching-engine new-order.
inline NewOrderRequest fromOrderRequest(const OrderRequest& r,
                                        OrdType type = OrdType::LIMIT,
                                        double stopPx = 0.0) {
    NewOrderRequest n;
    n.clOrdId = r.clientOrderId;
    n.symbol  = r.symbol;
    n.buy     = (r.side == Side::BUY);
    n.type    = type;
    n.limitPx = r.px;
    n.stopPx  = stopPx;
    n.qty     = r.qty;
    return n;
}

// MatchExecReport → fills_processor ExecReport (post-trade journal path).
inline ExecReport toFillsExecReport(const MatchExecReport& m,
                                    const std::string& symbol,
                                    bool buy,
                                    const std::string& strategyId,
                                    double arrivalMid,
                                    int64_t tsNs) {
    ExecReport e;
    e.clOrdId    = m.clOrdId;
    e.strategyId = strategyId;
    e.symbol     = symbol;
    e.buy        = buy;
    e.fillPx     = m.fillPx;
    e.arrivalMid = arrivalMid;
    e.fillQty    = m.fillQty;
    e.isMaker    = m.isMaker;
    e.tsNs       = tsNs;

    if (m.type == "NEW")
        e.type = EXEC_NEW_ACK;
    else if (m.type == "PARTIAL_FILL")
        e.type = EXEC_PARTIAL_FILL;
    else if (m.type == "FILL")
        e.type = EXEC_FULL_FILL;
    else if (m.type == "CANCELLED")
        e.type = EXEC_CANCELLED;
    else
        e.type = EXEC_REJECTED;

    return e;
}

}  // namespace hft
