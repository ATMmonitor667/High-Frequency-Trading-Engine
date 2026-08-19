#pragma once

// ==========================================================================
// TscClock -- nanosecond timestamps from the CPU Time Stamp Counter
// ==========================================================================
// Integrated from Quant/AdvancedHFT/01_timing_fixed_point.cpp.
//
// A nanosecond timestamp source that costs ~6-10ns per read, vs ~20-30ns for
// clock_gettime. Reads the CPU's Time Stamp Counter with __rdtsc() and converts
// cycles -> ns using a frequency calibrated once at startup.
//
// Usage (call calibrate() ONCE, at process start, before any now_ns()):
//   TscClock::calibrate();               // ~50ms busy-wait, cold path
//   const int64_t t = TscClock::now_ns();
//
// Two timelines are exposed and they are NOT interchangeable:
//   now_ns()   CLOCK_MONOTONIC-based. Cheap, comparable across calls, used for
//              every hot-path timestamp (order tsNs, latency deltas).
//   wall_ns()  CLOCK_REALTIME. Syscall-priced. Log headers and journal file
//              names only -- never for measuring durations (it can step).
//
// This header replaces the earlier steady_clock-calibrated TscClock that lived
// in latency.h. The old spelling (rdtsc/toNs/cyclesPerNs) is kept intact so
// benchmarks/latency_bench.cpp and existing callers compile unchanged.

#include <cstddef>
#include <cstdint>
#include <ctime>

#if defined(_MSC_VER)
  #include <intrin.h>
  #define HFT_HAS_RDTSC 1
#elif defined(__x86_64__) || defined(__i386__)
  #include <x86intrin.h>
  #define HFT_HAS_RDTSC 1
#else
  #define HFT_HAS_RDTSC 0
#endif

#if defined(_WIN32)
  #include <chrono>
#endif

namespace hft {

// --------------------------------------------------------------------------
// Cache-line + branch-hint helpers. Defined here (rather than per-component)
// so every hot path in the engine uses one spelling of these idioms.
// --------------------------------------------------------------------------
constexpr std::size_t CACHELINE = 64;

#if defined(__GNUC__) || defined(__clang__)
  #define HFT_LIKELY(x)   __builtin_expect(!!(x), 1)
  #define HFT_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
  #define HFT_LIKELY(x)   (x)
  #define HFT_UNLIKELY(x) (x)
#endif

// CLOCK_MONOTONIC nanoseconds. The reference timeline TscClock calibrates
// against and anchors to; also the fallback timestamp on non-x86 targets.
inline int64_t monotonic_ns() {
#if defined(_WIN32)
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
#else
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
#endif
}

// Spin hint for calibration's busy-wait. Never sleep in the wait: sleeping
// hands the core back to the scheduler, and the wakeup latency lands entirely
// inside delta_ns while contributing nothing to delta_cycles.
inline void cpu_pause() {
#if HFT_HAS_RDTSC
    _mm_pause();
#endif
}

// --------------------------------------------------------------------------
class TscClock {
public:
    static int64_t rdtsc() {
#if HFT_HAS_RDTSC
        return static_cast<int64_t>(__rdtsc());
#else
        return monotonic_ns();   // fallback: 1 "cycle" == 1 ns
#endif
    }

