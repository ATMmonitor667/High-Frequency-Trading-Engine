#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "../../src/execution/order_gateway/order_gateway.h"

// =============================================================================
// Standalone unit tests for the FIX GatewaySession (no external framework).
// Adapted from the original FIX_GATEWAY_TEST harness.
//
// Build & run (MSVC):
//   cl /EHsc /std:c++17 /Fe:test_order_gateway.exe tests\unit\test_order_gateway.cpp
//
// Build & run (g++ 7+):
//   g++ -std=c++17 tests/unit/test_order_gateway.cpp -o test_gw && ./test_gw
//
// (Header-only component, so no extra .cpp is needed on the command line.)
// =============================================================================

using namespace hft;

namespace {

std::string visible(std::string s) {
    for (char& c : s)
        if (c == '\x01') c = '|';
    return s;
}

}  // namespace

int main() {
    int passed = 0, total = 0;
    auto check = [&](const char* name, bool ok) {
        ++total;
        if (ok) ++passed;
        std::cout << name << ": " << (ok ? "PASS" : "FAIL") << '\n';
    };

    GatewayConfig cfg;
    cfg.senderCompId = "CLIENT";
    cfg.targetCompId = "EXCHANGE";

    // Example 1: encode NewOrderSingle and verify structure + checksum.
    {
        GatewaySession g(cfg, nullptr);
        const std::string msg =
            g.encodeNewOrderSingle({42, "AAPL", true, 150.0, 100});
        const FixFields f = g.parseFixMsg(msg);

        const bool structural =
            f.at(8) == "FIX.4.2" && f.at(35) == "D" && f.at(11) == "42" &&
            f.at(55) == "AAPL"   && f.at(54) == "1" && f.at(38) == "100" &&
            f.at(44) == "150.00" && f.at(40) == "2" && f.at(59) == "0";

        const bool csOk =
            std::stoi(f.at(10)) == static_cast<int>(g.computeChecksum(msg));

        const size_t bodyStart = msg.find("35=");
        const size_t bodyEnd   = msg.rfind(std::string(1, '\x01') + "10=") + 1;
        const bool lenOk = std::stoul(f.at(9)) == (bodyEnd - bodyStart);

        check("Example 1 (encode NOS)", structural && csOk && lenOk);
        std::cout << "  " << visible(msg) << '\n';
    }

    // sendOrder assigns & advances the outbound seqnum.
    {
        GatewaySession g(cfg, nullptr);
        const uint64_t s1 = g.sendOrder({1, "AAPL", true, 10.0, 5});
        const uint64_t s2 = g.sendOrder({2, "MSFT", false, 20.0, 7});
        check("sendOrder seqnum advance",
              s1 == 1 && s2 == 2 && g.outSeqNum() == 3 && g.sent().size() == 2);
    }

    // Example 2: parse an ExecReport (SOH-delimited).
    {
        GatewaySession g(cfg, nullptr);
        const char S = '\x01';
        std::string raw = std::string("8=FIX.4.2") + S + "35=8" + S + "11=42" + S +
                          "39=2" + S + "32=100" + S + "31=150.0" + S +
                          "17=EXEC001" + S;
        const FixExecReport r = g.parseExecReport(g.parseFixMsg(raw));
        check("Example 2 (parse ExecReport)",
              r.clOrdId == 42 && r.ordStatus == 2 && r.lastQty == 100 &&
              r.lastPx == 150.0 && r.execId == "EXEC001");
    }

    // onData dispatches a well-formed ExecReport to the OMS callback.
    {
        FixExecReport got{};
        bool fired = false;
        GatewaySession g(cfg, [&](const FixExecReport& r){ got = r; fired = true; });

        const char S = '\x01';
        std::string body = std::string("35=8") + S + "34=1" + S + "11=7" + S +
                           "39=2" + S + "32=50" + S + "31=99.5" + S +
                           "17=E1" + S;
        std::string full = std::string("8=FIX.4.2") + S + "9=" +
                           std::to_string(body.size()) + S + body + "10=000" + S;
        g.onData(full.data(), full.size());
        check("onData dispatch + seq advance",
              fired && got.clOrdId == 7 && got.lastQty == 50 && g.inSeqNum() == 2);
    }

    // Sequence gap triggers a ResendRequest (35=2).
    {
        GatewaySession g(cfg, nullptr);
        const char S = '\x01';
        auto wrap = [&](const std::string& b){
            return std::string("8=FIX.4.2") + S + "9=" +
                   std::to_string(b.size()) + S + b + "10=000" + S;
        };
        std::string m1 = wrap(std::string("35=0") + S + "34=1" + S);
        std::string m3 = wrap(std::string("35=0") + S + "34=3" + S);
        g.onData(m1.data(), m1.size());
        g.onData(m3.data(), m3.size());

        bool resendSent = false;
        for (const auto& s : g.sent())
            if (g.parseFixMsg(s).count(35) && g.parseFixMsg(s).at(35) == "2")
                resendSent = true;
        check("seqnum gap -> ResendRequest", resendSent && g.inSeqNum() == 2);
    }

    // connect() logs on and transitions state.
    {
        GatewaySession g(cfg, nullptr);
        const bool ok = g.connect();
        check("connect -> LOGGING_IN + Logon",
              ok && g.state() == SessionState::LOGGING_IN &&
              g.sent().size() == 1 &&
              g.parseFixMsg(g.sent()[0]).at(35) == "A");
    }

    std::cout << "\n" << passed << "/" << total << " tests passed\n";
    return passed == total ? 0 : 1;
}
