#pragma once

// Thin adapter: TwapAlgo/VwapAlgo emit OrderRequest; OrderManager::submit
// takes OmsOrderRequest (OMS assigns its own clOrdId). Use this when the
// trading-engine loop drains child orders into the OMS.
//
//   if (auto child = twap.tick(nowNs, book))
//       oms.submit(toOmsRequest(*child));

#include "../../common/types.h"
#include "../order_manager/order_manager.h"

namespace hft {

inline OmsOrderRequest toOmsRequest(const OrderRequest& r) {
    OmsOrderRequest o;
    o.symbol = r.symbol;
    o.buy    = (r.side == Side::BUY);
    o.px     = r.px;
    o.qty    = r.qty;
    o.tsNs   = r.tsNs;
    return o;
}

}  // namespace hft
