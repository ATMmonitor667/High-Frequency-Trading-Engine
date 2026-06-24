#pragma once

#include <cstddef>
#include <cstdint>

namespace hft {

struct AddOrderMsg;
struct CancelOrderMsg;
struct TradeMsg;
struct FeedMessage;

// ==========================================================================
// PITCH wire-body decoder  (simplified CBOE PITCH 2.x message bodies)
// ==========================================================================
//
// The feed handler strips the UDP packet header and each MsgHeader, then hands
// us ONLY the raw message body bytes + the type tag. This file owns turning
// those bytes into typed structs (AddOrderMsg / CancelOrderMsg / TradeMsg).
//
// We do NOT handle:
//   - packet sequencing / gap detection  (FeedHandler)
//   - jump-table dispatch / callbacks    (FeedHandler)
//   - updating the limit order book      (OrderBookBuilder, downstream)
//
// ZERO-COPY in spirit: we read straight out of the buffer with memcpy — no
// std::string, no heap allocation. Never reinterpret_cast a packed struct
// directly (UB on strict-alignment CPUs); copy field-by-field instead.
//
//   MsgHeader stripped -> PitchDecoder::decodeXxx(body) -> FeedMessage -> cb_

class PitchDecoder {
public:
    // Minimum body length for a known type. Unknown types accept any len >= 0.
    static bool bodyLenOk(uint8_t type, uint8_t len);

    static AddOrderMsg    decodeAdd(const uint8_t* body);
    static CancelOrderMsg decodeCancel(const uint8_t* body);
    static TradeMsg       decodeTrade(const uint8_t* body);

    // Decode one message body into a FeedMessage. Returns false if bodyLenOk
    // fails (truncated/corrupt wire) or type is unknown.
    static bool decode(uint8_t type, const uint8_t* body, uint8_t len, FeedMessage& out);
};

}  // namespace hft
