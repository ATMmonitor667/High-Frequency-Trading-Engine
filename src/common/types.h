#pragma once

#include <cstdint>

namespace hft {

enum class Side { BUY, SELL };

struct Fill {
    uint64_t makerOrderId;
    uint64_t takerOrderId;
    double   price;
    long     qty;
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
