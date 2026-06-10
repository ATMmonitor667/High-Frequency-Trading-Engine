#include <cmath>
#include <iostream>

#include "../../src/core/orderbook/limit_order_book.h"

// Standalone smoke tests — mirrors the original examples.
// To build:
//   g++ -std=c++20 -I. tests/unit/test_limit_order_book.cpp -o test_lob && ./test_lob
// Later: wire into GoogleTest (Phase 1 completion task).

static bool isNaN(double x) { return std::isnan(x); }

int main() {
    using namespace hft;

    // Example 1: aggressive bid crosses a resting ask (partial fill)
    {
        LimitOrderBook book;
        book.add(1, Side::BUY,  100.0, 10);
        book.add(2, Side::SELL,  99.0,  5);
        auto fills = book.match();
        bool ok = fills.size() == 1
            && fills[0].makerOrderId == 2
            && fills[0].takerOrderId == 1
            && fills[0].price == 99.0
            && fills[0].qty == 5
            && book.best_bid() == 100.0
            && isNaN(book.best_ask());
        std::cout << "Example 1: " << (ok ? "PASS" : "FAIL") << '\n';
    }

    // Example 2: ask fully fills bid, ask remainder rests
    {
        LimitOrderBook book;
        book.add(1, Side::BUY,  100.0, 10);
        book.add(2, Side::SELL, 100.0, 15);
        auto fills = book.match();
        auto depth = book.getDepth(Side::SELL, 1);
        bool ok = fills.size() == 1
            && fills[0].makerOrderId == 1
            && fills[0].takerOrderId == 2
            && fills[0].price == 100.0
            && fills[0].qty == 10
            && book.best_ask() == 100.0
            && depth.size() == 1
            && depth[0].second == 5;
        std::cout << "Example 2: " << (ok ? "PASS" : "FAIL") << '\n';
    }

    // Example 3: cancel removes the order cleanly
    {
        LimitOrderBook book;
        book.add(1, Side::BUY, 100.0, 10);
        book.cancel(1);
        bool ok = isNaN(book.best_bid());
        std::cout << "Example 3: " << (ok ? "PASS" : "FAIL") << '\n';
    }

    return 0;
}
