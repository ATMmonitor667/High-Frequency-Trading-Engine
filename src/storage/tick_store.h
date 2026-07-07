#pragma once

// ==========================================================================
// Tick Data Store  (append-only, column-oriented time-series storage)
// ==========================================================================
// Persists tick data for the write-once / read-many workload: strategies (or a
// recorder) append ticks during the day; the replay / backtester reads ranges
// and resamples them into OHLCV bars offline.
//
//   append(sym, tick) --> [ TickStore ] --> <dir>/<sym>.bin
//   query(sym, from, to)  <-- binary search on the timestamp column
//   resample(...)         <-- OHLCV aggregation over a queried range
//
// On-disk format (one file per symbol, columnar):
//   [ uint64 count ][ int64 ts[count] ][ double px[count] ][ long qty[count] ]
// Columns are stored contiguously so a range scan reads only the timestamp
// column to locate bounds, then the price/qty slices for the hit range.
//
// Ordering: ticks are kept sorted by timestamp. If a caller appends out of
// order, flush() re-sorts all columns together so query()'s binary search stays
// correct. Ranges are half-open: query returns ticks in [fromNs, toNs).
//
// Thread safety: a single mutex serializes append / flush / query, matching the
// locking style of the other execution-side modules.
//
// PORTABILITY NOTE: records are written as raw little-endian native types, and
// qty is a platform `long` (4 bytes on Windows, 8 on 64-bit Linux). Files are
// therefore portable only across same-platform builds -- fine for this project.

#include <cstdint>
#include <iosfwd>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace hft {

struct Tick {
    int64_t tsNs  = 0;
    double  price = 0.0;
    long    qty   = 0;
};

struct OHLCVBar {
    int64_t tsNs   = 0;
    double  open   = 0.0;
    double  high   = 0.0;
    double  low    = 0.0;
    double  close  = 0.0;
    long    volume = 0;
};

class TickStore {
public:
    // Number of buffered ticks that triggers an automatic flush.
    static constexpr std::size_t FLUSH_BATCH = 4096;

    explicit TickStore(const std::string& dir);

    // Buffer a tick for a symbol; auto-flushes once FLUSH_BATCH is reached.
    void append(const std::string& sym, const Tick& t);

    // Persist a symbol's buffered ticks to disk (merging with existing data).
    void flush(const std::string& sym);

    // Return all ticks in [fromNs, toNs), flushing first so no buffered data is
    // missed.
    std::vector<Tick> query(const std::string& sym, int64_t fromNs, int64_t toNs);

    // Aggregate [fromNs, toNs) into a single OHLCV bar. NOTE: barNs is currently
    // unused -- this returns one bar spanning the whole range (see the .cpp for
    // the bucketing caveat).
    OHLCVBar resample(const std::string& sym, int64_t fromNs, int64_t toNs, int64_t barNs);

private:
    struct SegInfo {
        std::string       path;
        std::vector<Tick> writeBuf;
        std::size_t       flushedCount = 0;
    };

    std::string dir_;
    mutable std::mutex mu_;
    std::unordered_map<std::string, SegInfo> segs_;

    // Path helpers.
    static std::string segPath(const std::string& dir, const std::string& sym);
    static void        ensureDirExists(const std::string& dir);
    static std::vector<std::pair<std::string, std::string>> listBinFiles(const std::string& dir);

    // Binary header (record count) helpers.
    static std::uint64_t readHeaderCount(std::ifstream& in);
    static void          writeHeaderCount(std::ofstream& out, std::uint64_t n);

    // Assumes mu_ is held.
    SegInfo& ensureSegUnlocked(const std::string& sym);
    void     flushUnlocked(const std::string& sym, SegInfo& seg);
};

}  // namespace hft
