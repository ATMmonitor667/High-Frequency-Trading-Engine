#pragma once

// ==========================================================================
// Fills Processor  (post-trade attribution + crash-recoverable journal)
// ==========================================================================
// Sits downstream of the OrderGateway. It consumes raw execution reports, keeps
// per-order fill statistics (average price, slippage vs the arrival mid),
// attributes realized cash flow and fees to each strategy, and appends every
// event to a JSON journal so state can be rebuilt after a crash:
//
//   OrderGateway --(ExecReport)--> [FillsProcessor (HERE)] --> journal file
//                                          |
//                                          +--> getSummary(clOrdId)
//                                          +--> getPnL(strategyId)
//
// Persistence model: one JSON object per line, append-only, flushed on every
// write. On construction the processor replays the journal to rebuild memory;
// replayLog() clears and rebuilds, so it is idempotent.
//
// Thread safety: a single mutex guards the journal stream and both maps. The
// processor is written to be called from the gateway thread while query methods
// (getSummary / getPnL) are called from other threads.

#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>

namespace hft {

// Kind of execution report received from the gateway. Only PARTIAL/FULL fills
// update fill statistics and PnL; ACK/CANCELLED/REJECTED are journaled only.
enum ExecType {
    EXEC_NEW_ACK,
    EXEC_PARTIAL_FILL,
    EXEC_FULL_FILL,
    EXEC_CANCELLED,
    EXEC_REJECTED
};

// A single execution report. arrivalMid is the mid price at the moment the
// order arrived, used as the slippage benchmark. isMaker selects the fee tier.
struct ExecReport {
    uint64_t    clOrdId    = 0;
    std::string strategyId;
    std::string symbol;
    bool        buy        = true;
    ExecType    type       = EXEC_NEW_ACK;
    double      fillPx     = 0.0;
    double      arrivalMid = 0.0;
    long        fillQty    = 0;
    bool        isMaker    = false;
    int64_t     tsNs       = 0;
};

// Per-order roll-up returned by getSummary(). slippage is in basis points,
// signed so that a positive value always means an adverse (costly) fill.
struct FillSummary {
    uint64_t clOrdId   = 0;
    double   avgFillPx = 0.0;
    long     cumQty    = 0;
    double   slippage  = 0.0;
};

// Per-strategy roll-up returned by getPnL(). realisedPnL is the signed trading
// cash flow (buys negative, sells positive); it equals true realized PnL only
// when the strategy is flat. totalFees is positive for costs, negative for
// maker rebates. fillCount counts fill events (not orders).
struct StrategyPnL {
    std::string id;
    double      realisedPnL = 0.0;
    double      totalFees   = 0.0;
    long        fillCount   = 0;
};

class FillsProcessor {
public:
    // Opens the journal in append mode (creating it if missing) and replays any
    // existing contents to rebuild in-memory state.
    explicit FillsProcessor(const std::string& logPath);

    // Journal the report, then (for fills) update order stats and strategy PnL.
    void onExecReport(const ExecReport& r);

    // O(1) lookups. Return a zeroed record if the id / strategy is unknown.
    FillSummary getSummary(uint64_t clOrdId) const;
    StrategyPnL getPnL(const std::string& strategyId) const;

    // Rebuild in-memory state from a journal file. Clears first, so it is safe
    // to call more than once (idempotent).
    void replayLog(const std::string& path);

private:
    // Accumulator for one client order id.
    struct OrderRecord {
        double sumPxQty   = 0.0;
        long   cumQty     = 0;
        double arrivalMid = 0.0;
        bool   buy        = true;
    };

    // Fee tiers, in basis points of notional. Maker is a rebate (negative).
    static constexpr double TAKER_FEE_BPS = 0.03;
    static constexpr double MAKER_FEE_BPS = -0.01;

    // Shared update path used by both onExecReport() and replayLog(). Assumes
    // the caller already holds mu_.
    void applyFill_(const ExecReport& r);

    mutable std::mutex mu_;
    std::unordered_map<uint64_t, OrderRecord>    orders_;
    std::unordered_map<std::string, StrategyPnL> stratPnL_;
    std::ofstream log_;
};

}  // namespace hft
