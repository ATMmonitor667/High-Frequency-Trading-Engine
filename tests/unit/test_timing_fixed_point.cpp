#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <type_traits>

#include "../../src/common/fixed_point.h"
#include "../../src/common/timing.h"

// =============================================================================
// Standalone unit tests for TscClock (timing.h) and Px/Qty/Notional
// (fixed_point.h) -- the foundations integrated from
// Quant/AdvancedHFT/01_timing_fixed_point.cpp.
//
// Build & run (g++ 7+):
//   g++ -std=c++17 tests/unit/test_timing_fixed_point.cpp -o t && ./t
//
// Exit code is 0 only if every check passes.
// =============================================================================

namespace {

int g_checks = 0;
int g_failures = 0;
const char* g_current_test = "";

void report_fail(const std::string& msg, int line) {
    ++g_failures;
    std::cout << "  [FAIL] (" << g_current_test << ":" << line << ") " << msg << '\n';
}

void check_true(bool cond, const std::string& msg, int line) {
    ++g_checks;
    if (!cond) report_fail(msg, line);
}

template <typename A, typename B>
void check_eq(const A& a, const B& b, const std::string& msg, int line) {
    ++g_checks;
    if (!(a == b)) {
        ++g_failures;
        std::cout << "  [FAIL] (" << g_current_test << ":" << line << ") " << msg
                  << "  (got " << a << ", expected " << b << ")\n";
    }
}

#define CHECK(cond) check_true((cond), #cond, __LINE__)
#define CHECK_EQ(a, b) check_eq((a), (b), #a " == " #b, __LINE__)

#define RUN_TEST(fn)                                   \
    do {                                               \
        g_current_test = #fn;                          \
        int before = g_failures;                       \
        fn();                                          \
        if (g_failures == before)                      \
            std::cout << "[PASS] " << #fn << '\n';      \
        else                                           \
            std::cout << "[FAIL] " << #fn << '\n';      \
    } while (0)

using namespace hft;

// -----------------------------------------------------------------------------
// Px: conversion
// -----------------------------------------------------------------------------
void test_px_from_double() {
    CHECK_EQ(Px::from_double(123.4567).raw, int64_t(1234567));
    CHECK_EQ(Px::from_double(-1.5).raw,     int64_t(-15000));
    CHECK_EQ(Px::from_double(0.0).raw,      int64_t(0));
    CHECK_EQ(Px::from_double(100.0).raw,    int64_t(1000000));
}

// llround rounds half AWAY from zero, so from_double(x) and -from_double(-x)
// must agree on exact halves -- otherwise bid and ask pick up a one-unit
// asymmetry at the boundary.
void test_px_half_rounds_symmetrically() {
    CHECK_EQ(Px::from_double(0.00005).raw,  int64_t(1));
    CHECK_EQ(Px::from_double(-0.00005).raw, int64_t(-1));
}

void test_px_round_trips_through_double() {
    for (int64_t raw : {int64_t(1000000), int64_t(1234500), int64_t(-9999),
                        int64_t(0), int64_t(999999999)}) {
        const Px p{raw};
        CHECK_EQ(Px::from_double(p.to_double()).raw, raw);
    }
}

// -----------------------------------------------------------------------------
// Px: arithmetic and comparison stay integral
// -----------------------------------------------------------------------------
void test_px_arithmetic() {
    const Px a{1000000}, b{2500};             // $100.00 and $0.25
    CHECK_EQ((a + b).raw, int64_t(1002500));
    CHECK_EQ((a - b).raw, int64_t(997500));
    CHECK(a - b < a);
    CHECK(a + b > a);
    CHECK(a == Px{1000000});
    CHECK(a != b);
    CHECK(a >= Px{1000000});
    CHECK(a <= Px{1000000});
}

// The whole point of fixed point: the classic float trap must not reproduce.
void test_px_exact_where_double_is_not() {
    const double d = 0.1 + 0.2;
    CHECK(d != 0.3);                                   // the trap itself
    CHECK(Px::from_double(0.1) + Px::from_double(0.2) == Px::from_double(0.3));
}

// -----------------------------------------------------------------------------
// Px: tick maths
// -----------------------------------------------------------------------------
void test_round_to_tick() {
    CHECK_EQ(Px{1234567}.round_to_tick(Px{100}).raw, int64_t(1234600));
    CHECK_EQ(Px{1234500}.round_to_tick(Px{100}).raw, int64_t(1234500));  // already on grid
    CHECK_EQ(Px{1234549}.round_to_tick(Px{100}).raw, int64_t(1234500));  // rounds down
}

// Integer division truncates toward zero, so the naive "+half then divide"
// rounds negatives the wrong way. Negatives arise for real: a spread is a Px.
void test_round_to_tick_mirrors_across_zero() {
    CHECK_EQ(Px{-1234567}.round_to_tick(Px{100}).raw, int64_t(-1234600));
    CHECK_EQ(Px{-1234549}.round_to_tick(Px{100}).raw, int64_t(-1234500));
    CHECK_EQ(Px{0}.round_to_tick(Px{100}).raw,        int64_t(0));
}

void test_penny_tick_constant() {
    CHECK_EQ(PX_TICK_PENNY.raw, int64_t(100));
    CHECK_EQ(Px::from_double(100.004).round_to_tick(PX_TICK_PENNY).to_double(), 100.00);
    CHECK_EQ(Px::from_double(100.006).round_to_tick(PX_TICK_PENNY).to_double(), 100.01);
}

void test_ticks_between() {
    const Px tick{100};                        // $0.01
    CHECK_EQ(Px{1000500}.ticks_between(Px{1000000}, tick), int64_t(5));
    CHECK_EQ(Px{1000000}.ticks_between(Px{1000500}, tick), int64_t(-5));
    CHECK_EQ(Px{1000000}.ticks_between(Px{1000000}, tick), int64_t(0));
}

// -----------------------------------------------------------------------------
// Layout guarantees -- Px has to survive being copied through a lock-free queue
// -----------------------------------------------------------------------------
void test_px_layout() {
    CHECK(std::is_trivially_copyable<Px>::value);
    CHECK_EQ(sizeof(Px), size_t(8));
    CHECK(std::is_trivially_copyable<Notional>::value);
}

// -----------------------------------------------------------------------------
// Notional: the accumulator that int64 would silently wrap
// -----------------------------------------------------------------------------
void test_notional_basic() {
    const Notional n = Notional::of(Px::from_double(500.0), 1'000'000);
    CHECK(n.dollars() > 499'999'999.0);
    CHECK(n.dollars() < 500'000'001.0);
}

void test_notional_signed() {
    const Notional shortSide = Notional::of(Px::from_double(50.0), -200);
    CHECK(shortSide.dollars() < -9'999.0);
    CHECK(shortSide.dollars() > -10'001.0);
}

#if HFT_HAS_INT128
// A single fill never approaches INT64_MAX raw (~$922tn), but a session-long
// accumulator does. Multiplying in int64 first would wrap here; widening before
// the multiply is what keeps the result correct.
void test_notional_beyond_int64() {
    const Px   px{5'000'000};                  // $500.00 -> 5e6 raw
    const Qty  qty = 5'000'000'000'000LL;      // 5e12 shares
    const Notional n = Notional::of(px, qty);  // 2.5e19 raw > INT64_MAX (9.22e18)

    // Wrapping in int64 would have produced something small or negative.
    CHECK(n.raw > NotionalRaw(INT64_MAX));
    CHECK(n.dollars() > 2.4e15);

    // Exact check against the same product computed independently.
    const NotionalRaw expected =
        static_cast<NotionalRaw>(px.raw) * static_cast<NotionalRaw>(qty);
    CHECK(n.raw == expected);
}
#endif

// -----------------------------------------------------------------------------
// TscClock
// -----------------------------------------------------------------------------
void test_clock_calibrates() {
    CHECK(TscClock::cycles_per_ns() > 0.0);
    CHECK_EQ(TscClock::cycles_per_ns(), TscClock::cyclesPerNs());   // legacy spelling
}

// A 5ms sleep should measure as roughly 5ms. The bounds are wide because sleep
// overshoot on a loaded box is real; this catches a broken scale factor, not
// scheduler jitter.
void test_now_ns_measures_real_time() {
    const int64_t t0 = TscClock::now_ns();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const int64_t dt = TscClock::now_ns() - t0;
    CHECK(dt > 4'000'000);
    CHECK(dt < 100'000'000);
}

void test_now_ns_is_monotonic() {
    int64_t prev = TscClock::now_ns();
    for (int i = 0; i < 10000; ++i) {
        const int64_t cur = TscClock::now_ns();
        if (cur < prev) { report_fail("now_ns went backwards", __LINE__); break; }
        prev = cur;
    }
    ++g_checks;
}

// toNs() must agree with the now_ns() timeline: same calibration, one derived
// from a raw cycle delta and the other from the anchored conversion.
void test_to_ns_matches_now_ns() {
    const uint64_t c0 = static_cast<uint64_t>(TscClock::rdtsc());
    const int64_t  n0 = TscClock::now_ns();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const int64_t  n1 = TscClock::now_ns();
    const uint64_t c1 = static_cast<uint64_t>(TscClock::rdtsc());

    const double fromCycles = TscClock::toNs(c1 - c0);
    const double fromNowNs  = static_cast<double>(n1 - n0);

    // Within 10% -- the two brackets are not exactly the same interval.
    CHECK(fromCycles > fromNowNs * 0.9);
    CHECK(fromCycles < fromNowNs * 1.1 + 100000.0);
}

// wall_ns is CLOCK_REALTIME, so it should land near the current unix epoch in
// nanoseconds -- distinguishing it from the monotonic timeline now_ns uses.
void test_wall_ns_is_epoch_based() {
    const int64_t w = TscClock::wall_ns();
    CHECK(w > 1'600'000'000'000'000'000LL);   // after Sep 2020
    CHECK(w < 4'000'000'000'000'000'000LL);   // before year 2096
}

}  // namespace

int main() {
    TscClock::calibrate();

    RUN_TEST(test_px_from_double);
    RUN_TEST(test_px_half_rounds_symmetrically);
    RUN_TEST(test_px_round_trips_through_double);
    RUN_TEST(test_px_arithmetic);
    RUN_TEST(test_px_exact_where_double_is_not);
    RUN_TEST(test_round_to_tick);
    RUN_TEST(test_round_to_tick_mirrors_across_zero);
    RUN_TEST(test_penny_tick_constant);
    RUN_TEST(test_ticks_between);
    RUN_TEST(test_px_layout);
    RUN_TEST(test_notional_basic);
    RUN_TEST(test_notional_signed);
#if HFT_HAS_INT128
    RUN_TEST(test_notional_beyond_int64);
#endif
    RUN_TEST(test_clock_calibrates);
    RUN_TEST(test_now_ns_measures_real_time);
    RUN_TEST(test_now_ns_is_monotonic);
    RUN_TEST(test_to_ns_matches_now_ns);
    RUN_TEST(test_wall_ns_is_epoch_based);

    std::cout << "\n--------------------------------------------\n";
    std::cout << "TSC calibration: " << TscClock::cycles_per_ns() << " cycles/ns\n";
    std::cout << "Checks: " << g_checks << "   Failures: " << g_failures << '\n';
    if (g_failures == 0) {
        std::cout << "ALL TESTS PASSED\n";
        return 0;
    }
    std::cout << "SOME TESTS FAILED\n";
    return 1;
}
