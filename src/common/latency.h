#pragma once

// ==========================================================================
// Low-latency timing + latency histogram
// ==========================================================================
// TscClock reads the CPU Time Stamp Counter (rdtsc) for cheap (~single-digit ns)
// high-resolution timestamps, calibrated against steady_clock to convert cycles
// -> nanoseconds. LatencyHistogram collects samples and reports p50/p90/p99/
// p99.9/max -- the percentile view HFT latency work is judged on (never means).
//
// Usage:
//   TscClock::calibrate();
//   LatencyHistogram h("tick-to-trade");
//   const uint64_t t0 = TscClock::rdtsc();
//   ... work ...
//   h.record(TscClock::toNs(TscClock::rdtsc() - t0));
//   h.report();

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#if defined(_MSC_VER)
  #include <intrin.h>
#elif defined(__x86_64__) || defined(__i386__)
  #include <x86intrin.h>
#endif

namespace hft {

inline uint64_t rdtsc_now() {
#if defined(_MSC_VER) || defined(__x86_64__) || defined(__i386__)
    return __rdtsc();
#else
    // Portable fallback: steady_clock nanoseconds as "cycles" (cyclesPerNs=1).
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}

class TscClock {
public:
    // Measure TSC frequency against steady_clock over a short busy interval.
    static void calibrate(int ms = 50) {
        using clock = std::chrono::steady_clock;
        const uint64_t c0 = rdtsc_now();
        const auto     t0 = clock::now();
        const auto     dur = std::chrono::milliseconds(ms);
        while (clock::now() - t0 < dur) { /* busy wait */ }
        const uint64_t c1 = rdtsc_now();
        const auto     t1 = clock::now();
        const double ns = static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        if (ns > 0.0)
            cyclesPerNs_ = static_cast<double>(c1 - c0) / ns;
        if (cyclesPerNs_ <= 0.0)
            cyclesPerNs_ = 1.0;
    }

    static uint64_t rdtsc() { return rdtsc_now(); }
    static double   toNs(uint64_t cycles) { return static_cast<double>(cycles) / cyclesPerNs_; }
    static double   cyclesPerNs() { return cyclesPerNs_; }

private:
    static inline double cyclesPerNs_ = 1.0;
};

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
