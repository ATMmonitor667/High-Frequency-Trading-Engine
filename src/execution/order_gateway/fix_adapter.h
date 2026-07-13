#pragma once

// ==========================================================================
// FIX Gateway ↔ OMS adapter
// ==========================================================================
// Bridges the FIX GatewaySession to the OrderManager:
//   * OMS working order (OrderRecord) / strategy intent (OrderRequest)
//     -> FixOrderRequest for GatewaySession::sendOrder.
//   * Inbound FixExecReport -> the matching OrderManager callback, mapping the
//     FIX ordStatus (tag 39) to the OMS state machine.
//
// Typical wiring:
//   GatewaySession gw(cfg, [&](const FixExecReport& r){
//       applyFixReport(oms, r, nowNs());
//   });
//   gw.connect();
//   gw.sendOrder(toFixOrder(oms.getOrder(clOrdId)));

#include <cstdint>
#include <string>

#include "../../common/types.h"
#include "../order_manager/order_manager.h"
#include "order_gateway.h"

namespace hft {

// OMS working order -> FIX new-order.
inline FixOrderRequest toFixOrder(const OrderRecord& rec) {
    return FixOrderRequest{rec.clOrdId, rec.symbol, rec.buy, rec.px,
                           rec.qty - rec.cumFillQty};
}

// Strategy/risk OrderRequest -> FIX new-order.
inline FixOrderRequest toFixOrder(const OrderRequest& r) {
    return FixOrderRequest{r.clientOrderId, r.symbol, r.side == Side::BUY, r.px, r.qty};
}

// Apply an inbound FIX execution report to the OMS state machine.
// FIX tag 39: 0=New, 1=Partial, 2=Filled, 4=Cancelled, 8=Rejected.
inline void applyFixReport(OrderManager& oms, const FixExecReport& r, int64_t ackNs) {
    switch (r.ordStatus) {
        case 0: oms.onAck(r.clOrdId, ackNs);                         break;
        case 1: oms.onFill(r.clOrdId, r.lastPx, r.lastQty, false);   break;
        case 2: oms.onFill(r.clOrdId, r.lastPx, r.lastQty, true);    break;
        case 4: oms.onCancelAck(r.clOrdId);                          break;
        case 8: oms.onReject(r.clOrdId, "exchange reject");          break;
        default:                                                     break;
    }
}

}  // namespace hft
