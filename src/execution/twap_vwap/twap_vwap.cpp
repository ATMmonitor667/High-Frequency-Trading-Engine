#include "twap_vwap.h"

#include <algorithm>
#include <cmath>

namespace hft {

TwapAlgo::TwapAlgo(std::string sym, Side side, long totalQty,
                   int64_t startNs, int64_t endNs, int slices)
    : sym_(std::move(sym))
    , side_(side)
    , remaining_(totalQty)
    , sliceQty_(slices > 0 ? (totalQty + slices - 1) / slices : totalQty)
    , nextFireNs_(startNs)
    , baseInterval_(slices > 0 ? (endNs - startNs) / slices : 0)
    , nextId_(1)
    , rng_(static_cast<uint64_t>(startNs) ^ 0x9E3779B97F4A7C15ULL)
{
}

int64_t TwapAlgo::jitteredInterval() {
    // Uniform factor in [0.9, 1.1) so child fires are not perfectly periodic.
    const double factor =
        0.9 + 0.2 * (static_cast<double>(rng_()) /
                     static_cast<double>(rng_.max()));
    return static_cast<int64_t>(static_cast<double>(baseInterval_) * factor);
}

std::optional<OrderRequest> TwapAlgo::tick(int64_t nowNs, const BookUpdate& book) {
    if (nowNs < nextFireNs_ || remaining_ <= 0)
        return std::nullopt;

    const long qty = std::min(sliceQty_, remaining_);
    remaining_ -= qty;

    // Passive: buy rests at bid, sell rests at ask.
    const double px = (side_ == Side::BUY) ? book.bidPx : book.askPx;

    OrderRequest o;
    o.clientOrderId = nextId_++;
    o.symbol        = sym_;
    o.side          = side_;
    o.px            = px;
    o.qty           = qty;
    o.tsNs          = nowNs;

    nextFireNs_ += jitteredInterval();
    return o;
}

bool TwapAlgo::isDone() const {
    return remaining_ <= 0;
}

VwapAlgo::VwapAlgo(std::string sym, Side side, long totalQty,
                   int64_t startNs, int64_t endNs,
                   std::vector<std::pair<int64_t, double>> volumeProfile)
    : sym_(std::move(sym))
    , side_(side)
    , totalQty_(totalQty)
    , remaining_(totalQty)
    , curBucket_(0)
    , nextId_(1)
    , volProfile_(std::move(volumeProfile))
{
    (void)startNs;
    (void)endNs;

    double sum = 0.0;
    for (const auto& bucket : volProfile_)
        sum += bucket.second;

    if (sum > 0.0) {
        for (auto& bucket : volProfile_)
            bucket.second /= sum;
    }
}

std::optional<OrderRequest> VwapAlgo::tick(int64_t nowNs, const BookUpdate& book) {
    if (remaining_ <= 0 || curBucket_ >= static_cast<int>(volProfile_.size()))
        return std::nullopt;

    if (nowNs < volProfile_[curBucket_].first)
        return std::nullopt;

    long qty;
    if (curBucket_ == static_cast<int>(volProfile_.size()) - 1)
        qty = remaining_;  // last bucket drains remainder
    else
        qty = static_cast<long>(std::llround(
            static_cast<double>(totalQty_) * volProfile_[curBucket_].second));

    if (qty > remaining_)
        qty = remaining_;
    if (qty <= 0)
        return std::nullopt;

    remaining_ -= qty;
    ++curBucket_;

    const double px = (side_ == Side::BUY) ? book.bidPx : book.askPx;

    OrderRequest o;
    o.clientOrderId = nextId_++;
    o.symbol        = sym_;
    o.side          = side_;
    o.px            = px;
    o.qty           = qty;
    o.tsNs          = nowNs;
    return o;
}

bool VwapAlgo::isDone() const {
    return remaining_ <= 0 ||
           curBucket_ >= static_cast<int>(volProfile_.size());
}

}  // namespace hft
