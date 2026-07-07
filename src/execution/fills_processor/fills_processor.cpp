#include "fills_processor.h"

namespace hft {

// ==========================================================================
// Journal helpers
// ==========================================================================
// The journal is one JSON object per line, written by hand. These helpers do a
// small, tolerant reverse parse: they locate a "key" and read the value that
// follows it. They are deliberately forgiving -- a missing or malformed field
// falls back to the supplied default rather than throwing, so a partially
// written final line (torn write on crash) degrades gracefully.
//
// NOTE: the parser is substring-based, so it assumes fields are well-formed and
// that string values do not themselves contain a substring matching another
// key. That holds for everything this processor writes.
namespace {

const char* execTypeName(ExecType t) {
    switch (t) {
        case EXEC_NEW_ACK:      return "EXEC_NEW_ACK";
        case EXEC_PARTIAL_FILL: return "EXEC_PARTIAL_FILL";
        case EXEC_FULL_FILL:    return "EXEC_FULL_FILL";
        case EXEC_CANCELLED:    return "EXEC_CANCELLED";
        case EXEC_REJECTED:     return "EXEC_REJECTED";
        default:                return "EXEC_NEW_ACK";
    }
}

// Locate the quoted key token, e.g. "clOrdId", within the line.
std::size_t findKey(const std::string& line, const char* key) {
    std::string quoted = "\"";
    quoted += key;
    quoted += "\"";
    return line.find(quoted);
}

// Read a quoted string value that follows the given key.
std::string parseString(const std::string& line, const char* key) {
    std::size_t p = findKey(line, key);
    if (p == std::string::npos)
        return "";

    p = line.find(':', p);
    if (p == std::string::npos)
        return "";

    p = line.find('"', p);
    if (p == std::string::npos)
        return "";

    const std::size_t q = line.find('"', p + 1);
    if (q == std::string::npos)
        return "";

    return line.substr(p + 1, q - (p + 1));
}

// Read a JSON true/false following the key; fall back to def if absent.
bool parseBool(const std::string& line, const char* key, bool def) {
    std::size_t p = findKey(line, key);
    if (p == std::string::npos)
        return def;

    p = line.find(':', p);
    if (p == std::string::npos)
        return def;

    const std::size_t q = line.find_first_not_of(" \t", p + 1);
    if (q == std::string::npos)
        return def;

    if (line.compare(q, 4, "true") == 0)
        return true;
    if (line.compare(q, 5, "false") == 0)
        return false;
    return def;
}

// Read a signed integer following the key.
int64_t parseI64(const std::string& line, const char* key, int64_t def) {
    std::size_t p = findKey(line, key);
    if (p == std::string::npos)
        return def;

    p = line.find(':', p);
    if (p == std::string::npos)
        return def;

    const std::size_t q = line.find_first_of("-0123456789", p + 1);
    if (q == std::string::npos)
        return def;

    std::size_t e = q + 1;
    while (e < line.size() && line[e] >= '0' && line[e] <= '9')
        ++e;

    try {
        return std::stoll(line.substr(q, e - q));
    } catch (...) {
        return def;
    }
}

uint64_t parseU64(const std::string& line, const char* key, uint64_t def) {
    const int64_t v = parseI64(line, key, static_cast<int64_t>(def));
    return v < 0 ? def : static_cast<uint64_t>(v);
}

long parseLong(const std::string& line, const char* key, long def) {
    return static_cast<long>(parseI64(line, key, static_cast<int64_t>(def)));
}

// Read a floating-point value following the key (handles sign / exponent).
double parseDouble(const std::string& line, const char* key, double def) {
    std::size_t p = findKey(line, key);
    if (p == std::string::npos)
        return def;

    p = line.find(':', p);
    if (p == std::string::npos)
        return def;

    const std::size_t q = line.find_first_of("-0123456789", p + 1);
    if (q == std::string::npos)
        return def;

    std::size_t e = q + 1;
    while (e < line.size()) {
        const char c = line[e];
        const bool partOfNumber = (c >= '0' && c <= '9') ||
                                  c == '.' || c == 'e' || c == 'E' ||
                                  c == '+' || c == '-';
        if (!partOfNumber)
            break;
        ++e;
    }

    try {
        return std::stod(line.substr(q, e - q));
    } catch (...) {
        return def;
    }
}

// The "type" field may be journaled as a string (current format) or, for
// forward tolerance, as an integer index; handle both.
ExecType parseType(const std::string& line) {
    std::size_t p = findKey(line, "type");
    if (p == std::string::npos)
        return EXEC_NEW_ACK;

    p = line.find(':', p);
    if (p == std::string::npos)
        return EXEC_NEW_ACK;

    const std::size_t q = line.find_first_not_of(" \t", p + 1);
    if (q == std::string::npos)
        return EXEC_NEW_ACK;

    if (line[q] == '"') {
        const std::string t = parseString(line, "type");
        if (t == "EXEC_NEW_ACK")      return EXEC_NEW_ACK;
        if (t == "EXEC_PARTIAL_FILL") return EXEC_PARTIAL_FILL;
        if (t == "EXEC_FULL_FILL")    return EXEC_FULL_FILL;
        if (t == "EXEC_CANCELLED")    return EXEC_CANCELLED;
        if (t == "EXEC_REJECTED")     return EXEC_REJECTED;
        return EXEC_NEW_ACK;
    }

    int64_t v = parseI64(line, "type", 0);
    if (v < 0) v = 0;
    if (v > 4) v = 4;
    return static_cast<ExecType>(v);
}

// Parse one journal line into an ExecReport.
ExecReport parseReport(const std::string& line) {
    ExecReport r{};
    r.clOrdId    = parseU64(line, "clOrdId", 0);
    r.strategyId = parseString(line, "strategyId");
    r.symbol     = parseString(line, "symbol");
    r.buy        = parseBool(line, "buy", true);
    r.type       = parseType(line);
    r.fillPx     = parseDouble(line, "fillPx", 0.0);
    r.arrivalMid = parseDouble(line, "arrivalMid", 0.0);
    r.fillQty    = parseLong(line, "fillQty", 0);
    r.isMaker    = parseBool(line, "isMaker", false);
    r.tsNs       = parseI64(line, "tsNs", 0);
    return r;
}

}  // namespace

// ==========================================================================
// FillsProcessor
// ==========================================================================

FillsProcessor::FillsProcessor(const std::string& logPath) {
    // Open in append mode (creates the file if it does not yet exist). Full
    // double precision so prices survive the write/replay round-trip exactly.
    log_.open(logPath.c_str(), std::ios::out | std::ios::app);
    log_.precision(17);

    replayLog(logPath);
}

void FillsProcessor::onExecReport(const ExecReport& r) {
    std::lock_guard<std::mutex> lock(mu_);

    // 1) Persist and flush every event so the journal is crash-durable.
    log_ << "{"
         << "\"clOrdId\":"    << r.clOrdId                    << ","
         << "\"strategyId\":\"" << r.strategyId << "\","
         << "\"symbol\":\""   << r.symbol       << "\","
         << "\"buy\":"        << (r.buy ? "true" : "false")   << ","
         << "\"type\":\""     << execTypeName(r.type) << "\","
         << "\"fillPx\":"     << r.fillPx                     << ","
         << "\"arrivalMid\":" << r.arrivalMid                 << ","
         << "\"fillQty\":"    << r.fillQty                    << ","
         << "\"isMaker\":"    << (r.isMaker ? "true" : "false") << ","
         << "\"tsNs\":"       << r.tsNs
         << "}\n";
    log_.flush();

    // 2) Only fills move statistics and PnL.
    applyFill_(r);
}

FillSummary FillsProcessor::getSummary(uint64_t clOrdId) const {
    std::lock_guard<std::mutex> lock(mu_);

    auto it = orders_.find(clOrdId);
    if (it == orders_.end())
        return {clOrdId, 0.0, 0, 0.0};

    const OrderRecord& o = it->second;

    const double avgFillPx =
        (o.cumQty > 0) ? (o.sumPxQty / static_cast<double>(o.cumQty)) : 0.0;

    // Slippage in bps, signed so positive is always adverse: a buy filled above
    // the arrival mid, or a sell filled below it, both yield a positive cost.
    double slippage = 0.0;
    if (o.cumQty > 0 && o.arrivalMid > 0.0) {
        const double sign = o.buy ? +1.0 : -1.0;
        slippage = (avgFillPx - o.arrivalMid) / o.arrivalMid * 10000.0 * sign;
    }

    return {clOrdId, avgFillPx, o.cumQty, slippage};
}

StrategyPnL FillsProcessor::getPnL(const std::string& strategyId) const {
    std::lock_guard<std::mutex> lock(mu_);

    auto it = stratPnL_.find(strategyId);
    if (it == stratPnL_.end())
        return {strategyId, 0.0, 0.0, 0};

    return it->second;
}

void FillsProcessor::replayLog(const std::string& path) {
    std::lock_guard<std::mutex> lock(mu_);

    // Idempotent rebuild: start from a clean slate every time.
    orders_.clear();
    stratPnL_.clear();

    std::ifstream in(path.c_str());
    if (!in.is_open())
        return;

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty())
            continue;

