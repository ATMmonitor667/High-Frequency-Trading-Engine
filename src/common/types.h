#pragma once

#include <cstdint>
#include <string>

namespace hft {

enum class Side { BUY, SELL };

struct Fill {
    uint64_t makerOrderId;
    uint64_t takerOrderId;
    double   price;
    long     qty;
};

// A single outbound order intent that flows through the system:
//   StrategyEngine emits it -> RiskEngine gates it -> OrderGateway sends it.
// Shared here (rather than owned by one component) so every stage speaks the
// same type. clientOrderId is a unique, monotonically increasing id assigned by
// the producer; tsNs is the wall-clock nanosecond timestamp used by rate
// limiting and duplicate detection in the risk engine.
struct OrderRequest {
    uint64_t    clientOrderId = 0;
    std::string symbol;
    Side        side = Side::BUY;
    double      px   = 0.0;
    long        qty  = 0;
    int64_t     tsNs = 0;
};

struct alignas(64) Order {
    uint64_t id;
    Side     side;
    double   price;
    long     qty;
    Order*   prev = nullptr;
    Order*   next = nullptr;

    Order(uint64_t id, Side side, double price, long qty)
        : id(id), side(side), price(price), qty(qty) {}
};

}  // namespace hft
