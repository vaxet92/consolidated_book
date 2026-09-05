#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "consolidated_bbo.h"
#include "types.h"
#include "venue_book.h"
#include "venue_health.h"

namespace market_data {
namespace consolidated {

// One price level of the merged book, with per-venue attribution and the
// running totals up to and including this level.
//
// Attribution is a FIXED array, not a vector: there can never be more
// contributors than VenueId::COUNT, and a vector here would mean one heap
// allocation per level - ~500 per merge, ~50k/sec at depth-update rates,
// which is exactly what DESIGN_1 §7.5 rules out on a hot path.
struct MergedLevel {
    PriceTicks price = 0;

    // Sized by kMaxVenues, NOT kVenueCount. The merge loop is bounded by
    // Core's runtime venue count, which reaches kMaxVenues (8) - so an
    // enum-sized array here (3) meant the FOURTH venue quoting a given price
    // wrote past the end, corrupting the neighbouring MergedLevel in the
    // vector. Silent, and latent only because exactly three venues run today.
    //
    // KEY: this array's bound and the merge loop's bound are the SAME quantity
    // and must be written as the same constant. They drifted apart once
    // already (§17.6 migrated venue_levels and the loop bounds, and missed
    // this array), which is why the write site also carries a runtime guard
    // rather than trusting the two to stay in step.
    //
    // MEASURED, and the reason this is inline rather than out of line: an
    // attempt to pack attribution into a side array (MergedLevel 176 -> 48
    // bytes) made the merge 40% SLOWER - ratio 1.13 -> 1.57, ~11 us -> ~15.8
    // us. push_back per contributor costs a size+capacity load, a branch, and
    // a store to a second write stream, ~3000 times per merge; the inline
    // store wins because this level is already in L1 from being written a
    // moment ago. The out-of-line layout should only win in the BAND WALK,
    // which reads none of this - and nothing measures the band walk yet, so
    // that gain is unproven and the loss is not. See becnhmark_results.md.
    std::array<VenueQuote, kMaxVenues> venues{};
    uint8_t venue_count = 0;

    // Prefix sums, filled during the merge. Computing them here rather than
    // inside each band function means the accumulation happens once instead
    // of once per band, and turns every band query into "find the crossing
    // level, read two numbers, divide".
    //
    // This level's OWN quantity is deliberately not stored - it is the
    // difference against the previous level's cum_qty. Use LevelQty() rather
    // than open-coding that subtraction, so the index-0 base case lives in
    // one place.
    QtyUnits cum_qty = 0;
    Notional cum_notional = 0;  // running sum of price x qty, RAW scale (x 1e16)
};

// venue_count is uint8_t, so the venue cap must fit in it. Failing here beats
// silently wrapping to 0 contributors on a level.
static_assert(kMaxVenues <= 255, "MergedLevel::venue_count is uint8_t");

// Quantity at `side[index]` alone. Keeps the index-0 boundary condition in
// one place instead of at every call site.
inline QtyUnits LevelQty(const std::vector<MergedLevel>& side, size_t index) {
    return index == 0 ? side[0].cum_qty : side[index].cum_qty - side[index - 1].cum_qty;
}

// The k-way merge of every venue's depth (DESIGN_1 §5.2, the "lazy" path):
// rebuilt at publish time, never maintained per update. Sorted vectors, not
// maps - this is written once and then read strictly in order, so a map's
// O(log n) key lookup would be paid for and never used.
struct Book {
    std::vector<MergedLevel> bids;  // descending price - bids[0] is the best bid
    std::vector<MergedLevel> asks;  // ascending price  - asks[0] is the best ask

