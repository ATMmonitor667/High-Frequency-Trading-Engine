#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

#include "../../src/marketdata/feed_handler/feed_handler.h"

// Standalone tests for the market data FeedHandler.
// Build & run:
//   g++ -std=c++17 tests/unit/test_feed_handler.cpp
//       src/marketdata/feed_handler/feed_handler.cpp
//       src/marketdata/decoders/pitch_decoder.cpp -o test_feed && ./test_feed

namespace {

using namespace hft;

std::vector<uint8_t> makeAddBody(uint64_t orderId, long qty)
{
    std::vector<uint8_t> body(ADD_BODY_LEN, 0);
    uint8_t buy = 1;
    double  px  = 100.5;
    std::memcpy(body.data(), &orderId, 8);
    std::memcpy(body.data() + 14, &buy, 1);
    std::memcpy(body.data() + 15, &px, 8);
    std::memcpy(body.data() + 23, &qty, 8);
    return body;
}

std::vector<uint8_t> makeCancelBody(uint64_t orderId, long cancelledQty)
{
    std::vector<uint8_t> body(CANCEL_BODY_LEN, 0);
    std::memcpy(body.data(), &orderId, 8);
    std::memcpy(body.data() + 14, &cancelledQty, 8);
    return body;
}

void appendMessage(std::vector<uint8_t>& packet, uint8_t type,
                   const std::vector<uint8_t>& body)
{
    MsgHeader mh{type, static_cast<uint8_t>(body.size())};
    const size_t off = packet.size();
    packet.resize(off + sizeof(MsgHeader) + body.size());
    std::memcpy(packet.data() + off, &mh, sizeof(mh));
    std::memcpy(packet.data() + off + sizeof(mh), body.data(), body.size());
}

std::vector<uint8_t> makePacket(uint64_t seqNum, uint16_t msgCount,
                                const std::vector<std::vector<uint8_t>>& bodies,
                                const std::vector<uint8_t>& types)
{
    std::vector<uint8_t> packet(sizeof(PacketHeader), 0);
    PacketHeader ph{seqNum, msgCount};
    std::memcpy(packet.data(), &ph, sizeof(ph));
    for (size_t i = 0; i < bodies.size(); ++i)
        appendMessage(packet, types[i], bodies[i]);
    return packet;
}

}  // namespace

int main() {
    using namespace hft;

    // --- Example 1: one ADD, seq in sync, callback once ---
    {
        static int cbCount = 0;
        auto onMessage = [](const FeedMessage& m) {
            ++cbCount;
            assert(m.type == MessageType::Add);
            assert(m.add.orderId == 42);
            assert(m.add.qty == 10);
        };
        FeedHandler handler(+onMessage);
        auto packet = makePacket(1, 1, {makeAddBody(42, 10)}, {MSG_ADD});
        handler.processPacket(packet.data(), packet.size());
        assert(cbCount == 1);
        assert(handler.expectedSeq() == 2);
        assert(handler.lastGapFrom() == 0);
    }

    // --- Example 2: gap 5 → 8, retransmit(5,7), packet still processed ---
    {
        static int cbCount = 0;
        FeedHandler handler([](const FeedMessage&) { ++cbCount; });

        // Prime: seq 1..4 (no messages) so expectedSeq_ becomes 5.
        for (uint64_t seq = 1; seq <= 4; ++seq) {
            std::vector<uint8_t> pkt(sizeof(PacketHeader), 0);
            PacketHeader ph{seq, 0};
            std::memcpy(pkt.data(), &ph, sizeof(ph));
            handler.processPacket(pkt.data(), pkt.size());
        }
        assert(handler.expectedSeq() == 5);

        auto packet = makePacket(8, 1, {makeAddBody(2, 3)}, {MSG_ADD});
        handler.processPacket(packet.data(), packet.size());
        assert(handler.lastGapFrom() == 5);
        assert(handler.lastGapTo() == 7);
        assert(cbCount == 1);
        assert(handler.expectedSeq() == 9);
    }

    // --- Two messages in one packet (ADD + CANCEL) ---
    {
        static int cbCount = 0;
        static int lastCancelQty = 0;
        auto onMessage = [](const FeedMessage& m) {
            ++cbCount;
            if (m.type == MessageType::Cancel)
                lastCancelQty = static_cast<int>(m.cancel.cancelledQty);
        };
        FeedHandler handler(+onMessage);
        auto packet = makePacket(1, 2,
            {makeAddBody(99, 10), makeCancelBody(99, 4)},
            {MSG_ADD, MSG_CANCEL});
        handler.processPacket(packet.data(), packet.size());
        assert(cbCount == 2);
        assert(lastCancelQty == 4);
    }

    // --- Unknown type → staticNoOp, no callback ---
    {
        static int cbCount = 0;
        FeedHandler handler([](const FeedMessage&) { ++cbCount; });
        std::vector<uint8_t> junk(4, 0);
        auto packet = makePacket(1, 1, {junk}, {0xFF});
        handler.processPacket(packet.data(), packet.size());
        assert(cbCount == 0);
    }

    std::cout << "All feed handler tests passed\n";
    return 0;
}