        const ExecReport r = parseReport(line);

        // Skip lines that failed to parse the mandatory identifiers.
        if (r.clOrdId == 0 || r.strategyId.empty())
            continue;

        applyFill_(r);
    }
}

// Shared state update used by both the live path and replay. The caller must
// already hold mu_. Non-fill reports (ack / cancel / reject) are ignored here.
void FillsProcessor::applyFill_(const ExecReport& r) {
    if (r.type != EXEC_PARTIAL_FILL && r.type != EXEC_FULL_FILL)
        return;

    // Per-order accumulation. Capture the arrival mid and side on the first fill.
    OrderRecord& o = orders_[r.clOrdId];
    if (o.cumQty == 0) {
        o.arrivalMid = r.arrivalMid;
        o.buy        = r.buy;
    }
    o.sumPxQty += r.fillPx * static_cast<double>(r.fillQty);
    o.cumQty   += r.fillQty;

    // Per-strategy PnL and fees.
    StrategyPnL& sp = stratPnL_[r.strategyId];
    if (sp.id.empty())
        sp.id = r.strategyId;

    const double notional = r.fillPx * static_cast<double>(r.fillQty);
    const double feeBps    = r.isMaker ? MAKER_FEE_BPS : TAKER_FEE_BPS;

    sp.totalFees   += notional * feeBps / 10000.0;
    sp.realisedPnL += (r.buy ? -1.0 : +1.0) * notional;
    sp.fillCount   += 1;
}

}  // namespace hft
