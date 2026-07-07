#include "tick_store.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace hft {

// ==========================================================================
// Path / directory helpers
// ==========================================================================

std::string TickStore::segPath(const std::string& dir, const std::string& sym) {
    if (dir.empty())
        return sym + ".bin";

    const char last = dir.back();
    if (last == '/' || last == '\\')
        return dir + sym + ".bin";

    return dir + '/' + sym + ".bin";
}

void TickStore::ensureDirExists(const std::string& dir) {
    if (dir.empty())
        return;
    std::filesystem::create_directories(std::filesystem::path(dir));
}

// Return (symbol, path) for every .bin segment already on disk.
std::vector<std::pair<std::string, std::string>>
TickStore::listBinFiles(const std::string& dir) {
    std::vector<std::pair<std::string, std::string>> out;
    if (dir.empty() || !std::filesystem::exists(dir))
        return out;

    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (!entry.is_regular_file())
            continue;
        if (entry.path().extension() != ".bin")
            continue;
        out.push_back({entry.path().stem().string(), entry.path().string()});
    }
    return out;
}

// ==========================================================================
// Binary header helpers
// ==========================================================================

std::uint64_t TickStore::readHeaderCount(std::ifstream& in) {
    std::uint64_t n = 0;
    in.read(reinterpret_cast<char*>(&n), sizeof(n));
    return n;
}

void TickStore::writeHeaderCount(std::ofstream& out, std::uint64_t n) {
    out.write(reinterpret_cast<const char*>(&n), sizeof(n));
}

// ==========================================================================
// Construction -- discover existing segments and their record counts.
// ==========================================================================

TickStore::TickStore(const std::string& dir) : dir_(dir) {
    ensureDirExists(dir_);

    for (const auto& [sym, path] : listBinFiles(dir_)) {
        SegInfo info;
        info.path = path;

        std::ifstream in(info.path, std::ios::binary);
        if (in.good()) {
            in.seekg(0, std::ios::beg);
            info.flushedCount = static_cast<std::size_t>(readHeaderCount(in));
        }

        segs_.emplace(sym, std::move(info));
    }
}

// ==========================================================================
// Segment bookkeeping (caller holds mu_)
// ==========================================================================

TickStore::SegInfo& TickStore::ensureSegUnlocked(const std::string& sym) {
    auto it = segs_.find(sym);
    if (it != segs_.end())
        return it->second;

    SegInfo info;
    info.path = segPath(dir_, sym);
    auto res = segs_.emplace(sym, std::move(info));
    return res.first->second;
}

// ==========================================================================
// Public API
// ==========================================================================

void TickStore::append(const std::string& sym, const Tick& t) {
    std::lock_guard<std::mutex> lock(mu_);

    SegInfo& seg = ensureSegUnlocked(sym);
    seg.writeBuf.push_back(t);

    if (seg.writeBuf.size() >= FLUSH_BATCH)
        flushUnlocked(sym, seg);
}

void TickStore::flush(const std::string& sym) {
    std::lock_guard<std::mutex> lock(mu_);
    SegInfo& seg = ensureSegUnlocked(sym);
    flushUnlocked(sym, seg);
}

