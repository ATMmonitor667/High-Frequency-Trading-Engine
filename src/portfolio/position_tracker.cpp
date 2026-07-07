#include "position_tracker.h"

#include <cmath>

namespace hft {

// ==========================================================================
// Queries
// ==========================================================================

Position PositionTracker::getPosition(const std::string& sym) const {
    auto it = state_.find(sym);
    if (it == state_.end())
        return {sym, 0, 0.0, 0.0};

    const SymState& s = it->second;
    return Position{sym, s.netQty, s.avgCost, s.markPx};
}

double PositionTracker::getRealisedPnL(const std::string& sym) const {
    auto it = state_.find(sym);
    if (it == state_.end())
        return 0.0;
    return it->second.realisedPnL;
}

double PositionTracker::getUnrealisedPnL(const std::string& sym) const {
    auto it = state_.find(sym);
    if (it == state_.end())
        return 0.0;

    const SymState& s = it->second;
    return (s.markPx - s.avgCost) * static_cast<double>(s.netQty);
}

double PositionTracker::getTotalPnL() const {
    double total = 0.0;
    for (const auto& kv : state_) {
        const SymState& s = kv.second;
        const double unrealised = (s.markPx - s.avgCost) * static_cast<double>(s.netQty);
        total += s.realisedPnL + unrealised;
    }
    return total;
}

RiskMetrics PositionTracker::getRisk() const {
    double grossExposure = 0.0;
    double netDelta      = 0.0;
    double equity        = 0.0;

    for (const auto& kv : state_) {
        const SymState& s = kv.second;
        const double exposure = static_cast<double>(s.netQty) * s.markPx;
        grossExposure += std::abs(exposure);
        netDelta      += exposure;

        const double unrealised = (s.markPx - s.avgCost) * static_cast<double>(s.netQty);
        equity += s.realisedPnL + unrealised;
    }

    double currentDrawdown = 0.0;
    if (peakEquity_ > 0.0) {
        currentDrawdown = (peakEquity_ - equity) / peakEquity_;
        if (currentDrawdown < 0.0) currentDrawdown = 0.0;
        if (currentDrawdown > 1.0) currentDrawdown = 1.0;
    }

    return {grossExposure, netDelta, maxDrawdown_, currentDrawdown, peakEquity_};
}

std::vector<MonitoringAlert> PositionTracker::drainAlerts() {
    std::vector<MonitoringAlert> out;
    out.swap(alerts_);  // move contents out and leave alerts_ empty
    return out;
}

// ==========================================================================
// Event handlers
// ==========================================================================

void PositionTracker::onFill(const PositionFill& f) {
    SymState& s = state_[f.symbol];

    if (f.buy) {
        // BUY: open a new lot at the fill price.
        s.lots.push_back({f.price, f.qty});
        s.netQty += f.qty;
    } else {
        // SELL: FIFO-consume the oldest lots, booking realized PnL per lot.
        long remaining = f.qty;
        while (remaining > 0 && !s.lots.empty()) {
            Lot& lot = s.lots.front();
            const long take = std::min(lot.qty, remaining);

            s.realisedPnL += (f.price - lot.price) * static_cast<double>(take);

            lot.qty   -= take;
            remaining -= take;
            if (lot.qty == 0)
                s.lots.pop_front();
        }

        // Long-only model: never go short. Any oversell beyond held inventory is
        // clamped rather than opening a short position.
        s.netQty -= f.qty;
        if (s.netQty < 0)
            s.netQty = 0;
    }

    recomputeAvgCost_(s);
    updateRiskAndAlerts_(f.symbol, f.tsNs);
}

void PositionTracker::onMark(const std::string& sym, double markPx, int64_t tsNs) {
    SymState& s = state_[sym];

    const double  prevMark = s.markPx;
    const int64_t prevTs   = lastMarkTs_[sym];

    s.markPx         = markPx;
    lastMarkTs_[sym] = tsNs;

    // Mark-jump alert: a move > 5% versus the previous mark, within 60 seconds.
    if (prevMark > 0.0 && prevTs > 0) {
        const int64_t dt  = tsNs - prevTs;
        const double  pct = std::abs(markPx - prevMark) / prevMark;
        if (dt <= kMarkJumpWindow && pct > kMarkJumpPct)
            alerts_.push_back({"MarkJump", sym, pct, tsNs});
    }

    updateRiskAndAlerts_(sym, tsNs);
}

// ==========================================================================
// Helpers
// ==========================================================================

void PositionTracker::recomputeAvgCost_(SymState& s) {
    if (s.netQty == 0 || s.lots.empty()) {
        s.avgCost = 0.0;
        return;
    }

    double sumPxQty = 0.0;
    long   sumQty   = 0;
    for (const Lot& lot : s.lots) {
        sumPxQty += lot.price * static_cast<double>(lot.qty);
        sumQty   += lot.qty;
    }
    s.avgCost = (sumQty > 0) ? (sumPxQty / static_cast<double>(sumQty)) : 0.0;
}

void PositionTracker::updateRiskAndAlerts_(const std::string& sym, int64_t tsNs) {
    // One pass over all symbols: equity (for drawdown) and gross exposure.
    double equity        = 0.0;
    double grossExposure = 0.0;
    for (const auto& kv : state_) {
        const SymState& s = kv.second;
        const double unrealised = (s.markPx - s.avgCost) * static_cast<double>(s.netQty);
        equity        += s.realisedPnL + unrealised;
        grossExposure += std::abs(static_cast<double>(s.netQty) * s.markPx);
    }

    // Track the equity peak and worst drawdown seen so far.
    if (equity > peakEquity_)
        peakEquity_ = equity;

    double currentDrawdown = 0.0;
    if (peakEquity_ > 0.0) {
        currentDrawdown = (peakEquity_ - equity) / peakEquity_;
        if (currentDrawdown < 0.0) currentDrawdown = 0.0;
        if (currentDrawdown > 1.0) currentDrawdown = 1.0;
    }
    if (currentDrawdown > maxDrawdown_)
        maxDrawdown_ = currentDrawdown;

    if (currentDrawdown > kDrawdownAlert)
        alerts_.push_back({"Drawdown", sym, currentDrawdown, tsNs});

    if (grossExposure > kGrossExpAlert)
        alerts_.push_back({"GrossExposure", sym, grossExposure, tsNs});
}

}  // namespace hft
