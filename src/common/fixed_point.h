#pragma once

// ==========================================================================
// Px / Qty / Notional -- fixed-point price arithmetic
// ==========================================================================
// Integrated from Quant/AdvancedHFT/01_timing_fixed_point.cpp.
//
// NO doubles on the hot path. Doubles compare unreliably (0.1 + 0.2 != 0.3),
// are slower to compare, and real exchanges speak integers anyway -- ITCH
// prices are int32 with 4 implied decimals. Px stores price as an int64 count
// of "raw units" of 1/10000 dollar:  $123.4567  ->  raw 1234567.
//
// MIGRATION STATUS
// ----------------
// This header is ADDITIVE. The engine still carries `double px` through
// types.h / BookUpdate / matching engine / gateway; swapping those over is the
// cross-cutting change tracked in TODO_NEXT_COMPONENTS.txt. Until then:
//
//   * New code should take Px.
//   * At a boundary with double-typed code, convert explicitly with
//     Px::from_double / Px::to_double. Both are COLD PATH -- do not put them
//     inside a per-message loop.
//   * Decoders should build Px from the wire integer directly (Px{raw}), not
//     via double. Going wire -> double -> Px only adds a rounding step and a
//     chance to lose a least-significant unit.

#include <cassert>
#include <cmath>
#include <cstdint>
#include <type_traits>

namespace hft {

// --------------------------------------------------------------------------
// Fixed-point price. 4 implied decimals (matches ITCH). $123.4567 <-> 1234567.
// --------------------------------------------------------------------------
struct Px {
    int64_t raw = 0;
    static constexpr int64_t SCALE = 10'000;

    // COLD PATH ONLY -- config parsing, test fixtures, human input. See the
    // migration note above before reaching for this on a decoded message.
    static Px from_double(double d) {
        // llround rounds half AWAY FROM ZERO, so negatives mirror positives:
        // 0.00005 -> 1 raw unit and -0.00005 -> -1. std::round-to-even would
        // make from_double(x) and -from_double(-x) disagree on exact halves,
        // which shows up as a one-unit asymmetry between bid and ask.
        return Px{static_cast<int64_t>(std::llround(d * static_cast<double>(SCALE)))};
    }

    // DISPLAY ONLY -- never compare two doubles obtained this way.
    double to_double() const {
        return static_cast<double>(raw) / static_cast<double>(SCALE);
    }

    // Snap to the nearest multiple of `tick`, rounding half away from zero.
    Px round_to_tick(Px tick) const {
        // A zero or negative tick is a config bug, not a runtime condition.
        assert(tick.raw > 0 && "round_to_tick: tick must be positive");
        if (tick.raw <= 0) return *this;

        // C++ integer division truncates TOWARD ZERO, not toward -inf. Adding
        // half a tick before dividing gives round-half-up for positives, but
        // for negatives that same +half moves toward zero and rounds the wrong
        // way. Subtracting half instead pushes away from zero, so both signs
        // round half away from zero and stay mirror images:
        //     1234567 -> 1234600      -1234567 -> -1234600
        const int64_t half = tick.raw / 2;
        const int64_t q    = (raw >= 0) ? (raw + half) / tick.raw
                                        : (raw - half) / tick.raw;
        return Px{q * tick.raw};
    }

    // Signed distance from `other` to *this, in ticks.
    int64_t ticks_between(Px other, Px tick) const {
        assert(tick.raw > 0 && "ticks_between: tick must be positive");
        const int64_t diff = raw - other.raw;
        // A price that is not a whole number of ticks off means something
        // upstream produced an off-grid quote -- a bad decoder, or a tick
        // table that does not match the instrument. Catch it here in debug
        // rather than letting truncation silently swallow the remainder.
        assert(diff % tick.raw == 0 && "ticks_between: prices not tick-aligned");
        return diff / tick.raw;
    }

    friend Px   operator+(Px a, Px b) { return Px{a.raw + b.raw}; }
    friend Px   operator-(Px a, Px b) { return Px{a.raw - b.raw}; }
    friend bool operator==(Px a, Px b) { return a.raw == b.raw; }
    friend bool operator!=(Px a, Px b) { return a.raw != b.raw; }
    friend bool operator<(Px a, Px b)  { return a.raw < b.raw; }
    friend bool operator<=(Px a, Px b) { return a.raw <= b.raw; }
    friend bool operator>(Px a, Px b)  { return a.raw > b.raw; }
    friend bool operator>=(Px a, Px b) { return a.raw >= b.raw; }
};

static_assert(std::is_trivially_copyable_v<Px>,
              "Px must be trivially copyable to travel through lock-free queues");
static_assert(sizeof(Px) == 8, "Px must stay 8 bytes -- it is copied per message");

using Qty = int64_t;

// US equities minimum increment, $0.01. Handy default for round_to_tick.
inline constexpr Px PX_TICK_PENNY{Px::SCALE / 100};

// --------------------------------------------------------------------------
// Px * Qty without silent overflow.
// --------------------------------------------------------------------------
// raw notional is px.raw * qty in units of 1/10000 dollar, so int64 saturates
// at INT64_MAX = 9.22e18 raw = ~$922 trillion. A single fill never gets close
// (a $50 stock would need ~1.8e12 shares). But this is also what you ACCUMULATE
// into -- sum the notional of every fill in a session and a large book crosses
// that line, silently and without a signal. 128 bits buys ~1.7e34 raw, which no
// accumulator will reach.
#if defined(__SIZEOF_INT128__)
  #define HFT_HAS_INT128 1
// __extension__ so the header stays clean under -Wpedantic: __int128 is a GCC/
// Clang extension, not ISO C++.
__extension__ typedef __int128 NotionalRaw;
#else
  // MSVC and other compilers without __int128. Falls back to int64 and gives
  // up the headroom above; the saturation guard in Notional::of() at least
  // makes an overflow loud instead of silent.
  #define HFT_HAS_INT128 0
using NotionalRaw = int64_t;
#endif

struct Notional {
    NotionalRaw raw = 0;

    static Notional of(Px px, Qty qty) {
#if HFT_HAS_INT128
        // Widen BEFORE multiplying. Writing NotionalRaw(px.raw * qty) would
        // overflow in int64 first and then widen the already-wrong result.
        return Notional{static_cast<NotionalRaw>(px.raw) *
                        static_cast<NotionalRaw>(qty)};
#else
        // No 128-bit type: saturate rather than wrap, so a breach is visible
        // downstream instead of turning a huge notional into a small one.
        if (px.raw != 0 && (qty > INT64_MAX / px.raw || qty < INT64_MIN / px.raw))
            return Notional{px.raw > 0 == qty > 0 ? INT64_MAX : INT64_MIN};
        return Notional{px.raw * qty};
#endif
    }

    // DISPLAY / reporting only.
    double dollars() const {
        return static_cast<double>(raw) / static_cast<double>(Px::SCALE);
    }

    friend Notional operator+(Notional a, Notional b) { return Notional{a.raw + b.raw}; }
    friend Notional operator-(Notional a, Notional b) { return Notional{a.raw - b.raw}; }
};

}  // namespace hft
