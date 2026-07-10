#pragma once

// ==========================================================================
// Exchange Matching Engine  (authoritative multi-symbol LOB)
// ==========================================================================
// This is the SOURCE OF TRUTH for the exchange simulator — not a read-only
// replica like OrderBookBuilder. Orders enter here, matching happens here,
// and ExecReports leave here.
//
// WHERE IT SITS IN THE PIPELINE
// -----------------------------
//   apps/trading_engine                  apps/exchange_sim
//   --------------------                 -------------------
//   Strategy -> Risk -> OMS -> Gateway --TCP/UDP--> MatchingEngine (HERE)
//                                                    |        |
//                                                    |        +--> market data
//                                                    |             (FeedHandler)
//                                                    +--> MatchExecReport
//                                                         -> Gateway acks/fills
//                                                         -> FillsProcessor
//
// RELATIONSHIP TO LimitOrderBook
// ------------------------------
//   LimitOrderBook (core/orderbook) = single-symbol learning LOB used in unit
//   tests / early phases. MatchingEngine owns per-symbol LOBs with order-type
//   routing (LIMIT/MARKET/IOC/FOK/STOP_LIMIT), a slab pool, and exec reports.
//   Do NOT merge them — the sim uses MatchingEngine; the builder mirrors MD.
//
// TYPE NAMING (avoids collisions with existing hft types)
// -------------------------------------------------------
//   MatchOrder       vs  hft::Order          (types.h — LOB node)
//   MatchExecReport  vs  hft::ExecReport     (fills_processor.h — post-trade)
//   Use matching_adapter.h to convert MatchExecReport -> fills_processor form
//   when the gateway bridges sim fills into the trading engine.
//
// INCOMPLETE (intentionally left as stubs — do not fill in here):
//   submitOrder(), onTrade()

#include <algorithm>
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
    double     price    = 0.0;
    long       totalQty = 0;
    MatchOrder* head    = nullptr;
    MatchOrder* tail    = nullptr;
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
struct MatchExecReport {
    uint64_t    clOrdId   = 0;
    std::string type;       // "NEW" | "FILL" | "PARTIAL_FILL" | "CANCELLED" | "REJECTED"
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

struct MatchPool {
    MatchOrder slab[kMaxMatchOrders];
    int        freeList[kMaxMatchOrders];
    int        freeTop = 0;

    MatchPool() : freeTop(kMaxMatchOrders) {
        for (int i = 0; i < kMaxMatchOrders; ++i)
            freeList[i] = i;
    }

    MatchOrder* alloc() {
        if (freeTop == 0) return nullptr;
        const int idx = freeList[--freeTop];
        return &slab[idx];
    }

    void free(MatchOrder* ptr) {
        if (!ptr) return;
        const int idx = static_cast<int>(ptr - slab);
        freeList[freeTop++] = idx;
    }
};

struct SymbolLob {
    std::unordered_map<uint64_t, MatchOrder*>              orderIndex;
    std::map<double, MatchPriceLevel, std::greater<double>> bids;
    std::map<double, MatchPriceLevel, std::less<double>>    asks;
    std::vector<MatchOrder*> stopBuys;
    std::vector<MatchOrder*> stopSells;
};

class MatchingEngine {
public:
    MatchingEngine();

    // STUB — not implemented yet. Do not fill in until the problem is finished.
    std::vector<MatchExecReport> submitOrder(const NewOrderRequest& req);

    MatchExecReport cancelOrder(uint64_t clOrdId);

    // STUB — not implemented yet (stop activation). Called from matchIncoming.
    void onTrade(const std::string& symbol, double fillPx);

    BookSnapshot getBook(const std::string& symbol, int levels = 5) const;

private:
    MatchPool                                 pool_;
    std::unordered_map<std::string, SymbolLob> books_;

    MatchExecReport makeFillReport(uint64_t id, const std::string& type,
                                   double px, long fillQty, long leaves,
                                   bool isMaker) const;

    MatchExecReport makeFillReport(const MatchOrder& o, double fillPx,
                                   long fillQty, bool isMaker) const;

    MatchExecReport makeNewReport(const MatchOrder& o) const;
    MatchExecReport makeCancelledReport(uint64_t clOrdId, long leaves) const;

    SymbolLob& lobForSymbol(const std::string& symbol);

    void appendLevel(MatchPriceLevel& level, MatchOrder* o);
    void removeLink(MatchPriceLevel& level, MatchOrder* o);
    void restOrder(SymbolLob& book, MatchOrder* o);
    void upLink(SymbolLob& book, MatchOrder* o);

    bool removeFromStopList(SymbolLob& book, MatchOrder* o);
    bool isOnStopList(const SymbolLob& book, MatchOrder* o) const;

    bool crossesLimit(const MatchOrder& taker, double makerPx) const;

    void matchIncoming(const std::string& symbol, SymbolLob& lob,
                       MatchOrder* taker, std::vector<MatchExecReport>& out);
};

}  // namespace hft
