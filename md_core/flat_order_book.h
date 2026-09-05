#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <ranges>
#include <utility>
#include <vector>

#include "types.h"
#include "types/venue_registry.h"

namespace market_data {

// A venue's depth book stored as two sorted, contiguous vectors instead of two
// red-black trees. Same public API as MapOrderBook, which stays in the tree as the
// permanent test oracle (CLAUDE.md §6) - the two are driven by identical update
// streams and must always agree.
//
// WHY: the merge is the measured bottleneck, and it is almost entirely tree
// WALKING, not merge logic. From becnhmark_results.md, same run, ~4800 node
// visits each:
//
//     merge_full     8500 ns    full merge: selection, attribution, prefix sums
//     iterate_only   9000 ns    walking the same std::maps, NO merge logic
//
// Iterating the trees costs MORE than the entire merge. That is what a
// contiguous layout removes: ~4800 pointer-chases to scattered heap nodes
// become a sequential scan the hardware prefetcher can follow.
//
// KEY: this is DESIGN.md step 16, whose stated condition was "only if step 11
// shows std::map is the bottleneck". Step 11 ran and it does. The decision is
// not being reversed on taste - the gate it was waiting on opened.
class FlatOrderBook {
   public:
    // Same signature as MapOrderBook, deliberately: Core constructs one or the
    // other and nothing else about its construction changes.
    FlatOrderBook(VenueId venue, InstrumentKey instrument);

    void ApplyUpdate(const BookUpdate& update);

    std::optional<std::pair<PriceTicks, QtyUnits>> BestBid() const;
    std::optional<std::pair<PriceTicks, QtyUnits>> BestAsk() const;

    VenueId venue() const { return venue_; }
    InstrumentKey instrument() const { return instrument_; }
    uint64_t last_seq() const { return last_seq_; }

    // Arrival time of the last depth update, on OUR monotonic clock. 0 means
    // NEVER HEARD FROM, which is not the same as stale - see MapOrderBook, the
    // staleness predicate depends on the distinction.
    int64_t last_update_mono_ns() const { return last_update_mono_ns_; }

    // BEST FIRST, matching MapOrderBook::bids()/asks() exactly, so every existing
    // reader - the merge, ComputeBBO, the tests - keeps its semantics and only
    // swaps ->first/->second for .price/.qty.
    //
    // KEY: storage order is the OPPOSITE of this, and stays private to this
    // class. A reverse view costs nothing at runtime (it is a pointer plus a
    // direction) and it means no caller can accidentally depend on the
    // physical layout, which is the thing most likely to change next.
    auto bids() const { return std::views::reverse(bids_); }
    auto asks() const { return std::views::reverse(asks_); }

    // Bytes memmoved by the most recent ApplyUpdate, and cumulatively.
    //
    // KEY: this, not ns/call, is the metric that transfers. A nanosecond figure
    // describes this laptop; bytes moved describes the algorithm and predicts
    // it on any machine. It also isolates the flat book's one real weakness -
    // shifting elements - from cache effects and scheduler noise, which a
    // latency number blends together. The average should approach zero once
    // in-place application lands; the p99 is where a new price at the top of a
    // deep book shows up, and a median hides it completely.
    uint64_t last_bytes_moved() const { return last_bytes_moved_; }
    uint64_t total_bytes_moved() const { return total_bytes_moved_; }

   private:
    // Applies one side's delta. `storage_less` defines the STORAGE order -
    // std::less for bids (ascending, so back() is the highest price),
    // std::greater for asks (descending, so back() is the lowest). Returns the
    // bytes memmoved.
    //
    // KEY: ONE walk, BACKWARD from back(), stopping as soon as the delta is
    // exhausted - so a top-of-book delta never reads the deep end at all. That
    // walk writes matched quantities in place as it goes while counting the
    // levels that enter and leave. When nothing enters or leaves - the common
    // case in a live feed - it is already finished: 0 bytes moved, and a cost
    // set by the DELTA size rather than the book size.
    //
    // That last property is the point. The live Binance book grows without
    // bound (consolidated_book.h), so an apply whose cost scales with the book
    // gets worse the longer the process runs. Scaling with the delta - which
    // the venue bounds for us - is what makes the flat book safe to ship.
    //
    // KEY: committing those quantity writes BEFORE knowing whether the delta
    // also inserts or erases is safe because a BookUpdate is absolute, not
    // incremental (types.h). If the relocation pass does run, it re-reads every
    // value from the delta and writes the same numbers again. Idempotence is
    // what collapses two walks into one.
    template <typename StorageLess>
    uint64_t ApplySide(std::vector<PriceLevel>& side, const std::vector<PriceLevel>& levels, StorageLess storage_less);