    // Monotonic arrival stamp of the update that triggered this merge, copied
    // straight from BookUpdate::recv_mono_ns.
    //
    // KEY: Core does not read a clock - it forwards a number it was given, so
    // md_core keeps its no-I/O, no-clock rule. The publisher, which is allowed
    // a clock, subtracts this from its own reading to get the full pipeline
    // latency: provider parse -> handoff -> book apply -> merge -> published.
    //
    // That end-to-end figure is the ONLY fair way to compare the current
    // mutex handoff against the per-venue SPSC queues (DESIGN_1 §7.2).
    // Timing ApplyUpdate itself would not work: after the change it becomes a
    // queue push that returns immediately, which measures work MOVING to
    // another thread rather than work getting cheaper.
    //
    // Not only instrumentation - this is also the honest input for the wire's
    // server_ts_ns and for answering "how old is this snapshot?".
    int64_t source_mono_ns = 0;

    // Bid-side depth of each venue's book at the moment of this merge, indexed
    // by VenueId. Zero for a venue that is not configured.
    //
    // Diagnostic first: the live publish latency came in ~10x above what
    // bench_md_core predicted, and the leading suspect is that the benchmark
    // modelled 1000-level maps (~16 KB, comfortably in L2) while the real
    // Binance book grows WITHOUT BOUND - its diff stream reports changes
    // across a $30,000 price range and nothing trims them. A megabyte-scale
    // red-black tree turns every `++it` in the merge into a cache miss, and
    // the merge is ~100% traversal (§7.2).
    //
    // Needs no clock, so it costs md_core nothing architecturally - it is read
    // while the lock is already held, from state already in hand.
    //
    // Also the natural input for `VenueStatus` on the wire, which currently
    // reports no depth at all.
    //
    // Sized by kMaxVenues and indexed by SLOT, like every other per-venue
    // array (DESIGN.md §17.6). Pure capacity - the entries are counts, with no
    // venue identity in them, so an unused slot reads 0, which is already what
    // "this venue contributed nothing" means.
    std::array<uint32_t, kMaxVenues> venue_levels{};