    // Calibrate cycles_per_ns_ against the monotonic clock over `window_ms`.
    // Cold path -- call once at startup. Costs ~3 * window_ms of busy-wait.
    static void calibrate(int window_ms = 50) {
#if !HFT_HAS_RDTSC
        // No TSC on this target: the rdtsc() fallback already returns
        // nanoseconds, so the conversion factor must be the identity.
        (void)window_ms;
        cycles_per_ns_ = 1.0;
        ns_per_cycle_  = 1.0;
        anchor_tsc_    = rdtsc();
        anchor_ns_     = monotonic_ns();
#else
        // Hoisted out of the spin loop and kept in int64 the whole way:
        // `window_ms * 1e6` would promote the arithmetic to double.
        const int64_t window_ns = static_cast<int64_t>(window_ms) * 1'000'000;

        double candidates[3];
        for (int round = 0; round < 3; ++round) {
            // Open the sandwich with the cheap counter innermost, so the
            // clock_gettime overhead lands outside the rdtsc bracket.
            const int64_t ns0 = monotonic_ns();
            const int64_t c0  = rdtsc();

            const int64_t deadline = ns0 + window_ns;
            while (monotonic_ns() < deadline) {
                cpu_pause();
            }

            // Close in the SAME order as the open. Both ends then carry the
            // same read-offset between the two counters, so the offsets
            // cancel in the ratio instead of biasing it one way.
            const int64_t ns1 = monotonic_ns();
            const int64_t c1  = rdtsc();

            candidates[round] = static_cast<double>(c1 - c0) /
                                static_cast<double>(ns1 - ns0);
        }

        // Median of 3. Even while spinning, a context switch or an SMI can
        // steal the core and inflate delta_ns without inflating delta_cycles
        // proportionally. A mean would absorb that outlier; the median
        // discards it. Sorting network for 3 elements -- no <algorithm>.
        double a = candidates[0], b = candidates[1], c = candidates[2];
        if (a > b) { double t = a; a = b; b = t; }
        if (b > c) { double t = b; b = c; c = t; }
        if (a > b) { double t = a; a = b; b = t; }

        cycles_per_ns_ = (b > 0.0) ? b : 1.0;
        ns_per_cycle_  = 1.0 / cycles_per_ns_;

        // Anchor last: read both counters back-to-back so now_ns() has a
        // coincident (tsc, ns) pair to extrapolate forward from.
        anchor_tsc_ = rdtsc();
        anchor_ns_  = monotonic_ns();
#endif
    }

    // Nanoseconds on the monotonic timeline, derived from TSC. HOT.
    static int64_t now_ns() {
        // Whole body is: one rdtsc, one integer subtract, one fmul, one add.
        // No syscall, no vDSO call, no branch -- this is why it beats
        // clock_gettime by ~3x and why it is safe to call per-message.
        //
        // MONOTONICITY: guaranteed on a single core, because the TSC is a
        // free-running counter that never steps backwards and ns_per_cycle_
        // is fixed after calibrate(). ACROSS cores it can skew: each core
        // has its own TSC register, and while modern CPUs reset them together
        // at power-on, firmware, deep C-states and socket-crossing can leave
        // them offset by tens to hundreds of nanoseconds. Two timestamps
        // taken on different cores are therefore NOT safely subtractable.
        // Pin the threads that timestamp a shared timeline.
        //
        // DRIFT: cycles_per_ns_ is an estimate, so now_ns() diverges from
        // CLOCK_MONOTONIC linearly -- at ~1e-6 relative error that is ~1us
        // per second of uptime. Fine within a trading session; re-anchor
        // periodically if you need to stay tied to wall time for hours.
        //
        // BEFORE calibrate(): ns_per_cycle_ is 1.0 and the anchors are 0, so
        // this returns a raw cycle count. Monotonic and safe to subtract, but
        // the units are wrong. Calibrate at startup.
        return anchor_ns_ +
               static_cast<int64_t>(
                   static_cast<double>(rdtsc() - anchor_tsc_) * ns_per_cycle_);
    }

    // CLOCK_REALTIME ns. Cold path: log headers, journal file naming. Can step
    // backwards (NTP), so never subtract two of these to measure a duration.
    static int64_t wall_ns() {
#if defined(_WIN32)
        return static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
#else
        timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        return static_cast<int64_t>(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
#endif
    }

    // Convert a raw cycle delta (two rdtsc() reads) to nanoseconds.
    static double toNs(uint64_t cycles) {
        return static_cast<double>(cycles) * ns_per_cycle_;
    }

    static double cycles_per_ns() { return cycles_per_ns_; }
    static double cyclesPerNs()   { return cycles_per_ns_; }   // legacy spelling

private:
    static inline double  cycles_per_ns_ = 1.0;
    static inline int64_t anchor_tsc_    = 0;
    static inline int64_t anchor_ns_     = 0;

    // Reciprocal of cycles_per_ns_, kept so the hot path multiplies instead of
    // divides -- the stated latency target for now_ns() is "rdtsc + 1 fmul +
    // 1 add", which a division cannot hit. Derived state: only ever written
    // alongside cycles_per_ns_ in calibrate(), never set independently.
    static inline double  ns_per_cycle_  = 1.0;
};

// Legacy free function kept for callers written against the old latency.h.
inline uint64_t rdtsc_now() { return static_cast<uint64_t>(TscClock::rdtsc()); }

}  // namespace hft
