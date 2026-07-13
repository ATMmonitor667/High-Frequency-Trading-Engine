#include "matching_engine.h"

namespace hft {

MatchingEngine::MatchingEngine() = default;

// --------------------------------------------------------------------------
// Report factories
// --------------------------------------------------------------------------
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

MatchExecReport MatchingEngine::makeTriggeredReport(const MatchOrder& o) const {
    return MatchExecReport{o.clOrdId, "TRIGGERED", 0.0, 0, o.remaining, false};
}

MatchExecReport MatchingEngine::makeRejectedReport(uint64_t clOrdId) const {
    return MatchExecReport{clOrdId, "REJECTED", 0.0, 0, 0, false};
}

SymbolLob& MatchingEngine::lobForSymbol(const std::string& symbol) {
    return books_[symbol];
}

// --------------------------------------------------------------------------
// Price-level FIFO maintenance
// --------------------------------------------------------------------------
void MatchingEngine::appendLevel(MatchPriceLevel& level, MatchOrder* o) {
    o->next = nullptr;
    if (!level.head) {
        level.head = o;
        level.tail = o;
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
    if (!cur) return;  // not found in this level

    if (prev) prev->next = cur->next;
    else      level.head = cur->next;

    if (level.tail == cur) level.tail = prev;

    level.totalQty -= cur->remaining;
    if (level.totalQty < 0) level.totalQty = 0;  // defensive clamp

    cur->next = nullptr;
}

void MatchingEngine::restOrder(SymbolLob& book, MatchOrder* o) {
    if (!o || o->remaining <= 0)
        return;
    if (book.orderIndex.count(o->clOrdId))
        return;

    book.orderIndex.emplace(o->clOrdId, o);

    const double px = o->limitPx;
    if (o->buy) {
        auto itLvl = book.bids.find(px);
        if (itLvl == book.bids.end())
            itLvl = book.bids.emplace(px, MatchPriceLevel{px, 0, nullptr, nullptr}).first;
        appendLevel(itLvl->second, o);
        itLvl->second.totalQty += o->remaining;
    } else {
        auto itLvl = book.asks.find(px);
        if (itLvl == book.asks.end())
            itLvl = book.asks.emplace(px, MatchPriceLevel{px, 0, nullptr, nullptr}).first;
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

// --------------------------------------------------------------------------
// Stop-order list maintenance
// --------------------------------------------------------------------------
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

bool MatchingEngine::parkStopOrder(SymbolLob& lob, MatchOrder* o) {
    if (!o || lob.orderIndex.count(o->clOrdId))
        return false;

    lob.orderIndex.emplace(o->clOrdId, o);
    if (o->buy)
        lob.stopBuys.push_back(o);
    else
        lob.stopSells.push_back(o);
    return true;
}

bool MatchingEngine::stopTriggered(const MatchOrder& o, double fillPx) const {
    if (o.type != OrdType::STOP_LIMIT)
        return false;
    if (o.buy)
        return fillPx >= o.stopPx;
    return fillPx <= o.stopPx;
}

void MatchingEngine::activateTriggeredStops(const std::string& symbol, SymbolLob& lob,
                                            double fillPx,
                                            std::vector<MatchExecReport>& out) {
    std::vector<MatchOrder*> triggered;
    triggered.reserve(lob.stopBuys.size() + lob.stopSells.size());

    for (MatchOrder* o : lob.stopBuys)
        if (o && stopTriggered(*o, fillPx))
            triggered.push_back(o);
    for (MatchOrder* o : lob.stopSells)
        if (o && stopTriggered(*o, fillPx))
            triggered.push_back(o);

    for (MatchOrder* o : triggered) {
        removeFromStopList(lob, o);
        out.push_back(makeTriggeredReport(*o));
        matchIncoming(symbol, lob, o, out);
        if (o->remaining > 0)
            restOrder(lob, o);
        else
            pool_.free(o);
    }
}

// --------------------------------------------------------------------------
// Matching
// --------------------------------------------------------------------------
bool MatchingEngine::crossesLimit(const MatchOrder& taker, double makerPx) const {
    if (taker.type == OrdType::MARKET || taker.type == OrdType::IOC)
        return true;
    if (taker.type == OrdType::FOK && taker.limitPx == 0.0)
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

            activateTriggeredStops(symbol, lob, px, out);
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

            activateTriggeredStops(symbol, lob, px, out);
        }
    }
}

// --------------------------------------------------------------------------
// Order lifecycle helpers
// --------------------------------------------------------------------------
void MatchingEngine::initOrder(const NewOrderRequest& req, MatchOrder* o) {
    if (!o) return;
    o->clOrdId   = req.clOrdId;
    o->type      = req.type;
    o->buy       = req.buy;
    o->limitPx   = req.limitPx;
    o->stopPx    = req.stopPx;
    o->qty       = req.qty;
    o->remaining = req.qty;
    o->next      = nullptr;
}

void MatchingEngine::cancelTakerRemainder(SymbolLob& lob, MatchOrder* taker,
                                          std::vector<MatchExecReport>& out) {
    (void)lob;
    if (!taker || taker->remaining <= 0)
        return;
    out.push_back(makeCancelledReport(taker->clOrdId, taker->remaining));
    pool_.free(taker);
}

bool MatchingEngine::peekFokFeasible(const SymbolLob& lob, const MatchOrder& taker) const {
    long avail = 0;
    if (taker.buy) {
        for (const auto& kv : lob.asks) {
            if (!crossesLimit(taker, kv.first))
                break;
            avail += kv.second.totalQty;
            if (avail >= taker.qty)
                return true;
        }
    } else {
        for (const auto& kv : lob.bids) {
            if (!crossesLimit(taker, kv.first))
                break;
            avail += kv.second.totalQty;
            if (avail >= taker.qty)
                return true;
        }
    }
    return false;
}

void MatchingEngine::finishTakerAfterMatch(SymbolLob& lob, MatchOrder* taker,
                                           OrdType type,
                                           std::vector<MatchExecReport>& out) {
    if (!taker)
        return;
    if (taker->remaining > 0) {
        if (type == OrdType::LIMIT)
            restOrder(lob, taker);
        else
            cancelTakerRemainder(lob, taker, out);
    } else {
        pool_.free(taker);
    }
}

bool MatchingEngine::orderIdExists(uint64_t clOrdId) const {
    for (const auto& entry : books_) {
        if (entry.second.orderIndex.count(clOrdId))
            return true;
    }
    return false;
}

// --------------------------------------------------------------------------
// Public API
// --------------------------------------------------------------------------
std::vector<MatchExecReport> MatchingEngine::submitOrder(const NewOrderRequest& req) {
    std::vector<MatchExecReport> out;

    if (req.qty <= 0 || orderIdExists(req.clOrdId))
        return {makeRejectedReport(req.clOrdId)};

    MatchOrder* o = pool_.alloc();
    if (!o)
        return {makeRejectedReport(req.clOrdId)};

    initOrder(req, o);
    SymbolLob& lob = lobForSymbol(req.symbol);

    out.push_back(makeNewReport(*o));

    if (req.type == OrdType::STOP_LIMIT) {
        if (!parkStopOrder(lob, o)) {
            out = {makeRejectedReport(req.clOrdId)};
            pool_.free(o);
        }
        return out;
    }

    if (req.type == OrdType::FOK && !peekFokFeasible(lob, *o)) {
        out.push_back(makeCancelledReport(o->clOrdId, o->remaining));
        pool_.free(o);
        return out;
    }

    matchIncoming(req.symbol, lob, o, out);
    finishTakerAfterMatch(lob, o, req.type, out);
    return out;
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
        return makeCancelledReport(clOrdId, leaves);
    }
    return makeRejectedReport(clOrdId);
}

void MatchingEngine::onTrade(const std::string& symbol, double fillPx) {
    SymbolLob& lob = lobForSymbol(symbol);
    activateTriggeredStops(symbol, lob, fillPx, pendingTradeReports_);
}

std::vector<MatchExecReport> MatchingEngine::takePendingTradeReports() {
    std::vector<MatchExecReport> out;
    out.swap(pendingTradeReports_);
    return out;
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

}  // namespace hft