    // Clears without releasing capacity, so a Book reused across publishes
    // stops allocating after warm-up (§7.5).
    void Clear() {
        bids.clear();
        asks.clear();
    }
};


// §5.2: N must cover the deepest question asked - the 50M notional band and
// the 1000bps price band.
//
// Sized to the depth the venues actually publish, so nothing already in
// memory is discarded: Bybit orderbook.50 gives 50 levels, OKX books gives
// 400, Binance's REST snapshot up to 1000 - about 1450 merged. At 500 we
// were throwing away roughly two thirds of what we already had.
//
// Bands will STILL exhaust this, and that is correct rather than a bug:
// 1000bps is ~10% away on BTCUSDT and no venue publishes anywhere near that
// far. Exhaustion is reported through insufficient_depth, never hidden.
inline constexpr size_t kDefaultMaxDepth = 1500;

// Merges into `out`, reusing its buffers. Caller keeps one Book alive across
// publishes rather than constructing a fresh one each time.
//
// `health` is the staleness verdict per venue (DESIGN_1 §6). A venue that is
// not kLive contributes nothing to the merge.
//
// KEY: a stale venue must be EXCLUDED, not merely reported. The merge takes
// max(bid) and min(ask); a frozen venue never moves, so when the market falls
// it always looks like the best bid and when it rises it always looks like
// the best ask. Staleness is not noise that averages out - the merge actively
// selects for it, so one frozen venue out of three corrupts the output nearly
// every time the market moves.
//
// nullptr admits every venue. That is the correct neutral default for a pure
// merge function: it merges what it is given, and deciding what it is given
// is the caller's policy decision, made in Core where the timestamps live.
// The tests that exercise merge behaviour alone rely on this default, so the
// guarantee that production never forgets to pass it is a Core-level test,
// not this signature.
// `venue_count` bounds every per-venue loop. Pass Core's high-water mark, not
// kVenueCount and not books.size(): the enum bound silently drops any venue
// registered beyond it, and the capacity bound iterates empty slots on a path
// measured in microseconds. A slot whose venue was removed is still counted
// and skipped as a null book - slots are dense, so a removal leaves a hole and
// stopping early would drop every venue above it (DESIGN.md §17.6).
void MergeBooks(const VenueBookArray& books, size_t venue_count, Book& out, size_t max_depth = kDefaultMaxDepth,
                const VenueHealthArray* health = nullptr);

// ---------------------------------------------------------------------------
// Band math (§8.2 / §8.3). Both walk the same prefix-sum book; they differ
// only in the stopping condition - notional reached vs price limit passed.
// ---------------------------------------------------------------------------

// Volume band (§8.2): sweep until `target_notional` of quote currency is
// filled, splitting the final level proportionally.
struct NotionalFill {
    PriceTicks vwap = 0;         // filled_notional / filled_qty
    PriceTicks worst_price = 0;  // last level touched
    QtyUnits filled_qty = 0;
    uint64_t filled_notional = 0;     // USDT x 1e8, same scale as target_notional
    uint32_t level_count = 0;         // levels consumed (the partial one counts)
    bool insufficient_depth = false;  // book ran out before reaching the target
};

// target_notional is USDT x 1e8 (1M -> 100'000'000'000'000).
//
// Accumulation happens in unsigned __int128 at the raw price x qty scale
// (x 1e16): 50M USDT raw is 5e23, which overflows uint64 and a double's
// exact-integer range alike. vwap falls out as 1e16 / 1e8 = 1e8, already a
// correctly scaled PriceTicks.
//
// Exhausting the book is a legitimate answer on BTCUSDT, not an error - the
// partial fill is returned with insufficient_depth set (§5.2).
NotionalFill FillToNotional(const std::vector<MergedLevel>& side, uint64_t target_notional);

// Fills EVERY band in ONE forward walk. The bands are nested
// (1M subset 5M subset 10M subset 25M subset 50M), so crossing a threshold
// just records that band's result and the walk continues - DESIGN_1 §7.3.
// Calling FillToNotional once per band would rewalk the book each time.
//
// `targets` must be sorted ascending. `out` is filled in the same order and
// is reused across publishes rather than reallocated (§7.5).
void FillToNotionalBands(const std::vector<MergedLevel>& side, const std::vector<uint64_t>& targets,
                         std::vector<NotionalFill>& out);

// Price band (§8.3): cumulative liquidity within `bps` of the top of this
// side. Measured from the BBO, per the assignment's literal wording; §8.3
// notes measuring from the mid is the more common convention and is left as
// a config flag, not built.
struct BpsFill {
    PriceTicks vwap = 0;         // cum_notional / cum_qty
    PriceTicks limit_price = 0;  // the bps boundary itself
    QtyUnits cum_qty = 0;
    uint64_t cum_notional = 0;  // USDT x 1e8
    uint32_t level_count = 0;

    // The walk reached the end of the book before crossing limit_price, so
    // the totals are a LOWER BOUND on the liquidity within the band rather
    // than the whole of it. Distinguishing the two matters: on BTCUSDT the
    // wider bands are always truncated, because no venue publishes anything
    // near 10% of depth, and a truncated result otherwise looks identical to
    // a complete one.
    bool insufficient_depth = false;
};

// `is_bid` picks the direction: bids walk DOWN from the best bid to
// best_bid x (1 - bps/10000), asks walk UP to best_ask x (1 + bps/10000).
//
// Unlike FillToNotional there is no partial level - a level is either inside
// the boundary or outside it. Running out of book before reaching the
// boundary is still reported, via insufficient_depth, because a truncated
// total is otherwise indistinguishable from a complete one.
BpsFill FillToBps(const std::vector<MergedLevel>& side, uint32_t bps, bool is_bid);

// One forward walk for every bps band, same reasoning as
// FillToNotionalBands. `bps_bands` must be sorted ascending; `out` is filled
// in the same order and reused across publishes.
void FillToBpsBands(const std::vector<MergedLevel>& side, const std::vector<uint32_t>& bps_bands, bool is_bid,
                    std::vector<BpsFill>& out);

}  // namespace consolidated
}  // namespace market_data
