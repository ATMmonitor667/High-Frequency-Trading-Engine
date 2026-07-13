#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "../../src/core/matching_engine/matching_engine.h"

// NOTE: MatchingEngine embeds a ~6.8 MB order slab (MatchOrder[kMaxMatchOrders]).
// It MUST be heap-allocated (make_unique) — a stack instance overflows the
// default 1 MB stack and crashes before main() prints anything.

// =============================================================================
// Standalone unit tests for MatchingEngine (no external test framework).
//
// Build & run (MSVC):
//   cl /EHsc /std:c++17 /Fe:test_matching_engine.exe
//     tests\unit\test_matching_engine.cpp
//     src\core\matching_engine\matching_engine.cpp
//
// Build & run (g++ 7+):
//   g++ -std=c++17 tests/unit/test_matching_engine.cpp
//     src/core/matching_engine/matching_engine.cpp -o test_me && ./test_me
//
// Exit code is 0 only if every check passes.
// =============================================================================

namespace {

int g_checks = 0;
int g_failures = 0;
const char* g_current_test = "";

void check_true(bool cond, const std::string& msg, int line) {
    ++g_checks;
    if (!cond) {
        ++g_failures;
        std::cout << "  [FAIL] (" << g_current_test << ":" << line << ") " << msg << '\n';
    }
}

#define CHECK(cond) check_true((cond), #cond, __LINE__)

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

long countReports(const std::vector<MatchExecReport>& reps, const std::string& type) {
    long n = 0;
    for (const auto& r : reps)
        if (r.type == type) ++n;
    return n;
}

const std::string SYM = "AAPL";

// Example 1: LIMIT buy crosses a resting ask; fill occurs at the maker price.
void test_limit_cross_fills_at_maker_price() {
    auto engPtr = std::make_unique<MatchingEngine>();
    MatchingEngine& eng = *engPtr;
    eng.submitOrder({1, SYM, false, OrdType::LIMIT, 99.0, 0.0, 100});
    auto reps = eng.submitOrder({2, SYM, true, OrdType::LIMIT, 100.0, 0.0, 100});

    bool sawFillAt99 = false;
    for (const auto& r : reps)
        if (r.type == "FILL" && r.fillPx == 99.0 && r.fillQty == 100)
            sawFillAt99 = true;
    CHECK(sawFillAt99);
}

// Example 2: FOK with insufficient liquidity cancels the whole order, no fills.
void test_fok_insufficient_qty_cancels() {
    auto engPtr = std::make_unique<MatchingEngine>();
    MatchingEngine& eng = *engPtr;
    eng.submitOrder({10, SYM, false, OrdType::LIMIT, 99.0, 0.0, 50});
    auto reps = eng.submitOrder({11, SYM, true, OrdType::FOK, 0.0, 0.0, 100});

    CHECK(countReports(reps, "CANCELLED") >= 1);
    CHECK(countReports(reps, "FILL") == 0);
}

// FOK with sufficient liquidity fills fully.
void test_fok_sufficient_qty_fills() {
    auto engPtr = std::make_unique<MatchingEngine>();
    MatchingEngine& eng = *engPtr;
    eng.submitOrder({12, SYM, false, OrdType::LIMIT, 99.0, 0.0, 100});
    auto reps = eng.submitOrder({13, SYM, true, OrdType::FOK, 99.0, 0.0, 100});

    CHECK(countReports(reps, "CANCELLED") == 0);
    CHECK(countReports(reps, "FILL") >= 1);
}

// IOC fills what it can and cancels the remainder (never rests).
void test_ioc_cancels_remainder() {
    auto engPtr = std::make_unique<MatchingEngine>();
    MatchingEngine& eng = *engPtr;
    eng.submitOrder({20, SYM, false, OrdType::LIMIT, 99.0, 0.0, 40});
    auto reps = eng.submitOrder({21, SYM, true, OrdType::IOC, 99.0, 0.0, 100});

    CHECK(countReports(reps, "FILL") + countReports(reps, "PARTIAL_FILL") >= 1);
    CHECK(countReports(reps, "CANCELLED") >= 1);
    // Nothing rested: no bid remains.
    CHECK(eng.getBook(SYM).bids.empty());
}

// Example 3: STOP_LIMIT activates when a trade touches the stop price.
void test_stop_limit_activates() {
    auto engPtr = std::make_unique<MatchingEngine>();
    MatchingEngine& eng = *engPtr;
    eng.submitOrder({30, SYM, true, OrdType::STOP_LIMIT, 101.0, 100.0, 100});
    eng.submitOrder({31, SYM, false, OrdType::LIMIT, 100.0, 0.0, 1});
    auto reps = eng.submitOrder({32, SYM, true, OrdType::LIMIT, 105.0, 0.0, 1});

    bool sawStopEvent = false;
    for (const auto& r : reps)
        if (r.clOrdId == 30 &&
            (r.type == "TRIGGERED" || r.type == "FILL" || r.type == "PARTIAL_FILL"))
            sawStopEvent = true;
    CHECK(sawStopEvent);
}

// LIMIT with no cross rests and shows up in the book.
void test_limit_rests_when_no_cross() {
    auto engPtr = std::make_unique<MatchingEngine>();
    MatchingEngine& eng = *engPtr;
    eng.submitOrder({40, SYM, true, OrdType::LIMIT, 98.0, 0.0, 100});
    auto snap = eng.getBook(SYM);
    CHECK(snap.bids.size() == 1);
    CHECK(snap.bids[0].first == 98.0);
    CHECK(snap.bids[0].second == 100);
}

// Duplicate clOrdId is rejected.
void test_duplicate_id_rejected() {
    auto engPtr = std::make_unique<MatchingEngine>();
    MatchingEngine& eng = *engPtr;
    eng.submitOrder({50, SYM, true, OrdType::LIMIT, 98.0, 0.0, 100});
    auto reps = eng.submitOrder({50, SYM, true, OrdType::LIMIT, 98.0, 0.0, 100});
    CHECK(countReports(reps, "REJECTED") == 1);
}

// Cancel removes a resting order.
void test_cancel_removes_resting() {
    auto engPtr = std::make_unique<MatchingEngine>();
    MatchingEngine& eng = *engPtr;
    eng.submitOrder({60, SYM, true, OrdType::LIMIT, 98.0, 0.0, 100});
    auto rep = eng.cancelOrder(60);
    CHECK(rep.type == "CANCELLED");
    CHECK(eng.getBook(SYM).bids.empty());
}

}  // namespace

int main() {
    RUN_TEST(test_limit_cross_fills_at_maker_price);
    RUN_TEST(test_fok_insufficient_qty_cancels);
    RUN_TEST(test_fok_sufficient_qty_fills);
    RUN_TEST(test_ioc_cancels_remainder);
    RUN_TEST(test_stop_limit_activates);
    RUN_TEST(test_limit_rests_when_no_cross);
    RUN_TEST(test_duplicate_id_rejected);
    RUN_TEST(test_cancel_removes_resting);

    std::cout << "\n--------------------------------------------\n";
    std::cout << "Checks: " << g_checks << "   Failures: " << g_failures << '\n';
    if (g_failures == 0) {
        std::cout << "ALL TESTS PASSED\n";
        return 0;
    }
    std::cout << "SOME TESTS FAILED\n";
    return 1;
}
