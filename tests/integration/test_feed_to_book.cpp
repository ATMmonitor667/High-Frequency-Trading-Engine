#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

#include "../../src/marketdata/feed_handler/feed_handler.h"
#include "../../src/marketdata/book_builder/order_book_builder.h"

// =============================================================================
// Integration test: FeedHandler -> OrderBookBuilder
//
// This is the pipeline join from PROJECT_PLAN Phase 4 ("consume the feed and
// rebuild the book locally"). The FeedHandler decodes raw packet bytes into
// FeedMessage events; the callback below applies them to a local L2 replica and
// we assert the rebuilt book matches what we encoded.
//
// Build & run:
//   g++ -std=c++17 tests/integration/test_feed_to_book.cpp
//       src/marketdata/feed_handler/feed_handler.cpp
//       src/marketdata/decoders/pitch_decoder.cpp
//       src/marketdata/book_builder/order_book_builder.cpp
//       -o test_feed_to_book && ./test_feed_to_book
// =============================================================================

using namespace hft;

namespace {

int g_failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            ++g_failures;                                                 \
            std::cout << "  [FAIL] (" << __LINE__ << ") " << #cond << '\n'; \
        }                                                                 \
    } while (0)

// FeedHandler's MessageCallback is a plain (non-capturing) function pointer, so
// the bridge to a builder instance goes through a file-scope pointer. The
// callback is exactly the switch sketched in the OrderBookBuilder header.
OrderBookBuilder* g_book = nullptr;

void onFeedMessage(const FeedMessage& m) {
    switch (m.type) {
        case MessageType::Add:
            g_book->onAdd(m.add.orderId,
                          m.add.buy ? Side::BUY : Side::SELL,
                          m.add.px, m.add.qty);
            break;
        case MessageType::Cancel:
            g_book->onCancel(m.cancel.orderId, m.cancel.cancelledQty);
            break;
        case MessageType::Trade:
            g_book->onTrade(m.trade.px, m.trade.qty);
            break;
        default:
            break;
    }
}

// --- wire builders (same layout the decoders expect) ------------------------

std::vector<uint8_t> makeAddBody(uint64_t orderId, bool buy, double px, long qty) {
    std::vector<uint8_t> body(ADD_BODY_LEN, 0);
    uint8_t buyByte = buy ? 1 : 0;
    std::memcpy(body.data(), &orderId, 8);          // orderId(8)
    // symbol(6) left as zeros
    std::memcpy(body.data() + 14, &buyByte, 1);     // buy(1)
    std::memcpy(body.data() + 15, &px, 8);          // px(8)
    std::memcpy(body.data() + 23, &qty, 8);         // qty(8)
    return body;
}

std::vector<uint8_t> makeCancelBody(uint64_t orderId, long cancelledQty) {
    std::vector<uint8_t> body(CANCEL_BODY_LEN, 0);
    std::memcpy(body.data(), &orderId, 8);          // orderId(8)
    std::memcpy(body.data() + 14, &cancelledQty, 8);// cancelledQty(8)
    return body;
}

void appendMessage(std::vector<uint8_t>& packet, uint8_t type,
                   const std::vector<uint8_t>& body) {
    MsgHeader mh{type, static_cast<uint8_t>(body.size())};
    const size_t off = packet.size();
    packet.resize(off + sizeof(MsgHeader) + body.size());
    std::memcpy(packet.data() + off, &mh, sizeof(mh));
    std::memcpy(packet.data() + off + sizeof(mh), body.data(), body.size());
}

std::vector<uint8_t> makePacket(uint64_t seqNum,
                                const std::vector<std::pair<uint8_t, std::vector<uint8_t>>>& msgs) {
    std::vector<uint8_t> packet(sizeof(PacketHeader), 0);
    PacketHeader ph{seqNum, static_cast<uint16_t>(msgs.size())};
    std::memcpy(packet.data(), &ph, sizeof(ph));
    for (const auto& m : msgs) appendMessage(packet, m.first, m.second);
    return packet;
}

}  // namespace

int main() {
    OrderBookBuilder book("AAPL");
    g_book = &book;
    FeedHandler handler(&onFeedMessage);

    // Packet 1: build a small two-sided book.
    auto p1 = makePacket(1, {
        {MSG_ADD, makeAddBody(1, true,  100.0, 10)},
        {MSG_ADD, makeAddBody(2, true,  100.0, 7)},   // aggregates at 100
        {MSG_ADD, makeAddBody(3, false, 101.0, 5)},
        {MSG_ADD, makeAddBody(4, false, 102.0, 8)},
    });
    handler.processPacket(p1.data(), p1.size());

    CHECK(book.getLevel(Side::BUY, 0).first == 100.0);
    CHECK(book.getLevel(Side::BUY, 0).second == 17L);   // 10 + 7
    CHECK(book.getLevel(Side::SELL, 0).first == 101.0);
    CHECK(book.getLevel(Side::SELL, 0).second == 5L);
    CHECK(book.getLevel(Side::SELL, 1).first == 102.0);
    CHECK(!book.isCrossed());

    // Packet 2: a cancel and a trade flow through the same callback.
    auto p2 = makePacket(2, {
        {MSG_CANCEL, makeCancelBody(1, 10)},          // remove id 1 -> level = 7
        {MSG_TRADE,  []{                              // consume 5 from ask @101
            std::vector<uint8_t> body(TRADE_BODY_LEN, 0);
            uint64_t execId = 99; double px = 101.0; long qty = 5;
            std::memcpy(body.data(), &execId, 8);
            std::memcpy(body.data() + 14, &px, 8);
            std::memcpy(body.data() + 22, &qty, 8);
            return body;
        }()},
    });
    handler.processPacket(p2.data(), p2.size());

    CHECK(book.getLevel(Side::BUY, 0).second == 7L);    // only id 2 left
    CHECK(std::isnan(book.getLevel(Side::SELL, 0).first) || book.getLevel(Side::SELL, 0).first == 102.0);
    CHECK(book.getLevel(Side::SELL, 0).first == 102.0); // 101 fully consumed
    CHECK(book.getLevel(Side::SELL, 0).second == 8L);

    // A sequence gap upstream should be reflected as stale until a snapshot.
    book.markStale();
    CHECK(book.isStale());
    book.onSnapshot({{10, 100.5, 3, true}, {11, 100.6, 4, false}});
    CHECK(!book.isStale());
    CHECK(book.getLevel(Side::BUY, 0).first == 100.5);
    CHECK(book.getLevel(Side::SELL, 0).first == 100.6);

    std::cout << "\n--------------------------------------------\n";
    if (g_failures == 0) {
        std::cout << "ALL INTEGRATION CHECKS PASSED\n";
        return 0;
    }
    std::cout << "SOME INTEGRATION CHECKS FAILED (" << g_failures << ")\n";
    return 1;
}