    // Rewrites only `side[deepest..end)`, where `deepest` is the lowest index
    // the delta reached. Staged through merged_ and copied back.
    //
    // KEY: an earlier version did this in place, choosing the walk direction
    // from the NET size change - backward when growing, forward when shrinking.
    // That is wrong, and the oracle test caught it. The invariant has to hold at
    // every step, not just at the end:
    //
    //     book  [96, 97, 98, 99]        delta best-first: 100 -> 5, 98 -> 0
    //     inserts 1, erases 1, net 0 -> "backward"
    //     first step writes the new 100 into side[3], which still held 99
    //
    // In the backward pass `write - read` starts at inserts - erases and each
    // insert shrinks it, so meeting the inserts first drives it negative; the
    // forward pass fails the mirror image. ANY delta holding both an insert and
    // an erase can break either direction depending on the order they appear
    // in - and the corruption is silent, because the side stays sorted and the
    // right length. Staging removes the question: the destination is never an
    // input.
    //
    // Still O(region), never O(book) - which is the property that made this
    // worth doing. The cost is two write passes over the region instead of one.
    //
    // `deepest` is safe to under-estimate: passing 0 degenerates to a full
    // rebuild, which is slower but never wrong.
    template <typename StorageLess>
    uint64_t Relocate(std::vector<PriceLevel>& side, size_t deepest, const PriceLevel* base, ptrdiff_t step, size_t m,
                      StorageLess storage_less);

    VenueId venue_;
    InstrumentKey instrument_;
    uint64_t last_seq_ = 0;
    int64_t last_update_mono_ns_ = 0;  // 0 = never received anything

    // REVERSE layout: worst price first, BEST price at back().
    //
    // KEY: top-of-book is where nearly every update lands, and back() is the
    // only end of a vector that is cheap to grow and shrink. With the best
    // price at front() instead, a new best bid on a 1000-level book memmoves
    // ~16 KB (1000 x sizeof(PriceLevel)); at back() it is a push_back and moves
    // NOTHING. A level five deep costs 80 bytes rather than ~15.9 KB.
    //
    // The cost is that every reader walks backwards. That is close to free -
    // hardware prefetchers detect descending strides as well as ascending ones
    // - which is why the expensive end was chosen for the rare direction.
    std::vector<PriceLevel> bids_;  // ascending price:  back() = best bid
    std::vector<PriceLevel> asks_;  // descending price: back() = best ask

    // Staging for a delta that arrives in neither storage order nor its exact
    // reverse. Held as a member, not a local, so the rare unsorted case does
    // not allocate on the hot path after the first time.
    std::vector<PriceLevel> scratch_;

    // Staging for Relocate's merged region. Cannot be scratch_ or `side`: a
    // merge writes while both inputs are still being read, so the destination
    // has to be a third buffer.
    //
    // KEY: this holds the REGION the delta touched, not the whole side - the
    // first version merged and swapped whole sides, which is what made a
    // 5-level delta cost a 1000-level rewrite. An attempt to remove this buffer
    // entirely, by relocating in place, is what introduced the aliasing bug
    // documented on Relocate above. It is here deliberately, not by omission.
    std::vector<PriceLevel> merged_;

    uint64_t last_bytes_moved_ = 0;
    uint64_t total_bytes_moved_ = 0;
};

// Enough for the deepest tier any venue actually publishes to us today
// (Binance 1000). Reserved once in the constructor so a warmed-up book never
// allocates while applying an update.
//
// A deeper configured depth simply grows the vector once and keeps that
// capacity - vectors here are never shrunk, so the allocation happens at most
// a handful of times in the life of the process rather than per message.
inline constexpr size_t kInitialLevelCapacity = 1024;

// The production book array. MapOrderBookArray (the std::map one) still exists and
// is still built - it is the oracle, and the benchmark's comparison arm.
using FlatBookArray = std::array<std::unique_ptr<FlatOrderBook>, kMaxVenues>;

}  // namespace market_data
