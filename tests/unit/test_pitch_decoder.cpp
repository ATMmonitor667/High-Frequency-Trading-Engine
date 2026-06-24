#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

#include "../../src/marketdata/decoders/pitch_decoder.h"
#include "../../src/marketdata/feed_handler/feed_handler.h"

// Standalone tests for PitchDecoder (wire body parsing only).
// Build & run:
//   g++ -std=c++17 tests/unit/test_pitch_decoder.cpp
//       src/marketdata/decoders/pitch_decoder.cpp -o test_pitch && ./test_pitch

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

std::vector<uint8_t> makeTradeBody(uint64_t execId, long qty)
{
    std::vector<uint8_t> body(TRADE_BODY_LEN, 0);
    double px = 50.25;
    std::memcpy(body.data(), &execId, 8);
    std::memcpy(body.data() + 14, &px, 8);
    std::memcpy(body.data() + 22, &qty, 8);
    return body;
}

}  // namespace

int main() {
    using namespace hft;

    // --- decodeAdd ---
    {
        auto body = makeAddBody(42, 10);
        assert(PitchDecoder::bodyLenOk(MSG_ADD, static_cast<uint8_t>(body.size())));
        AddOrderMsg m = PitchDecoder::decodeAdd(body.data());
        assert(m.orderId == 42);
        assert(m.qty == 10);
        assert(m.buy == true);
        assert(m.px == 100.5);
    }

    // --- decodeCancel ---
    {
        auto body = makeCancelBody(99, 4);
        assert(PitchDecoder::bodyLenOk(MSG_CANCEL, static_cast<uint8_t>(body.size())));
        CancelOrderMsg m = PitchDecoder::decodeCancel(body.data());
        assert(m.orderId == 99);
        assert(m.cancelledQty == 4);
    }

    // --- decodeTrade ---
    {
        auto body = makeTradeBody(7, 100);
        assert(PitchDecoder::bodyLenOk(MSG_TRADE, static_cast<uint8_t>(body.size())));
        TradeMsg m = PitchDecoder::decodeTrade(body.data());
        assert(m.execId == 7);
        assert(m.qty == 100);
        assert(m.px == 50.25);
    }

    // --- bodyLenOk rejects truncated bodies ---
    {
        assert(!PitchDecoder::bodyLenOk(MSG_ADD, ADD_BODY_LEN - 1));
        assert(!PitchDecoder::bodyLenOk(MSG_CANCEL, CANCEL_BODY_LEN - 1));
        assert(!PitchDecoder::bodyLenOk(MSG_TRADE, TRADE_BODY_LEN - 1));
        assert(PitchDecoder::bodyLenOk(0xFF, 1));  // unknown type -> pass (FeedHandler no-op)
    }

    // --- decode() bundles type + body into FeedMessage ---
    {
        auto body = makeAddBody(1, 5);
        FeedMessage fm{};
        assert(PitchDecoder::decode(MSG_ADD, body.data(),
                                    static_cast<uint8_t>(body.size()), fm));
        assert(fm.type == MessageType::Add);
        assert(fm.add.orderId == 1);
        assert(fm.add.qty == 5);
    }

    // --- decode() fails on bad length ---
    {
        auto body = makeAddBody(1, 5);
        FeedMessage fm{};
        assert(!PitchDecoder::decode(MSG_ADD, body.data(), ADD_BODY_LEN - 1, fm));
    }

    std::cout << "All pitch decoder tests passed\n";
    return 0;
}
