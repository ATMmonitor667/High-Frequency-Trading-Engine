#pragma once

#include <cstddef>
#include <cstdint>

// Market data feed handler (simplified CBOE PITCH 2.x variant).
//
// Consumes raw UDP datagrams, detects sequence gaps, and dispatches decoded
// messages to a downstream callback (e.g. an order book builder). Parsing is
// zero-copy: parseXxx() reads fields straight out of the network buffer with no
// intermediate allocation. Dispatch is O(1) through a 256-entry jump table
// indexed by the message type byte; unknown types hit a no-op handler so a
// corrupt or unexpected tag can never crash the hot path.

namespace hft {

struct AddOrderMsg {
    uint64_t orderId;
    char     symbol[6];
    bool     buy;
    double   px;
    long     qty;
};

struct CancelOrderMsg {
    uint64_t orderId;
    char     symbol[6];
    long     cancelledQty;
};

struct TradeMsg {
    uint64_t execId;
    char     symbol[6];
    double   px;
    long     qty;
};

// Wire layout (packed -- no padding on the network).
struct PacketHeader {
    uint64_t seqNum;
    uint16_t msgCount;
} __attribute__((packed));

struct MsgHeader {
    uint8_t type;
    uint8_t len;
} __attribute__((packed));

static constexpr uint8_t MSG_ADD    = 0x01;
static constexpr uint8_t MSG_CANCEL = 0x02;
static constexpr uint8_t MSG_TRADE  = 0x03;

// Decoded message handed to the downstream consumer (tagged union).
struct FeedMessage {
    uint8_t type;
    union {
        AddOrderMsg    add;
        CancelOrderMsg cancel;
        TradeMsg       trade;
    };
};

using MessageCallback = void(*)(const FeedMessage&);

class FeedHandler {
    using ParseFn = void(*)(FeedHandler*, const uint8_t*);

    MessageCallback cb_;
    uint64_t        expectedSeq_ = 0;
    ParseFn         jumpTable_[256] = {};

    // Last gap range handed to requestRetransmit(), kept for inspection.
    uint64_t lastGapFrom_ = 0;
    uint64_t lastGapTo_   = 0;
    uint64_t gapCount_    = 0;

    // Jump-table trampolines: decode one message, then fire the callback.
    static void staticParseAdd(FeedHandler* self, const uint8_t* msg);
    static void staticParseCancel(FeedHandler* self, const uint8_t* msg);
    static void staticParseTrade(FeedHandler* self, const uint8_t* msg);
    static void staticNoop(FeedHandler* self, const uint8_t* msg);

public:
    explicit FeedHandler(MessageCallback cb);

    void processPacket(const uint8_t* buf, size_t len);
    void dispatchMsg(const uint8_t* msg, uint8_t type);

    AddOrderMsg    parseAdd(const uint8_t* msg);
    CancelOrderMsg parseCancel(const uint8_t* msg);
    TradeMsg       parseTrade(const uint8_t* msg);

    void requestRetransmit(uint64_t fromSeq, uint64_t toSeq);

    // Inspection helpers (testing / monitoring).
    uint64_t expectedSeq() const { return expectedSeq_; }
    uint64_t gapCount()    const { return gapCount_; }
    uint64_t lastGapFrom() const { return lastGapFrom_; }
    uint64_t lastGapTo()   const { return lastGapTo_; }
};

}  // namespace hft