// Merge the write buffer with existing on-disk data and rewrite the segment.
//
// NOTE: because the columns are stored contiguously in one file, new ticks
// cannot be appended in place (that would overwrite the price/qty columns).
// Each flush reads the whole segment, merges, and rewrites it -- O(N) per flush.
// For the intended workload (few large flushes at FLUSH_BATCH granularity) this
// is acceptable, but it is not a true O(1) append.
void TickStore::flushUnlocked(const std::string& sym, SegInfo& seg) {
    if (seg.writeBuf.empty())
        return;

    const std::string path = seg.path.empty() ? segPath(dir_, sym) : seg.path;
    seg.path = path;

    std::uint64_t oldN = static_cast<std::uint64_t>(seg.flushedCount);
    std::vector<int64_t> oldTs;
    std::vector<double>  oldPx;
    std::vector<long>    oldQty;

    // Read existing columns (if the file exists and has content).
    {
        std::ifstream in(path, std::ios::binary);
        if (in.good()) {
            in.seekg(0, std::ios::end);
            const auto fileSz = static_cast<std::uint64_t>(in.tellg());
            in.seekg(0, std::ios::beg);

            if (fileSz >= sizeof(std::uint64_t)) {
                oldN = readHeaderCount(in);
                if (oldN > 0) {
                    const std::uint64_t tsOff  = sizeof(std::uint64_t);
                    const std::uint64_t pxOff  = tsOff + oldN * sizeof(int64_t);
                    const std::uint64_t qtyOff = pxOff + oldN * sizeof(double);

                    oldTs.resize(static_cast<std::size_t>(oldN));
                    oldPx.resize(static_cast<std::size_t>(oldN));
                    oldQty.resize(static_cast<std::size_t>(oldN));

                    in.seekg(static_cast<std::streamoff>(tsOff), std::ios::beg);
                    in.read(reinterpret_cast<char*>(oldTs.data()),
                            static_cast<std::streamsize>(oldN * sizeof(int64_t)));

                    in.seekg(static_cast<std::streamoff>(pxOff), std::ios::beg);
                    in.read(reinterpret_cast<char*>(oldPx.data()),
                            static_cast<std::streamsize>(oldN * sizeof(double)));

                    in.seekg(static_cast<std::streamoff>(qtyOff), std::ios::beg);
                    in.read(reinterpret_cast<char*>(oldQty.data()),
                            static_cast<std::streamsize>(oldN * sizeof(long)));
                }
            }
        }
    }

    const std::uint64_t addN = static_cast<std::uint64_t>(seg.writeBuf.size());
    const std::uint64_t newN = oldN + addN;

    std::vector<int64_t> ts;
    std::vector<double>  px;
    std::vector<long>    qty;
    ts.reserve(static_cast<std::size_t>(newN));
    px.reserve(static_cast<std::size_t>(newN));
    qty.reserve(static_cast<std::size_t>(newN));

    for (std::size_t i = 0; i < oldTs.size(); ++i) {
        ts.push_back(oldTs[i]);
        px.push_back(oldPx[i]);
        qty.push_back(oldQty[i]);
    }
    for (const Tick& t : seg.writeBuf) {
        ts.push_back(t.tsNs);
        px.push_back(t.price);
        qty.push_back(t.qty);
    }

    // Keep timestamps sorted so query()'s binary search is valid. Only pay the
    // sort cost if the merged column is actually out of order.
    if (ts.size() > 1 && !std::is_sorted(ts.begin(), ts.end())) {
        std::vector<std::size_t> idx(ts.size());
        for (std::size_t i = 0; i < idx.size(); ++i)
            idx[i] = i;
        std::sort(idx.begin(), idx.end(),
                  [&](std::size_t a, std::size_t b) { return ts[a] < ts[b]; });

        std::vector<int64_t> ts2(ts.size());
        std::vector<double>  px2(ts.size());
        std::vector<long>    qty2(ts.size());
        for (std::size_t i = 0; i < idx.size(); ++i) {
            ts2[i]  = ts[idx[i]];
            px2[i]  = px[idx[i]];
            qty2[i] = qty[idx[i]];
        }
        ts.swap(ts2);
        px.swap(px2);
        qty.swap(qty2);
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    writeHeaderCount(out, newN);
    if (newN > 0) {
        out.write(reinterpret_cast<const char*>(ts.data()),
                  static_cast<std::streamsize>(newN * sizeof(int64_t)));
        out.write(reinterpret_cast<const char*>(px.data()),
                  static_cast<std::streamsize>(newN * sizeof(double)));
        out.write(reinterpret_cast<const char*>(qty.data()),
                  static_cast<std::streamsize>(newN * sizeof(long)));
    }
    out.flush();

    seg.flushedCount = static_cast<std::size_t>(newN);
    seg.writeBuf.clear();
}

std::vector<Tick> TickStore::query(const std::string& sym, int64_t fromNs, int64_t toNs) {
    flush(sym);  // never return stale buffered-only data

    std::lock_guard<std::mutex> lock(mu_);

    auto it = segs_.find(sym);
    if (it == segs_.end())
        return {};

    const std::string path =
        it->second.path.empty() ? segPath(dir_, sym) : it->second.path;

    std::ifstream in(path, std::ios::binary);
    if (!in.good())
        return {};

    in.seekg(0, std::ios::end);
    const auto fileSz = static_cast<std::uint64_t>(in.tellg());
    in.seekg(0, std::ios::beg);
    if (fileSz < sizeof(std::uint64_t))
        return {};

    const std::uint64_t n = readHeaderCount(in);
    if (n == 0)
        return {};

    const std::uint64_t tsOff  = sizeof(std::uint64_t);
    const std::uint64_t pxOff  = tsOff + n * sizeof(int64_t);
    const std::uint64_t qtyOff = pxOff + n * sizeof(double);

    // Read the timestamp column and binary-search the [from, to) bounds.
    std::vector<int64_t> ts(static_cast<std::size_t>(n));
    in.seekg(static_cast<std::streamoff>(tsOff), std::ios::beg);
    in.read(reinterpret_cast<char*>(ts.data()),
            static_cast<std::streamsize>(n * sizeof(int64_t)));

    auto loIt = std::lower_bound(ts.begin(), ts.end(), fromNs);
    auto hiIt = std::lower_bound(ts.begin(), ts.end(), toNs);

    const std::size_t lo = static_cast<std::size_t>(loIt - ts.begin());
    const std::size_t hi = static_cast<std::size_t>(hiIt - ts.begin());
    if (hi <= lo)
        return {};

    const std::size_t k = hi - lo;
    std::vector<double> px(k);
    std::vector<long>   qty(k);

    in.seekg(static_cast<std::streamoff>(pxOff + static_cast<std::uint64_t>(lo) * sizeof(double)),
             std::ios::beg);
    in.read(reinterpret_cast<char*>(px.data()),
            static_cast<std::streamsize>(k * sizeof(double)));

    in.seekg(static_cast<std::streamoff>(qtyOff + static_cast<std::uint64_t>(lo) * sizeof(long)),
             std::ios::beg);
    in.read(reinterpret_cast<char*>(qty.data()),
            static_cast<std::streamsize>(k * sizeof(long)));

    std::vector<Tick> out;
    out.reserve(k);
    for (std::size_t i = 0; i < k; ++i) {
        Tick t;
        t.tsNs  = ts[lo + i];
        t.price = px[i];
        t.qty   = qty[i];
        out.push_back(t);
    }
    return out;
}

OHLCVBar TickStore::resample(const std::string& sym, int64_t fromNs, int64_t toNs,
                             int64_t barNs) {
    // Single-bar aggregation: the whole [fromNs, toNs) range collapses to one
    // OHLCV bar, so barNs is not used here. (Bucketing into multiple bars would
    // require returning std::vector<OHLCVBar>.)
    (void)barNs;

    auto ticks = query(sym, fromNs, toNs);

    OHLCVBar bar{};
    bar.tsNs = fromNs;
    if (ticks.empty())
        return bar;  // open/high/low/close = 0, volume = 0

    bar.open  = ticks.front().price;
    bar.high  = ticks.front().price;
    bar.low   = ticks.front().price;
    bar.close = ticks.front().price;

    long vol = 0;
    for (const Tick& t : ticks) {
        if (t.price > bar.high) bar.high = t.price;
        if (t.price < bar.low)  bar.low  = t.price;
        bar.close = t.price;
        vol += t.qty;
    }
    bar.volume = vol;
    return bar;
}

}  // namespace hft
