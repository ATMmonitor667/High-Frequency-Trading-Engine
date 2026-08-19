#pragma once

// ==========================================================================
// Latency histogram
// ==========================================================================
// LatencyHistogram collects samples and reports p50/p90/p99/p99.9/max -- the
// percentile view HFT latency work is judged on (never means).
//
// TscClock itself now lives in common/timing.h, which this header pulls in so
// existing `#include "common/latency.h"` callers keep compiling unchanged. That
// version is calibrated with a median-of-3 window and adds now_ns()/wall_ns();
// the rdtsc()/toNs()/cyclesPerNs() spellings used here are unchanged.
//
// Usage:
//   TscClock::calibrate();
//   LatencyHistogram h("tick-to-trade");
//   const uint64_t t0 = TscClock::rdtsc();
//   ... work ...
//   h.record(TscClock::toNs(TscClock::rdtsc() - t0));
//   h.report();

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "timing.h"

namespace hft {

class LatencyHistogram {
public:
    explicit LatencyHistogram(std::string name) : name_(std::move(name)) {}

    void reserve(size_t n) { samples_.reserve(n); }
    void record(double ns) { samples_.push_back(ns); }
    size_t count() const { return samples_.size(); }

    double percentile(double p) const {
        if (samples_.empty()) return 0.0;
        std::vector<double> s = samples_;
        std::sort(s.begin(), s.end());
        size_t idx = static_cast<size_t>(p / 100.0 * (s.size() - 1) + 0.5);
        if (idx >= s.size()) idx = s.size() - 1;
        return s[idx];
    }

    double mean() const {
        if (samples_.empty()) return 0.0;
        double sum = 0.0;
        for (double v : samples_) sum += v;
        return sum / static_cast<double>(samples_.size());
    }

    void report(std::ostream& os = std::cout) const {
        os << "  " << name_ << "  (n=" << samples_.size() << ")\n"
           << "    mean   " << mean()             << " ns\n"
           << "    p50    " << percentile(50.0)   << " ns\n"
           << "    p90    " << percentile(90.0)   << " ns\n"
           << "    p99    " << percentile(99.0)   << " ns\n"
           << "    p99.9  " << percentile(99.9)   << " ns\n"
           << "    max    " << percentile(100.0)  << " ns\n";
    }

private:
    std::string         name_;
    std::vector<double> samples_;
};

}  // namespace hft
