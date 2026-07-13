#pragma once

// ==========================================================================
// FIX 4.2 Order Gateway Session  (outbound order entry + session management)
// ==========================================================================
// The gateway is the trading engine's link to the exchange over FIX. It owns
// the session lifecycle (logon/logout/heartbeat), encodes outbound orders,
// parses inbound execution reports, and detects sequence-number gaps.
//
//   OMS -> [OrderGateway/GatewaySession (HERE)] --FIX--> exchange
//              ^                                             |
//              +---- OmsCallback(FixExecReport) <--- onData(bytes) ----+
//
// RELATIONSHIP TO THE IN-PROCESS PATH:
//   apps/trading_engine currently routes to the in-process MatchingEngine
//   directly (the exchange_sim role). This GatewaySession is the *real-network*
//   transport alternative: same OMS on one side, a FIX venue on the other.
//   Bridge to the OMS via fix_adapter.h.
//
// TYPE NAMING (avoids collisions with existing hft types):
//   FixOrderRequest  vs  hft::OrderRequest  (types.h)
//   FixExecReport    vs  hft::ExecReport    (fills_processor.h)
//
// THREADING: not internally synchronised. Drive onData()/send*() from a single
// gateway thread (the OMS itself is thread-safe for cross-thread callbacks).

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hft {

enum class SessionState { DISCONNECTED, LOGGING_IN, ACTIVE, LOGGING_OUT };

struct GatewayConfig {
    std::string host, senderCompId, targetCompId;
    int         port          = 9000;
    int         heartbeatSecs = 30;
};

// Outbound order intent as the gateway sees it. Distinct from hft::OrderRequest
// (which the strategy/risk path uses): the gateway assigns FIX tags and a
// boolean side rather than hft::Side.
struct FixOrderRequest {
    uint64_t    clOrdId = 0;
    std::string symbol;
    bool        buy     = true;
    double      px      = 0.0;
    long        qty     = 0;
};

// Parsed FIX ExecutionReport (35=8). Distinct from fills_processor::ExecReport;
// ordStatus is the raw FIX tag 39 (0=New,1=Partial,2=Filled,4=Cancelled,8=Rejected).
struct FixExecReport {
    uint64_t    clOrdId   = 0;
    std::string execId;
    int         ordStatus = 0;
    double      lastPx    = 0.0;
    long        lastQty   = 0;
};

using FixFields   = std::unordered_map<int, std::string>;
using OmsCallback = std::function<void(const FixExecReport&)>;

class GatewaySession {
public:
    GatewaySession(GatewayConfig cfg, OmsCallback cb)
        : cfg_(std::move(cfg)), cb_(std::move(cb)) {}

    // ----- session lifecycle -------------------------------------------
    bool connect() {
        if (state_ != SessionState::DISCONNECTED)
            return false;
        state_ = SessionState::LOGGING_IN;
        // Logon: 98=EncryptMethod(0=none), 108=HeartBtInt.
        buildAndSend("A", fld(98, "0") + fld(108, std::to_string(cfg_.heartbeatSecs)));
        return true;
    }

    void disconnect() {
        if (state_ == SessionState::DISCONNECTED)
            return;
        state_ = SessionState::LOGGING_OUT;
        buildAndSend("5", std::string());  // Logout
        state_ = SessionState::DISCONNECTED;
    }

    // ----- outbound orders ---------------------------------------------
    uint64_t sendOrder(const FixOrderRequest& req) {
        const int seq = outSeqNum_;
        sentMessages_.push_back(encodeNOS(req, seq));
        ++outSeqNum_;
        return static_cast<uint64_t>(seq);
    }

    bool sendCancel(uint64_t clOrdId, const std::string& symbol, bool buy, long qty) {
        if (state_ == SessionState::DISCONNECTED)
            return false;
        buildAndSend("F",
                     fld(11, std::to_string(clOrdId)) +
                     fld(55, symbol) +
                     fld(54, buy ? "1" : "2") +
                     fld(38, std::to_string(qty)));
        return true;
    }

    void sendHeartbeat() { buildAndSend("0", std::string()); }

    // ----- inbound data ------------------------------------------------
    void onData(const char* buf, size_t len) {
        rxBuf_.append(buf, len);
        extractAndDispatch();
    }

    // ----- pure encode / parse helpers (public per spec) ---------------
    std::string encodeNewOrderSingle(const FixOrderRequest& req) {
        return encodeNOS(req, outSeqNum_);
    }

    FixExecReport parseExecReport(const FixFields& fields) {
        auto get = [&](int tag) -> std::string {
            auto it = fields.find(tag);
            return it == fields.end() ? std::string() : it->second;
        };

        FixExecReport r{};
        r.clOrdId   = toU64(get(11));
        r.execId    = get(17);
        r.ordStatus = toInt(get(39));
        r.lastPx    = toDouble(get(31));
        r.lastQty   = toLong(get(32));
        return r;
    }

    FixFields parseFixMsg(const std::string& raw) {
        FixFields f;
        size_t i = 0;
        while (i < raw.size()) {
            size_t soh = raw.find(SOH, i);
            if (soh == std::string::npos)
                soh = raw.size();
            const std::string field = raw.substr(i, soh - i);
            const size_t eq = field.find('=');
            if (eq != std::string::npos) {
                const int tag = toInt(field.substr(0, eq));
                if (tag != 0 || field.substr(0, eq) == "0")
                    f[tag] = field.substr(eq + 1);
            }
            i = soh + 1;
        }
        return f;
    }

    // Sum of every byte up to (and including) the SOH before "10=", mod 256.
    uint8_t computeChecksum(const std::string& msg) {
        const std::string csTag = std::string(1, SOH) + "10=";
        const size_t p = msg.rfind(csTag);
        const size_t end = (p == std::string::npos) ? msg.size() : p + 1;
        unsigned sum = 0;
        for (size_t i = 0; i < end; ++i)
            sum += static_cast<unsigned char>(msg[i]);
        return static_cast<uint8_t>(sum % 256);
    }

    // ----- accessors (testability) -------------------------------------
    SessionState state()      const { return state_; }
    int          outSeqNum()  const { return outSeqNum_; }
    int          inSeqNum()   const { return inSeqNum_; }
    const std::vector<std::string>& sent() const { return sentMessages_; }
    void setSendingTime(std::string t) { sendingTime_ = std::move(t); }

private:
    static constexpr char SOH = '\x01';

    static std::string fld(int tag, const std::string& val) {
        return std::to_string(tag) + "=" + val + SOH;
    }

    static int      toInt(const std::string& s)    { return s.empty() ? 0   : std::stoi(s); }
    static long     toLong(const std::string& s)   { return s.empty() ? 0   : std::stol(s); }
    static uint64_t toU64(const std::string& s)    { return s.empty() ? 0u  : std::stoull(s); }
    static double   toDouble(const std::string& s) { return s.empty() ? 0.0 : std::stod(s); }

    // Prepend 8/9, append 10 (checksum) around a fully-formed body.
    std::string finalize(const std::string& body) const {
        const std::string header = fld(8, "FIX.4.2") + fld(9, std::to_string(body.size()));
        const std::string prefix = header + body;
        unsigned sum = 0;
        for (unsigned char c : prefix)
            sum += c;
        char cs[4];
        std::snprintf(cs, sizeof cs, "%03u", sum % 256);
        return prefix + fld(10, cs);
    }

    std::string sessionHeader(const std::string& msgType, int seq) const {
        return fld(35, msgType) +
               fld(49, cfg_.senderCompId) +
               fld(56, cfg_.targetCompId) +
               fld(34, std::to_string(seq)) +
               fld(52, sendingTime_);
    }

    std::string encodeNOS(const FixOrderRequest& req, int seq) const {
        char px[32];
        std::snprintf(px, sizeof px, "%.2f", req.px);
        const std::string body =
            sessionHeader("D", seq) +
            fld(11, std::to_string(req.clOrdId)) +
            fld(55, req.symbol) +
            fld(54, req.buy ? "1" : "2") +
            fld(38, std::to_string(req.qty)) +
            fld(44, px) +
            fld(40, "2") +
            fld(59, "0");
        return finalize(body);
    }

    std::string buildAndSend(const std::string& msgType, const std::string& extra) {
        const int seq = outSeqNum_++;
        const std::string msg = finalize(sessionHeader(msgType, seq) + extra);
        sentMessages_.push_back(msg);
        return msg;
    }

    void sendResendRequest(int beginSeq, int endSeq) {
        buildAndSend("2", fld(7, std::to_string(beginSeq)) +
                          fld(16, std::to_string(endSeq)));
    }

    void extractAndDispatch() {
        const std::string csTag = std::string(1, SOH) + "10=";
        while (true) {
            const size_t start = rxBuf_.find("8=");
            if (start == std::string::npos) { rxBuf_.clear(); return; }

            const size_t cs = rxBuf_.find(csTag, start);
            if (cs == std::string::npos) {
                if (start > 0) rxBuf_.erase(0, start);
                return;
            }
            const size_t endSoh = rxBuf_.find(SOH, cs + csTag.size());
            if (endSoh == std::string::npos) {
                if (start > 0) rxBuf_.erase(0, start);
                return;
            }

            const std::string msg = rxBuf_.substr(start, endSoh - start + 1);
            rxBuf_.erase(0, endSoh + 1);
            handleMessage(parseFixMsg(msg));
        }
    }

    void handleMessage(const FixFields& f) {
        const auto typeIt = f.find(35);
        if (typeIt == f.end())
            return;
        const std::string& type = typeIt->second;

        // Sequence-number gap detection (skips admin msgs w/o 34).
        int seq = 0;
        const auto seqIt = f.find(34);
        if (seqIt != f.end())
            seq = toInt(seqIt->second);

        if (seq > inSeqNum_) {
            sendResendRequest(inSeqNum_, seq - 1);
            return;
        }
        if (seq != 0 && seq < inSeqNum_)
            return;  // duplicate / already processed

        if (type == "A") {
            state_ = SessionState::ACTIVE;
        } else if (type == "1") {
            sendHeartbeat();                        // TestRequest -> respond
        } else if (type == "5") {
            state_ = SessionState::DISCONNECTED;     // Logout
        } else if (type == "8") {
            if (cb_) cb_(parseExecReport(f));        // ExecutionReport
        }
        // type "0" (Heartbeat) and "2" (ResendRequest) need no action here.

        if (seq == inSeqNum_)
            ++inSeqNum_;
    }

    GatewayConfig cfg_;
    OmsCallback   cb_;
    SessionState  state_       = SessionState::DISCONNECTED;
    int           outSeqNum_   = 1;
    int           inSeqNum_    = 1;
    std::string   sendingTime_ = "20240101-12:00:00";
    std::string   rxBuf_;
    std::vector<std::string> sentMessages_;
};

}  // namespace hft
