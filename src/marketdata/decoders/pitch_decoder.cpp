#include "pitch_decoder.h"

#include "../feed_handler/feed_handler.h"

#include <cstring>

namespace hft {

bool PitchDecoder::bodyLenOk(uint8_t type, uint8_t len)
{
    switch (type) {
        case MSG_ADD:    return len >= ADD_BODY_LEN;
        case MSG_CANCEL: return len >= CANCEL_BODY_LEN;
        case MSG_TRADE:  return len >= TRADE_BODY_LEN;
        default:         return true;
    }
}

AddOrderMsg PitchDecoder::decodeAdd(const uint8_t* msg)
{
    // Wire layout (body only, MsgHeader already stripped by FeedHandler):
    //   orderId(8) | symbol(6) | buy(1) | px(8) | qty(8)
    AddOrderMsg out{};

    const uint8_t* p = msg;

    std::memcpy(&out.orderId, p, sizeof(out.orderId));
    p += sizeof(out.orderId);
    std::memcpy(out.symbol, p, 6);
    p += 6;

    uint8_t buyByte = 0;
    std::memcpy(&buyByte, p, 1);
    out.buy = (buyByte != 0);
    p += 1;

    std::memcpy(&out.px, p, sizeof(out.px));
    p += sizeof(out.px);

    std::memcpy(&out.qty, p, sizeof(out.qty));
    return out;
}

CancelOrderMsg PitchDecoder::decodeCancel(const uint8_t* msg)
{
    // Wire layout: orderId(8) | symbol(6) | cancelledQty(8)
    CancelOrderMsg output{};
    const uint8_t* p = msg;

    std::memcpy(&output.orderId, p, sizeof(output.orderId));
    p += sizeof(output.orderId);

    std::memcpy(output.symbol, p, 6);
    p += 6;

    std::memcpy(&output.cancelledQty, p, sizeof(output.cancelledQty));
    return output;
}

TradeMsg PitchDecoder::decodeTrade(const uint8_t* msg)
{
    // Wire layout: execId(8) | symbol(6) | px(8) | qty(8)
    TradeMsg out{};
    const uint8_t* ptr = msg;

    std::memcpy(&out.execId, ptr, sizeof(out.execId));
    ptr += sizeof(out.execId);

    std::memcpy(out.symbol, ptr, 6);
    ptr += 6;

    std::memcpy(&out.px, ptr, sizeof(out.px));
    ptr += sizeof(out.px);

    std::memcpy(&out.qty, ptr, sizeof(out.qty));
    return out;
}

bool PitchDecoder::decode(uint8_t type, const uint8_t* body, uint8_t len, FeedMessage& out)
{
    if (!bodyLenOk(type, len))
        return false;

    switch (type) {
        case MSG_ADD:
            out.type = MessageType::Add;
            out.add  = decodeAdd(body);
            return true;
        case MSG_CANCEL:
            out.type   = MessageType::Cancel;
            out.cancel = decodeCancel(body);
            return true;
        case MSG_TRADE:
            out.type  = MessageType::Trade;
            out.trade = decodeTrade(body);
            return true;
        default:
            return false;
    }
}

}  // namespace hft
