#pragma once

// ==========================================================================
// Position, PnL & Real-Time Monitoring
// ==========================================================================
// The real-time position-keeping layer, downstream of the FillsProcessor. Every
// fill updates positions and realized PnL via FIFO lot matching; every mark
// (price tick) refreshes unrealized PnL, tracks the equity peak / max drawdown,
// and raises monitoring alerts:
//
//   FillsProcessor / Gateway --(PositionFill)--> [PositionTracker (HERE)]
//                            --(mark price)----->        |
//                                                        +--> getPosition / getPnL
//                                                        +--> getRisk (exposure, drawdown)
//                                                        +--> drainAlerts
//
// Model note: this is a LONG-ONLY inventory model. BUY pushes a lot; SELL
// FIFO-consumes existing long lots. It does not open short positions -- selling
// more than is held is clamped (see position_tracker.cpp) rather than going
// short. That matches the spec's lot-based examples.
//
// Alerts (checklist): current drawdown > 10%, gross exposure > 5,000,000, and a
// single-tick mark move > 5% within 60 seconds.

#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

namespace hft {

// A single execution as seen by the position layer. Named PositionFill to avoid
// colliding with hft::Fill in common/types.h (the matching-engine maker/taker
// fill), which carries different fields.
struct PositionFill {
    std::string symbol;
    bool        buy   = true;
    double      price = 0.0;
    long        qty   = 0;
    int64_t     tsNs  = 0;
};

struct Position {
    std::string symbol;
    long        netQty    = 0;
    double      avgCostPx = 0.0;
    double      markPx    = 0.0;
};

struct RiskMetrics {
    double grossExposure   = 0.0;
    double netDelta        = 0.0;
    double maxDrawdown     = 0.0;
    double currentDrawdown = 0.0;
    double peakEquity      = 0.0;
};

struct MonitoringAlert {
    std::string type;
    std::string symbol;
    double      value = 0.0;
    int64_t     tsNs  = 0;
};

class PositionTracker {
public:
    // BUY: push a lot, netQty += qty. SELL: FIFO-consume lots, booking realized
    // PnL per lot, netQty -= qty. Recomputes avg cost and refreshes drawdown.
    void onFill(const PositionFill& f);

    // Update a symbol's mark price; may raise a MarkJump alert, then refresh
    // drawdown and exposure alerts.
    void onMark(const std::string& sym, double markPx, int64_t tsNs);

    Position    getPosition(const std::string& sym) const;
    double      getUnrealisedPnL(const std::string& sym) const;  // (mark - avgCost) * netQty
    double      getRealisedPnL(const std::string& sym) const;
    double      getTotalPnL() const;                             // Σ realised + unrealised
    RiskMetrics getRisk() const;

    // Returns all pending alerts and clears the internal queue.
    std::vector<MonitoringAlert> drainAlerts();

private:
    struct Lot {
        double price = 0.0;
        long   qty   = 0;
    };

    struct SymState {
        std::deque<Lot> lots;
        long            netQty      = 0;
        double          realisedPnL = 0.0;
        double          markPx      = 0.0;
        double          avgCost     = 0.0;
    };

    // Alert thresholds.
    static constexpr double kDrawdownAlert   = 0.10;         // 10%
    static constexpr double kGrossExpAlert   = 5'000'000.0;  // notional
    static constexpr double kMarkJumpPct     = 0.05;         // 5%
    static constexpr int64_t kMarkJumpWindow = 60LL * 1'000'000'000LL;  // 60s in ns

    // Recompute the quantity-weighted average cost of the open lots.
    void recomputeAvgCost_(SymState& s);

    // Refresh peak equity / max drawdown and raise drawdown + gross-exposure
    // alerts. Computes equity and gross exposure in a single pass over state_.
    void updateRiskAndAlerts_(const std::string& sym, int64_t tsNs);

    std::unordered_map<std::string, SymState> state_;
    std::unordered_map<std::string, int64_t>  lastMarkTs_;
    std::vector<MonitoringAlert>              alerts_;
    double peakEquity_  = 0.0;
    double maxDrawdown_ = 0.0;
};

}  // namespace hft
