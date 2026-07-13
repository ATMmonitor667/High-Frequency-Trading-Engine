#pragma once

// ==========================================================================
// Market-Making Engine  (two-sided quoting with inventory skew)
// ==========================================================================
// A market maker continuously quotes a bid and ask around a fair value,
// earning the spread when both sides fill. It manages inventory by shading
// fair value against its position, requotes only when quotes drift, suppresses
// the side that would grow inventory past a cap, and widens its spread after
// detecting adverse selection.
//
// WHERE IT SITS:
//
//   OrderBookBuilder -> book_adapter -> BookUpdate -> [MarketMaker (HERE)]
//        reads inventory from PositionTracker (IPositionTracker seam)
//        emits quotes via OrderManager        (IOrderManager   seam)
//            -> RiskEngine -> OrderGateway -> exchange
//
// DEPENDENCY INJECTION: the MarketMaker talks to two abstract seams so it can
// be unit-tested with mocks. market_maker_adapter.h provides the concrete
// implementations that forward to the real hft::OrderManager / PositionTracker.
//
// TYPE REUSE (no duplicate types): consumes hft::BookUpdate (strategy input)
// and hft::PositionFill (fill event). Its quote order is MMOrder (distinct from
// hft::Order in types.h).

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>

#include "../strategy_engine.h"                    // hft::BookUpdate
#include "../../portfolio/position_tracker.h"      // hft::PositionFill

namespace hft {

// Quote order the maker hands to the router seam. Distinct from hft::Order.
struct MMOrder {
    uint64_t    id = 0;
    std::string symbol;
    bool        buy   = true;
    double      price = 0.0;
    long        qty   = 0;
};

struct MMConfig {
    double halfSpreadBps     = 5.0;   // base half-spread
    double inventorySkewBps  = 2.0;   // bps of skew per maxInventory unit
    double requoteThreshBps  = 1.0;   // requote if quotes drift this much
    double adverseThreshBps  = 10.0;  // mid-move threshold for adverse detection
    long   maxInventory      = 1000;
    long   quoteQty          = 100;
    int    adverseCooldownMs = 500;
};

struct MMStatus {
    std::optional<double> bidPx, askPx;
    long   inventory         = 0;
    double realisedSpreadPnL = 0.0;
    bool   adverseDetected   = false;
    double spreadBps         = 0.0;   // (askPx - bidPx) / mid * 10000
};

// Injection seams (implemented by market_maker_adapter.h against the real OMS
// and PositionTracker, or by mocks in tests).
struct IOrderManager {
    virtual uint64_t submitOrder(MMOrder o) = 0;
    virtual void     cancelOrder(uint64_t)  = 0;
    virtual ~IOrderManager() = default;
};

struct IPositionTracker {
    virtual long getNetQty(const std::string& sym) const = 0;
    virtual ~IPositionTracker() = default;
};

class MarketMaker {
public:
    MarketMaker(MMConfig cfg, IOrderManager* oms, IPositionTracker* pos)
        : cfg_(cfg), oms_(oms), pos_(pos) {}

    void onBookUpdate(const BookUpdate& u) {
        symbol_ = u.symbol;
        const double mid = u.mid();
        inventory_ = pos_ ? pos_->getNetQty(symbol_) : 0;

        // Adverse-selection evaluation deferred from the last fill.
        if (hasPendingFillCheck_) {
            const int64_t window = static_cast<int64_t>(cfg_.adverseCooldownMs) * 1'000'000;
            if (u.tsNs - lastFillTsNs_ <= window) {
                const double move = cfg_.adverseThreshBps * mid / 10000.0;
                const bool adverse = lastFillBuy_ ? (mid < lastFillMid_ - move)
                                                  : (mid > lastFillMid_ + move);
                if (adverse)
                    adverseCooldownUntilNs_ = u.tsNs + window;
            }
            hasPendingFillCheck_ = false;
        }

        const bool coolingDown = u.tsNs < adverseCooldownUntilNs_;
        adverseDetected_ = coolingDown;

        // Fair value with inventory skew (long -> shade down to attract sellers).
        const double skewFactor = cfg_.maxInventory > 0
            ? static_cast<double>(inventory_) / static_cast<double>(cfg_.maxInventory)
            : 0.0;
        const double skewAmount = cfg_.inventorySkewBps * skewFactor * mid / 10000.0;
        const double fair = mid - skewAmount;

        const double effHalfSpreadBps = cfg_.halfSpreadBps * (coolingDown ? 3.0 : 1.0);
        const double halfSpread = fair * effHalfSpreadBps / 10000.0;

        const double newBid = fair - halfSpread;
        const double newAsk = fair + halfSpread;

        const bool suppressBid = inventory_ >= cfg_.maxInventory;   // already max long
        const bool suppressAsk = inventory_ <= -cfg_.maxInventory;  // already max short

        updateSide(/*isBid=*/true,  suppressBid, newBid, fair);
        updateSide(/*isBid=*/false, suppressAsk, newAsk, fair);

        lastMid_ = mid;
    }

    void onFill(const PositionFill& f) {
        realisedSpreadPnL_ += (f.buy ? (lastMid_ - f.price)
                                     : (f.price - lastMid_)) * static_cast<double>(f.qty);

        lastFillMid_         = lastMid_;
        lastFillBuy_         = f.buy;
        lastFillTsNs_        = f.tsNs;
        hasPendingFillCheck_ = true;

        if (f.buy) { activeBid_.reset(); bidId_ = 0; }
        else       { activeAsk_.reset(); askId_ = 0; }
    }

    MMStatus getStatus() const {
        double spreadBps = 0.0;
        if (activeBid_ && activeAsk_ && lastMid_ > 0.0)
            spreadBps = (*activeAsk_ - *activeBid_) / lastMid_ * 10000.0;

        return MMStatus{activeBid_, activeAsk_, inventory_,
                        realisedSpreadPnL_, adverseDetected_, spreadBps};
    }

private:
    bool shouldRequote(const std::optional<double>& active,
                       double newPx, double fair) const {
        if (!active)     return true;
        if (fair <= 0.0) return true;
        const double driftBps = std::abs(newPx - *active) / fair * 10000.0;
        return driftBps > cfg_.requoteThreshBps;
    }

    void updateSide(bool isBid, bool suppress, double newPx, double fair) {
        std::optional<double>& active = isBid ? activeBid_ : activeAsk_;
        uint64_t&              id     = isBid ? bidId_     : askId_;

        if (suppress) {
            if (id != 0 && oms_) oms_->cancelOrder(id);
            active.reset();
            id = 0;
            return;
        }

        if (!shouldRequote(active, newPx, fair))
            return;

        if (id != 0 && oms_) oms_->cancelOrder(id);

        MMOrder o{0, symbol_, isBid, newPx, cfg_.quoteQty};
        id = oms_ ? oms_->submitOrder(o) : 0;
        active = newPx;
    }

    MMConfig          cfg_;
    IOrderManager*    oms_ = nullptr;
    IPositionTracker* pos_ = nullptr;

    std::string           symbol_;
    std::optional<double> activeBid_, activeAsk_;
    uint64_t              bidId_ = 0, askId_ = 0;

    long   inventory_         = 0;
    double lastMid_           = 0.0;
    double realisedSpreadPnL_ = 0.0;

    bool    hasPendingFillCheck_    = false;
    bool    lastFillBuy_            = false;
    double  lastFillMid_            = 0.0;
    int64_t lastFillTsNs_           = 0;
    int64_t adverseCooldownUntilNs_ = 0;
    bool    adverseDetected_        = false;
};

}  // namespace hft
