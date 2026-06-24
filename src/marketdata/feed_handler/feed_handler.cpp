#include "feed_handler.h"

#include "../decoders/pitch_decoder.h"

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

        if (!PitchDecoder::bodyLenOk(mh.type, mh.len))
            break;

        dispatchMsg(cursor, mh.type);
        cursor += mh.len;
    }
}

void FeedHandler::requestRetransmit(uint64_t fromSeq, uint64_t toSeq)
{
    // processPacket detects the gap — this function just saves the missing range.
    // fromSeq = first packet we expected, toSeq = last missing packet before what we got.
    lastGapFrom_ = fromSeq;
    lastGapTo_   = toSeq;
}

void FeedHandler::staticParseAdd(FeedHandler* self, const uint8_t* msg)
{
    FeedMessage fm{};
    fm.type = MessageType::Add;
    fm.add  = PitchDecoder::decodeAdd(msg);
    self->cb_(fm);

}

void FeedHandler::staticParseCancel(FeedHandler* self, const uint8_t* msg)
{
    FeedMessage fm{};
    fm.type = MessageType::Cancel;
    fm.cancel = PitchDecoder::decodeCancel(msg);
    self->cb_(fm);
}

void FeedHandler::staticParseTrade(FeedHandler* self, const uint8_t* msg)
{
    FeedMessage fm{};
    fm.type = MessageType::Trade;
    fm.trade = PitchDecoder::decodeTrade(msg);
    self->cb_(fm);
}

void FeedHandler::staticNoOp(FeedHandler*, const uint8_t*) {}

}  // namespace hft
