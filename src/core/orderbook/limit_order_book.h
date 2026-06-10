#pragma once

#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../../common/memory_pool.h"
#include "../../common/types.h"

namespace hft {

class LimitOrderBook {
    struct PriceLevel {
        double price;
        long   totalQty = 0;
        Order* headptr  = nullptr;
        Order* tailptr  = nullptr;

        explicit PriceLevel(double price) : price(price) {}
    };

    std::map<double, PriceLevel*, std::greater<double>> bids;
    std::map<double, PriceLevel*>                       asks;
    std::unordered_map<uint64_t, Order*>                orderIndex;
    MemoryPool<Order, 100000>                           orderPool;

    void unlinkFromLevel(PriceLevel* level, Order* o);
    void appendToLevel(PriceLevel* level, Order* o);
    void removeFromBook(Order* o);

public:
    LimitOrderBook() = default;
    LimitOrderBook(const LimitOrderBook&) = delete;
    LimitOrderBook& operator=(const LimitOrderBook&) = delete;
    ~LimitOrderBook();

    void add(uint64_t id, Side side, double price, long qty);
    void cancel(uint64_t id);
    void modify(uint64_t id, long newQty);

    std::vector<Fill>                    match();
    double                               best_bid() const;
    double                               best_ask() const;
    std::vector<std::pair<double, long>> getDepth(Side side, int levels) const;
};

}  // namespace hft
