#include "feed_handler.h"

#include <cstring>
#include <limits>

namespace hft {

FeedHandler::FeedHandler(MessageCallback cb): cb_(cb)
{
    for(int i = 0; i < 256; i ++)
    {
        jumpTable_[i] = &staticNoOp;
    }
    jumpTable_[MSG_ADD]    = &staticParseAdd;
    jumpTable_[MSG_CANCEL] = &staticParseCancel;
    jumpTable_[MSG_TRADE]  = &staticParseTrade;
}

void FeedHandler::dispatchMsg(const uint8_t* msg, uint8_t type)
{
    jumpTable_[type](this, msg);
}

void FeedHandler::processPacket(const uint8_t* buf, size_t len)
{
    if (len < sizeof(PacketHeader))
        return;

    PacketHeader hdr;
    std::memcpy(&hdr, buf, sizeof(PacketHeader));

    if (hdr.seqNum != expectedSeq_) {
        if (hdr.seqNum > expectedSeq_)
            requestRetransmit(expectedSeq_, hdr.seqNum - 1);
    }

    expectedSeq_ = (hdr.seqNum == std::numeric_limits<uint64_t>::max())
                       ? 0ULL
                       : hdr.seqNum + 1;

    const uint8_t* cursor = buf + sizeof(PacketHeader);
    const uint8_t* end    = buf + len;

    for (uint16_t i = 0; i < hdr.msgCount; ++i) {
        if (static_cast<size_t>(end - cursor) < sizeof(MsgHeader))
            break;

        MsgHeader mh;
        std::memcpy(&mh, cursor, sizeof(MsgHeader));
        cursor += sizeof(MsgHeader);

        if (static_cast<size_t>(end - cursor) < mh.len)
            break;

        if (!bodyLenOk(mh.type, mh.len))
            break;

        dispatchMsg(cursor, mh.type);
        cursor += mh.len;
    }
}

AddOrderMsg  FeedHandler::parseAdd(const uint8_t* msg)
{
    // This returns a AddOrderMsg, not just a random return type the msg
    /*
    struct AddOrderMsg {
    uint64_t orderId;
    char     symbol[6];
    bool     buy;
    double   px;
    long     qty;
    };
    */
    AddOrderMsg out{}; // This creates a Order object from the Order Message struct

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

CancelOrderMsg FeedHandler::parseCancel(const uint8_t* msg)
{
    /*
    struct CancelOrderMsg {
    uint64_t orderId;
    char     symbol[6];
    long     cancelledQty;
    };
    */
    CancelOrderMsg output{};
    const uint8_t* p = msg;

    std::memcpy(&output.orderId, p, sizeof(output.orderId));
    p += sizeof(output.orderId);

    std::memcpy(output.symbol, p, 6);
    p += 6;

    std::memcpy(&output.cancelledQty, p, sizeof(output.cancelledQty));
    return output;
}

TradeMsg       FeedHandler::parseTrade(const uint8_t* msg)
{
    /*
        struct TradeMsg {
        uint64_t execId;
        char     symbol[6];
        double   px;
        long     qty;
    };
    */
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

void FeedHandler::requestRetransmit(uint64_t fromSeq, uint64_t toSeq)
{
    // processPacket detects the gap — this function just saves the missing range.
    // fromSeq = first packet we expected, toSeq = last missing packet before what we got.
    lastGapFrom_ = fromSeq;
    lastGapTo_   = toSeq;
}

bool FeedHandler::bodyLenOk(uint8_t type, uint8_t len)
{
    switch (type) {
        case MSG_ADD:    return len >= ADD_BODY_LEN;
        case MSG_CANCEL: return len >= CANCEL_BODY_LEN;
        case MSG_TRADE:  return len >= TRADE_BODY_LEN;
        default:         return true;
    }
}

void FeedHandler::staticParseAdd(FeedHandler* self, const uint8_t* msg)
{
    FeedMessage fm{};
    fm.type = MessageType::Add;
    fm.add  = self->parseAdd(msg);
    self->cb_(fm);

}

void FeedHandler::staticParseCancel(FeedHandler* self, const uint8_t* msg)
{
    FeedMessage fm{};
    fm.type = MessageType::Cancel;
    fm.cancel = self->parseCancel(msg);
    self->cb_(fm);
}

void FeedHandler::staticParseTrade(FeedHandler* self, const uint8_t* msg)
{
    FeedMessage fm{};
    fm.type = MessageType::Trade;
    fm.trade = self->parseTrade(msg);
    self->cb_(fm);
}

void FeedHandler::staticNoOp(FeedHandler*, const uint8_t*) {}

}  // namespace hft
