#include "limit_order_book.h"

#include <algorithm>
#include <limits>
#include <map>
#include <unordered_map>

namespace hft {

// Remove order from its price-level FIFO queue (O(1) via prev/next).
void LimitOrderBook::unlinkFromLevel(PriceLevel* level, Order* o) {
    if (o->prev) o->prev->next = o->next;
    else         level->headptr = o->next;
    if (o->next) o->next->prev = o->prev;
    else         level->tailptr = o->prev;
    level->totalQty -= o->qty;
}

// Append order to tail of a price level (FIFO).
void LimitOrderBook::appendToLevel(PriceLevel* level, Order* o) {
    o->prev = nullptr;
    o->next = nullptr;
    if (!level->headptr) {
        level->headptr = level->tailptr = o;
    } else {
        o->prev = level->tailptr;
        level->tailptr->next = o;
        level->tailptr = o;
    }
    level->totalQty += o->qty;
}

void LimitOrderBook::removeFromBook(Order* o) {
    double price = o->price;
    if (o->side == Side::BUY) {
        auto lvlIt = bids.find(price);
        if (lvlIt != bids.end()) {
            PriceLevel* level = lvlIt->second;
            unlinkFromLevel(level, o);
            if (!level->headptr) {
                delete level;
                bids.erase(lvlIt);
            }
        }
    } else {
        auto lvlIt = asks.find(price);
        if (lvlIt != asks.end()) {
            PriceLevel* level = lvlIt->second;
            unlinkFromLevel(level, o);
            if (!level->headptr) {
                delete level;
                asks.erase(lvlIt);
            }
        }
    }
}

LimitOrderBook::~LimitOrderBook() {
    for (auto it = orderIndex.begin(); it != orderIndex.end(); ++it)
        orderPool.release(it->second);
    orderIndex.clear();
    for (auto it = bids.begin(); it != bids.end(); ++it) delete it->second;
    for (auto it = asks.begin(); it != asks.end(); ++it) delete it->second;
    bids.clear();
    asks.clear();
}

// Insert a resting limit order. Does not match -- caller calls match() separately.
void LimitOrderBook::add(uint64_t id, Side side, double price, long qty) {
    if (orderIndex.count(id)) return;

    Order* o = orderPool.allocate(id, side, price, qty);
    if (!o) return;

    if (side == Side::BUY) {
        if (!bids.count(price)) {
            auto* level = new PriceLevel(price);
            level->headptr = level->tailptr = o;
            level->totalQty = qty;
            bids[price] = level;
        } else {
            appendToLevel(bids[price], o);
        }
    } else {
        if (!asks.count(price)) {
            auto* level = new PriceLevel(price);
            level->headptr = level->tailptr = o;
            level->totalQty = qty;
            asks[price] = level;
        } else {
            appendToLevel(asks[price], o);
        }
    }

    orderIndex[id] = o;
}

// Remove order by id. No-op if id is unknown or already filled.
void LimitOrderBook::cancel(uint64_t id) {
    auto it = orderIndex.find(id);
    if (it == orderIndex.end()) return;

    Order* o = it->second;
    removeFromBook(o);
    orderIndex.erase(it);
    orderPool.release(o);
}

// Change qty. Decrease keeps FIFO position; increase re-enqueues at tail.
void LimitOrderBook::modify(uint64_t id, long newQty) {
    auto it = orderIndex.find(id);
    if (it == orderIndex.end()) return;

    if (newQty <= 0) {
        cancel(id);
        return;
    }

    Order* o = it->second;
    long oldQty = o->qty;
    if (newQty == oldQty) return;

    PriceLevel* level = (o->side == Side::BUY)
        ? (bids.count(o->price) ? bids[o->price] : nullptr)
        : (asks.count(o->price) ? asks[o->price] : nullptr);
    if (!level) return;

    if (newQty < oldQty) {
        level->totalQty -= (oldQty - newQty);
        o->qty = newQty;
    } else {
        unlinkFromLevel(level, o);
        o->qty = newQty;
        appendToLevel(level, o);
    }
}

// Cross book while best_bid >= best_ask. Fills at maker price, FIFO at each level.
std::vector<Fill> LimitOrderBook::match() {
    std::vector<Fill> fills;

    while (!bids.empty() && !asks.empty() &&
           bids.begin()->first >= asks.begin()->first) {
        PriceLevel* bidLevel = bids.begin()->second;
        PriceLevel* askLevel = asks.begin()->second;
        Order* bidOrder = bidLevel->headptr;
        Order* askOrder = askLevel->headptr;

        long fillQty = std::min(bidOrder->qty, askOrder->qty);

        Order* makerOrder;
        Order* takerOrder;
        double fillPx;

        if (bidOrder->price > askOrder->price) {
            makerOrder = askOrder;
            takerOrder = bidOrder;
            fillPx = askOrder->price;
        } else {
            if (bidOrder->id < askOrder->id) {
                makerOrder = bidOrder;
                takerOrder = askOrder;
            } else {
                makerOrder = askOrder;
                takerOrder = bidOrder;
            }
            fillPx = bidOrder->price;
        }

        fills.push_back({makerOrder->id, takerOrder->id, fillPx, fillQty});

        bidOrder->qty -= fillQty;
        askOrder->qty -= fillQty;
        bidLevel->totalQty -= fillQty;
        askLevel->totalQty -= fillQty;

        if (bidOrder->qty == 0) {
            double px = bidOrder->price;
            unlinkFromLevel(bidLevel, bidOrder);
            orderIndex.erase(bidOrder->id);
            orderPool.release(bidOrder);
            if (!bidLevel->headptr) {
                delete bidLevel;
                bids.erase(px);
            }
        }

        if (askOrder->qty == 0) {
            double px = askOrder->price;
            unlinkFromLevel(askLevel, askOrder);
            orderIndex.erase(askOrder->id);
            orderPool.release(askOrder);
            if (!askLevel->headptr) {
                delete askLevel;
                asks.erase(px);
            }
        }
    }

    return fills;
}

// Top-of-book bid price. NaN if empty.
double LimitOrderBook::best_bid() const {
    if (bids.empty()) return std::numeric_limits<double>::quiet_NaN();
    return bids.begin()->first;
}

// Top-of-book ask price. NaN if empty.
double LimitOrderBook::best_ask() const {
    if (asks.empty()) return std::numeric_limits<double>::quiet_NaN();
    return asks.begin()->first;
}

// Up to `levels` price levels from best outward: {price, totalQty}.
std::vector<std::pair<double, long>> LimitOrderBook::getDepth(Side side, int levels) const {
    std::vector<std::pair<double, long>> result;

    if (side == Side::BUY) {
        int count = 0;
        for (auto it = bids.begin(); it != bids.end() && count < levels; ++it, ++count)
            result.push_back(std::make_pair(it->first, it->second->totalQty));
    } else {
        int count = 0;
        for (auto it = asks.begin(); it != asks.end() && count < levels; ++it, ++count)
            result.push_back(std::make_pair(it->first, it->second->totalQty));
    }

    return result;
}

}  // namespace hft
