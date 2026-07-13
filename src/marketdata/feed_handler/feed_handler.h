#pragma once

// Concepts: zero-copy parsing, PITCH protocol, jump-table dispatch, gap detection
//
// A market data feed handler receives a raw byte stream from an exchange over
// UDP multicast, parses binary messages, detects sequence gaps, and dispatches
// decoded events to downstream consumers (e.g., an Order Book Builder).
//
// The wire protocol used here is a simplified CBOE PITCH 2.x variant:
//   - Each UDP packet contains a sequence number + a batch of messages
//   - Each message has a 1-byte type tag followed by a fixed-size body
//   - Gaps in sequence numbers mean lost packets -> trigger gap-fill request
//
//   UDP bytes -> processPacket -> dispatchMsg -> jumpTable -> PitchDecoder
//             -> FeedMessage -> cb_ -> downstream builder

#include <cstddef>
#include <cstdint>

namespace hft {

//This is the Add order message structure. After we parse the raw bytes we fill this struct.
//This does NOT add to the limit order book directly from this file — the exchange already did that.
//We just decode the event and pass it downstream (the OrderBookBuilder will use it).
struct AddOrderMsg {
    uint64_t orderId;
    char     symbol[6];
    bool     buy;
    double   px;
    long     qty;
};

//This is the cancel order message. Same idea — we decode "exchange cancelled/reduced this order"
//and pass it on. The order book builder (not this file) applies the cancel to its replica.
struct CancelOrderMsg {
    uint64_t orderId;
    char     symbol[6];
    long     cancelledQty;
};

//Trade message — someone matched on the exchange. This becomes more useful when you hook up
//the matching engine but here we only parse the trade announcement from the feed.
struct TradeMsg {
    uint64_t execId;
    char     symbol[6];
    double   px;
    long     qty;
};

//The packet header is at the start of every UDP packet.
//seqNum counts packets as they arrive (5, 6, 7...) — NOT prices. If we expect 7 but get 9,
//packets 8 and 9's worth of data might be missing (a GAP).
//msgCount tells us how many messages are stuffed inside this one packet.
//
//packed tells the compiler there isn't any extra padding between fields. Usually the compiler
//adds gaps so types align on 8-byte boundaries etc, but on the wire the bytes are back-to-back
//with no gaps — packed makes our struct match that exact layout so we don't read wrong offsets.
// Portable exact-layout packing: #pragma pack works on MSVC, GCC and Clang
// (the earlier __attribute__((packed)) was GCC/Clang-only and broke MSVC).
#pragma pack(push, 1)
struct PacketHeader {
    uint64_t seqNum;
    uint16_t msgCount;
};

//Every message inside a packet starts with this little header before the body.
//type = which kind of message (0x01 add, 0x02 cancel, 0x03 trade)
//len  = how many bytes the body is, so we know where the next message starts
struct MsgHeader {
    uint8_t type;
    uint8_t len;
};
#pragma pack(pop)

/*
These are the actual hex values for the message type byte on the wire.
0x01, 0x02, 0x03 are just written in hexadecimal because it's easier to read in specs
than writing binary (which would be long) or decimal. On the wire it's still just one byte each.
We use uint8_t because the type tag is 8 bits / one byte.
*/
static constexpr uint8_t MSG_ADD    = 0x01;
static constexpr uint8_t MSG_CANCEL = 0x02;
static constexpr uint8_t MSG_TRADE  = 0x03;

// Fixed wire body sizes (must match PitchDecoder field order in decoders/).
static constexpr uint8_t ADD_BODY_LEN    = 31;  // 8+6+1+8+8
static constexpr uint8_t CANCEL_BODY_LEN = 22;  // 8+6+8
static constexpr uint8_t TRADE_BODY_LEN  = 30;  // 8+6+8+8

class FeedHandler;  //need this forward declaration because ParseFn mentions FeedHandler below

/*
This is a naming alias. using ParseFn means "this type is a pointer to a function".
It's NOT a void pointer to random data — it's void (*)(FeedHandler*, const uint8_t*)
which means: pointer to a function that returns void and takes those two pointer args.

The jump table stores these function pointers so when we see type 0x01 we can call the
right parser without a big switch statement. dispatchMsg just does jumpTable_[type](this, msg).

using only stores types not variable names — that's why you see FeedHandler* and uint8_t*
without actual variable names after them.
*/
using ParseFn = void(*)(FeedHandler*, const uint8_t*);

/*
MessageType is like a filter / label so we know which arm of the union is valid.
MessageType::Add is the same value as 0x01, Cancel is 0x02, etc.
enum class keeps them under one type name so everything stays uniform and we don't mix up
random integers with message kinds.
*/
enum class MessageType : uint8_t {
    Add     = MSG_ADD,
    Cancel  = MSG_CANCEL,
    Trade   = MSG_TRADE,
    Unknown = 0,
};

/*
FeedMessage bundles the label (MessageType) with the actual decoded data.
type says which one is valid, and the union holds add OR cancel OR trade — only one at a time.
Only read the union member that matches type (e.g. if type==Add, only touch .add).
*/
struct FeedMessage {
    MessageType type;
    union {
        AddOrderMsg    add;
        CancelOrderMsg cancel;
        TradeMsg       trade;
    };
};

/*
Another using alias — a pointer to a function that takes const FeedMessage& and returns void.
This is what the DOWNSTREAM code registers (e.g. order book builder). We call it cb_ after parsing.

Important: the callback does NOT trigger the jump table. The order is:
  processPacket → dispatchMsg → jumpTable → parse → FeedMessage → cb_(message)
So it's like you wrote onMessage(const FeedMessage& message) and inside you switch on
message.type and call builder.onAdd / onCancel / onTrade.
*/
using MessageCallback = void(*)(const FeedMessage&);

class FeedHandler {
public:
    explicit FeedHandler(MessageCallback cb);

    uint64_t lastGapFrom() const { return lastGapFrom_; }
    uint64_t lastGapTo()   const { return lastGapTo_; }
    uint64_t expectedSeq() const { return expectedSeq_; }

    void dispatchMsg(const uint8_t* msg, uint8_t type);
    void processPacket(const uint8_t* buf, size_t len);

    void requestRetransmit(uint64_t fromSeq, uint64_t toSeq);

private:
    MessageCallback cb_;              //the callback downstream gave us in the constructor
    uint64_t expectedSeq_ = 1;  //what seqNum we expect on the next packet
    uint64_t lastGapFrom_ = 0;  //saved gap range when we detect missing packets
    uint64_t lastGapTo_ = 0;
    /*
    jumpTable_ is 256 slots — one for every possible type byte (0x00 to 0xFF).
    jumpTable_[0x01] points to staticParseAdd, [0x02] to staticParseCancel, etc.
    anything unknown points to staticNoOp so we never crash.
    these functions parse and call cb_ — they don't touch the order book directly.
    */
    ParseFn jumpTable_[256] = {};

    static void staticParseAdd(FeedHandler* self, const uint8_t* msg);
    static void staticParseCancel(FeedHandler* self, const uint8_t* msg);
    static void staticParseTrade(FeedHandler* self, const uint8_t* msg);
    static void staticNoOp(FeedHandler*, const uint8_t*);
};

}  // namespace hft
