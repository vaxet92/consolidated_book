#pragma once

#include <optional>
#include <vector>

#include "types.h"
#include "venue_book.h"

namespace market_data {

struct VenueQuote {
    VenueId venue;
    QtyUnits qty;
};

struct ConsolidatedPriceLevel {
    PriceTicks price;
    QtyUnits total_qty;
    std::vector<VenueQuote> venues;  // only venues quoting exactly this price - client decides how to use ties
};

/*  price=0, total_qty=0, venues={} if genuinely no data yet - precondition is the publisher never calls this before the
    first ApplyUpdate
    bool crossed;  // best_bid->price >= best_ask->price. false if either side is empty.
*/
struct ConsolidatedBBO {
    ConsolidatedPriceLevel best_bid;
    ConsolidatedPriceLevel best_ask;
    bool crossed;  // best_bid->price >= best_ask->price. false if either side is empty.
};

// Eager BBO merge (DESIGN_1 §5.2): compares each venue's cached best
// bid/ask, no k-way merge over full depth. O(venues) per call, not O(depth).
ConsolidatedBBO ComputeConsolidatedBBO(const VenueBookArray& books);

}  // namespace market_data
