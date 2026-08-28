#pragma once

#include <vector>

#include "types.h"
#include "venue_book.h"

namespace market_data {
namespace consolidated {

struct VenueQuote {
    VenueId venue;
    QtyUnits qty;
};

struct ConsolidatedPriceLevel {
    // Default member initializers, not just relying on the caller to
    // zero-init: PriceTicks/QtyUnits are plain uint64_t, and `BBO result;`
    // (default-init, not value-init) leaves fundamental-type members as
    // indeterminate garbage, not 0. The merge logic in ComputeBBO depends
    // on price==0 meaning "nothing seen yet" - without these, that check
    // silently reads garbage instead.
    PriceTicks price = 0;
    QtyUnits total_qty = 0;
    std::vector<VenueQuote> venues;  // only venues quoting exactly this price - client decides how to use ties
};

// price=0, total_qty=0, venues={} if genuinely no data yet - precondition is
// the publisher never calls this before the first ApplyUpdate.
struct BBO {
    ConsolidatedPriceLevel best_bid;
    ConsolidatedPriceLevel best_ask;
    bool crossed = false;  // best_bid.price >= best_ask.price. false if either side is empty.
};

// Eager BBO merge (DESIGN_1 §5.2): compares each venue's cached best
// bid/ask, no k-way merge over full depth. O(venues) per call, not O(depth).
// Named ComputeBBO, not BBO, to avoid colliding with the struct above -
// C++ lets a function and a struct share a name, but the function then
// hides the struct name for lookup inside its own body, which breaks
// `BBO result;` in the implementation.
BBO ComputeBBO(const VenueBookArray& books);

// Consolidates the fast-BBO stream quotes (DESIGN_1 §4.4 option 1). Same
// max-bid / min-ask / tie-attribution logic as ComputeBBO, but sourced
// from the venues' own top-of-book channels instead of our depth books:
// lower latency, and independent of depth-book sync state.
// VenueQuoteArray comes from types.h - unqualified here because
// `consolidated` is nested inside market_data.
//
// Full scan over all venues, O(venues). Used to build from scratch, as the
// rescan fallback inside UpdateBBOWithQuote, and as the test oracle the
// incremental path is checked against - the same role std::map plays for
// VenueBook (§5.1).
BBO ComputeBBOFromQuotes(const VenueQuoteArray& quotes);

// Incremental: folds one venue's new quote into an existing BBO.
// O(venues at the best level) - typically 1 - instead of O(venues), falling
// back to a full rescan of one side only when the last venue leaves that
// side's best level. Built for larger venue counts; at 3 venues it is a
// wash with ComputeBBOFromQuotes.
//
// PRECONDITION: `quotes` must ALREADY contain `update` - the caller stores
// the quote before calling this, because the rescan path re-reads it from
// there. Passing a stale array silently produces a wrong book, and only on
// the rare rescan path, so the bug would hide for a long time.
void UpdateBBOWithQuote(BBO& current, const BboQuote& update, const VenueQuoteArray& quotes);

// TODO: not built yet. Will follow once §8.2/§8.3's band math lands in
// md_core - ComputeVolumeBands(...), ComputePriceBands(...).

}  // namespace consolidated
}  // namespace market_data