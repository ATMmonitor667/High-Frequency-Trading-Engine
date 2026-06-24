#include "order_book_builder.h"

#include <algorithm>
#include <limits>

namespace hft {

// Add delta to the level map; erase the level when its total reaches zero.
// map::operator[] default-constructs the value to 0 before adding, so a first
// insert (delta > 0) and a reduction-to-zero are both handled here.
void OrderBookBuilder::adjustLevel(Side side, double px, long delta) {
    if (side == Side::BUY) {
        bids_[px] += delta;
        if (bids_[px] == 0) bids_.erase(px);
    } else {
        asks_[px] += delta;
        if (asks_[px] == 0) asks_.erase(px);
    }
}

// Index the new order, then add its qty to the price level.
void OrderBookBuilder::onAdd(uint64_t orderId, Side side, double px, long qty) {
    const bool buy = (side == Side::BUY);
    orders_[orderId] = OrderEntry{orderId, px, qty, buy};
    adjustLevel(side, px, qty);
}

// Reduce the order and its level by cancelledQty; drop the order when empty.
void OrderBookBuilder::onCancel(uint64_t orderId, long cancelledQty) {
    auto it = orders_.find(orderId);
    if (it == orders_.end()) return;

    OrderEntry& o = it->second;
    Side side = o.buy ? Side::BUY : Side::SELL;

    adjustLevel(side, o.px, -cancelledQty);

    o.qty -= cancelledQty;
    if (o.qty <= 0) orders_.erase(it);
}

// Same-price qty change: push the delta into the level, drop if it empties.
void OrderBookBuilder::onModify(uint64_t orderId, long newQty) {
    auto it = orders_.find(orderId);
    if (it == orders_.end()) return;

    OrderEntry& o = it->second;
    Side side = o.buy ? Side::BUY : Side::SELL;

    long delta = newQty - o.qty;
    adjustLevel(side, o.px, delta);

    o.qty = newQty;
    if (o.qty <= 0) orders_.erase(it);
}

// Buyer aggressed -> liquidity removed from the ask side at px. (This feed has
// no aggressor flag; reducing asks is the spec simplification.)
void OrderBookBuilder::onTrade(double px, long qty) {
    if (qty <= 0) return;

    long remaining = qty;
    for (auto it = orders_.begin(); it != orders_.end() && remaining > 0;) {
        OrderEntry& o = it->second;
        if (!o.buy && o.px == px) {
            const long take = std::min(remaining, o.qty);
            o.qty -= take;
            remaining -= take;
            if (o.qty <= 0) it = orders_.erase(it);
            else            ++it;
        } else {
            ++it;
        }
    }

    adjustLevel(Side::SELL, px, -qty);
}

// Full reset: clear all state, clear stale_, replay each entry via onAdd.
void OrderBookBuilder::onSnapshot(const std::vector<OrderEntry>& orders) {
    orders_.clear();
    bids_.clear();
    asks_.clear();
    stale_ = false;
    for (const auto& e : orders)
        onAdd(e.id, e.buy ? Side::BUY : Side::SELL, e.px, e.qty);
}

// Nth best level from the top. {NaN, 0} when n is out of range.
std::pair<double, long> OrderBookBuilder::getLevel(Side side, int n) const {
    if (side == Side::BUY) {
        auto it = bids_.begin();
        std::advance(it, n);
        if (it == bids_.end())
            return {std::numeric_limits<double>::quiet_NaN(), 0};
        return {it->first, it->second};
    }
    auto it = asks_.begin();
    std::advance(it, n);
    if (it == asks_.end())
        return {std::numeric_limits<double>::quiet_NaN(), 0};
    return {it->first, it->second};
}

// Empty book is never "crossed".
bool OrderBookBuilder::isCrossed() const {
    if (bids_.empty() || asks_.empty()) return false;
    return bids_.begin()->first >= asks_.begin()->first;
}

}  // namespace hft
