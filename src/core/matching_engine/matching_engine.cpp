#include "matching_engine.h"

namespace hft {

MatchingEngine::MatchingEngine() = default;

MatchExecReport MatchingEngine::makeFillReport(uint64_t id, const std::string& type,
                                               double px, long fillQty, long leaves,
                                               bool isMaker) const {
    return MatchExecReport{id, type, px, fillQty, leaves, isMaker};
}

MatchExecReport MatchingEngine::makeFillReport(const MatchOrder& o, double fillPx,
                                               long fillQty, bool isMaker) const {
    const long leaves = o.remaining - fillQty;
    const std::string type = (leaves == 0) ? "FILL" : "PARTIAL_FILL";
    return makeFillReport(o.clOrdId, type, fillPx, fillQty, leaves, isMaker);
}

MatchExecReport MatchingEngine::makeNewReport(const MatchOrder& o) const {
    return MatchExecReport{o.clOrdId, "NEW", 0.0, 0, o.remaining, false};
}

MatchExecReport MatchingEngine::makeCancelledReport(uint64_t clOrdId, long leaves) const {
    return MatchExecReport{clOrdId, "CANCELLED", 0.0, 0, leaves, false};
}

SymbolLob& MatchingEngine::lobForSymbol(const std::string& symbol) {
    return books_[symbol];
}

void MatchingEngine::appendLevel(MatchPriceLevel& level, MatchOrder* o) {
    o->next = nullptr;
    if (!level.head) {
        level.head = level.tail = o;
    } else {
        level.tail->next = o;
        level.tail = o;
    }
}

void MatchingEngine::removeLink(MatchPriceLevel& level, MatchOrder* o) {
    if (!o || !level.head) return;

    MatchOrder* prev = nullptr;
    MatchOrder* cur  = level.head;
    while (cur && cur != o) {
        prev = cur;
        cur  = cur->next;
    }
    if (!cur) return;

    if (prev) prev->next = cur->next;
    else      level.head = cur->next;

    if (level.tail == cur) level.tail = prev;

    level.totalQty -= cur->remaining;
    if (level.totalQty < 0) level.totalQty = 0;

    cur->next = nullptr;
}

void MatchingEngine::restOrder(SymbolLob& book, MatchOrder* o) {
    auto it = book.orderIndex.find(o->clOrdId);
    if (it != book.orderIndex.end())
        return;  // already indexed — do not double-append

    book.orderIndex.emplace(o->clOrdId, o);

    const double px = o->limitPx;
    if (o->buy) {
        auto itLvl = book.bids.find(px);
        if (itLvl == book.bids.end()) {
            auto res = book.bids.emplace(px, MatchPriceLevel{px, 0, nullptr, nullptr});
            itLvl = res.first;
        }
        appendLevel(itLvl->second, o);
        itLvl->second.totalQty += o->remaining;
    } else {
        auto itLvl = book.asks.find(px);
        if (itLvl == book.asks.end()) {
            auto res = book.asks.emplace(px, MatchPriceLevel{px, 0, nullptr, nullptr});
            itLvl = res.first;
        }
        appendLevel(itLvl->second, o);
        itLvl->second.totalQty += o->remaining;
    }
}

void MatchingEngine::upLink(SymbolLob& book, MatchOrder* o) {
    if (!o) return;

    const double px = o->limitPx;
    if (o->buy) {
        auto itLvl = book.bids.find(px);
        if (itLvl != book.bids.end()) {
            removeLink(itLvl->second, o);
            if (!itLvl->second.head)
                book.bids.erase(itLvl);
        }
    } else {
        auto itLvl = book.asks.find(px);
        if (itLvl != book.asks.end()) {
            removeLink(itLvl->second, o);
            if (!itLvl->second.head)
                book.asks.erase(itLvl);
        }
    }

    book.orderIndex.erase(o->clOrdId);
}

bool MatchingEngine::removeFromStopList(SymbolLob& book, MatchOrder* o) {
    if (!o || o->type != OrdType::STOP_LIMIT) return false;

    auto& stops = o->buy ? book.stopBuys : book.stopSells;
    const auto end = std::remove(stops.begin(), stops.end(), o);
    if (end == stops.end()) return false;

    stops.erase(end, stops.end());
    book.orderIndex.erase(o->clOrdId);
    return true;
}

bool MatchingEngine::isOnStopList(const SymbolLob& book, MatchOrder* o) const {
    if (!o) return false;
    const auto& stops = o->buy ? book.stopBuys : book.stopSells;
    return std::find(stops.begin(), stops.end(), o) != stops.end();
}

bool MatchingEngine::crossesLimit(const MatchOrder& taker, double makerPx) const {
    if (taker.type == OrdType::MARKET || taker.type == OrdType::IOC ||
        taker.type == OrdType::FOK)
        return true;
    if (taker.buy)
        return taker.limitPx >= makerPx;
    return taker.limitPx <= makerPx;
}

void MatchingEngine::matchIncoming(const std::string& symbol, SymbolLob& lob,
                                   MatchOrder* taker,
                                   std::vector<MatchExecReport>& out) {
    if (!taker || taker->remaining <= 0)
        return;

    if (taker->buy) {
        while (taker->remaining > 0 && !lob.asks.empty()) {
            auto itLvl = lob.asks.begin();
            MatchPriceLevel& level = itLvl->second;
            const double px = itLvl->first;

            if (!crossesLimit(*taker, px))
                break;

            if (!level.head) {
                lob.asks.erase(itLvl);
                continue;
            }

            MatchOrder* maker = level.head;
            const long fillQty = std::min(taker->remaining, maker->remaining);
            if (fillQty <= 0)
                break;

            out.push_back(makeFillReport(*maker, px, fillQty, true));
            out.push_back(makeFillReport(*taker, px, fillQty, false));

            taker->remaining -= fillQty;

            if (maker->remaining == fillQty) {
                upLink(lob, maker);
                pool_.free(maker);
            } else {
                maker->remaining -= fillQty;
                level.totalQty -= fillQty;
            }

            onTrade(symbol, px);
        }
    } else {
        while (taker->remaining > 0 && !lob.bids.empty()) {
            auto itLvl = lob.bids.begin();
            MatchPriceLevel& level = itLvl->second;
            const double px = itLvl->first;

            if (!crossesLimit(*taker, px))
                break;

            if (!level.head) {
                lob.bids.erase(itLvl);
                continue;
            }

            MatchOrder* maker = level.head;
            const long fillQty = std::min(taker->remaining, maker->remaining);
            if (fillQty <= 0)
                break;

            out.push_back(makeFillReport(*maker, px, fillQty, true));
            out.push_back(makeFillReport(*taker, px, fillQty, false));

            taker->remaining -= fillQty;

            if (maker->remaining == fillQty) {
                upLink(lob, maker);
                pool_.free(maker);
            } else {
                maker->remaining -= fillQty;
                level.totalQty -= fillQty;
            }

            onTrade(symbol, px);
        }
    }
}

BookSnapshot MatchingEngine::getBook(const std::string& symbol, int levels) const {
    BookSnapshot snap;

    const auto it = books_.find(symbol);
    if (it == books_.end())
        return snap;

    const SymbolLob& lob = it->second;

    int n = 0;
    for (auto bidIt = lob.bids.begin(); bidIt != lob.bids.end() && n < levels; ++bidIt, ++n)
        snap.bids.emplace_back(bidIt->first, bidIt->second.totalQty);

    n = 0;
    for (auto askIt = lob.asks.begin(); askIt != lob.asks.end() && n < levels; ++askIt, ++n)
        snap.asks.emplace_back(askIt->first, askIt->second.totalQty);

    return snap;
}

MatchExecReport MatchingEngine::cancelOrder(uint64_t clOrdId) {
    for (auto& entry : books_) {
        SymbolLob& lob = entry.second;
        auto it = lob.orderIndex.find(clOrdId);
        if (it == lob.orderIndex.end())
            continue;

        MatchOrder* o = it->second;
        const long leaves = o->remaining;

        if (isOnStopList(lob, o))
            removeFromStopList(lob, o);
        else
            upLink(lob, o);

        pool_.free(o);
        return MatchExecReport{clOrdId, "CANCELLED", 0.0, 0, leaves, false};
    }
    return MatchExecReport{clOrdId, "REJECTED", 0.0, 0, 0, false};
}

// ---------------------------------------------------------------------------
// STUBS — incomplete in the problem solution; left untouched intentionally.
// ---------------------------------------------------------------------------

std::vector<MatchExecReport> MatchingEngine::submitOrder(const NewOrderRequest& /*req*/) {
    return {};
}

void MatchingEngine::onTrade(const std::string& /*symbol*/, double /*fillPx*/) {
    // TODO: activate STOP_LIMIT orders whose stopPx was crossed by fillPx.
}

}  // namespace hft
