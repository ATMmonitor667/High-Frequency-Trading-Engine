#pragma once

// ==========================================================================
// Exchange Matching Engine  (authoritative multi-symbol LOB)
// ==========================================================================
// SOURCE OF TRUTH for the exchange simulator — not a read-only replica like
// OrderBookBuilder. Orders enter here, matching happens here, and ExecReports
// leave here.
//
//   apps/trading_engine                  apps/exchange_sim
//   Strategy -> Risk -> OMS -> Gateway --▶ MatchingEngine (HERE)
//                                            |        |
//                                            |        +--> market data (feed)
//                                            +--> MatchExecReport -> Gateway
//                                                 -> OMS -> FillsProcessor
//
// ORDER TYPES: LIMIT (rest remainder), MARKET/IOC (cancel remainder),
//   FOK (peek-feasibility then all-or-nothing), STOP_LIMIT (park, activate on
//   a trade that crosses the stop price).
//
// TYPE NAMING (avoids collisions with existing hft types):
//   MatchOrder       vs  hft::Order       (types.h — LOB node)
//   MatchExecReport  vs  hft::ExecReport  (fills_processor.h — post-trade)
//   Convert via matching_adapter.h at the exchange_sim / gateway boundary.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hft {

enum class OrdType { LIMIT, MARKET, IOC, FOK, STOP_LIMIT };

static constexpr int kMaxMatchOrders = 100000;

struct MatchOrder;

struct MatchPriceLevel {
    double      price    = 0.0;
    long        totalQty = 0;
    MatchOrder* head     = nullptr;  // FIFO queue head
    MatchOrder* tail     = nullptr;
};

struct NewOrderRequest {
    uint64_t    clOrdId = 0;
    std::string symbol;
    bool        buy     = true;
    OrdType     type    = OrdType::LIMIT;
    double      limitPx = 0.0;
    double      stopPx  = 0.0;
    long        qty     = 0;
};

// Exchange-side execution report. Distinct from fills_processor::ExecReport.
// type is one of: "NEW" | "FILL" | "PARTIAL_FILL" | "CANCELLED" |
//                 "TRIGGERED" | "REJECTED".
struct MatchExecReport {
    uint64_t    clOrdId   = 0;
    std::string type;
    double      fillPx    = 0.0;
    long        fillQty   = 0;
    long        leavesQty = 0;
    bool        isMaker   = false;
};

struct BookSnapshot {
    std::vector<std::pair<double, long>> bids;
    std::vector<std::pair<double, long>> asks;
};

struct alignas(64) MatchOrder {
    uint64_t    clOrdId   = 0;
    OrdType     type      = OrdType::LIMIT;
    bool        buy       = true;
    double      limitPx   = 0.0;
    double      stopPx    = 0.0;
    long        qty       = 0;
    long        remaining = 0;
    MatchOrder* next      = nullptr;
};

// Index-based slab pool: pre-allocate all slots; free list for O(1) reuse.
struct MatchPool {
    MatchOrder slab[kMaxMatchOrders];
    int        freeList[kMaxMatchOrders];
    int        freeTop = 0;

    MatchPool() : freeTop(kMaxMatchOrders) {
        for (int i = 0; i < kMaxMatchOrders; ++i)
            freeList[i] = i;
    }

    MatchOrder* alloc() {
        if (freeTop == 0)
            return nullptr;
        return &slab[freeList[--freeTop]];
    }

    void free(MatchOrder* ptr) {
        if (!ptr)
            return;
        const std::ptrdiff_t idx = ptr - slab;
        if (idx < 0 || idx >= kMaxMatchOrders || freeTop >= kMaxMatchOrders)
            return;
        freeList[freeTop++] = static_cast<int>(idx);
    }
};

struct SymbolLob {
    std::unordered_map<uint64_t, MatchOrder*>               orderIndex;
    std::map<double, MatchPriceLevel, std::greater<double>> bids;
    std::map<double, MatchPriceLevel, std::less<double>>    asks;
    std::vector<MatchOrder*> stopBuys;
    std::vector<MatchOrder*> stopSells;
};

class MatchingEngine {
public:
    MatchingEngine();

    // Route by order type, match, and return all resulting exec reports.
    std::vector<MatchExecReport> submitOrder(const NewOrderRequest& req);

    // Remove a working or dormant-stop order. Returns a CANCELLED (or REJECTED
    // if unknown) report.
    MatchExecReport cancelOrder(uint64_t clOrdId);

    // External trade print: activate any stops the price crossed. Results are
    // buffered; drain them with takePendingTradeReports().
    void onTrade(const std::string& symbol, double fillPx);
    std::vector<MatchExecReport> takePendingTradeReports();

    BookSnapshot getBook(const std::string& symbol, int levels = 5) const;

private:
    MatchPool                                  pool_;
    std::unordered_map<std::string, SymbolLob> books_;
    std::vector<MatchExecReport>               pendingTradeReports_;

    // Report factories.
    MatchExecReport makeFillReport(uint64_t id, const std::string& type,
                                   double px, long fillQty, long leaves,
                                   bool isMaker) const;
    MatchExecReport makeFillReport(const MatchOrder& o, double fillPx,
                                   long fillQty, bool isMaker) const;
    MatchExecReport makeNewReport(const MatchOrder& o) const;
    MatchExecReport makeCancelledReport(uint64_t clOrdId, long leaves) const;
    MatchExecReport makeTriggeredReport(const MatchOrder& o) const;
    MatchExecReport makeRejectedReport(uint64_t clOrdId) const;

    SymbolLob& lobForSymbol(const std::string& symbol);

    // Price-level FIFO maintenance.
    void appendLevel(MatchPriceLevel& level, MatchOrder* o);
    void removeLink(MatchPriceLevel& level, MatchOrder* o);
    void restOrder(SymbolLob& book, MatchOrder* o);
    void upLink(SymbolLob& book, MatchOrder* o);

    // Stop-order list maintenance.
    bool removeFromStopList(SymbolLob& book, MatchOrder* o);
    bool isOnStopList(const SymbolLob& book, MatchOrder* o) const;
    bool parkStopOrder(SymbolLob& lob, MatchOrder* o);
    bool stopTriggered(const MatchOrder& o, double fillPx) const;
    void activateTriggeredStops(const std::string& symbol, SymbolLob& lob,
                                double fillPx, std::vector<MatchExecReport>& out);

    // Matching.
    bool crossesLimit(const MatchOrder& taker, double makerPx) const;
    void matchIncoming(const std::string& symbol, SymbolLob& lob,
                       MatchOrder* taker, std::vector<MatchExecReport>& out);

    // Order lifecycle helpers.
    void initOrder(const NewOrderRequest& req, MatchOrder* o);
    void cancelTakerRemainder(SymbolLob& lob, MatchOrder* taker,
                              std::vector<MatchExecReport>& out);
    bool peekFokFeasible(const SymbolLob& lob, const MatchOrder& taker) const;
    void finishTakerAfterMatch(SymbolLob& lob, MatchOrder* taker, OrdType type,
                               std::vector<MatchExecReport>& out);
    bool orderIdExists(uint64_t clOrdId) const;
};

}  // namespace hft
