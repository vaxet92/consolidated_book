#include "consolidated_bbo.h"

namespace market_data {

ConsolidatedBBO ComputeConsolidatedBBO(const VenueBookArray& books) {
    ConsolidatedBBO result;
    auto& best_bid = result.best_bid;
    auto& best_ask = result.best_ask;

    for (size_t i = 0; i < books.size(); ++i) {
        if (!books[i]) {
            continue;  // venue not configured for this instrument
        }

        VenueId venue = static_cast<VenueId>(i);

        if (auto bid = books[i]->BestBid()) {
            auto [price, qty] = *bid;
            if (0 == best_bid.price || price > best_bid.price) {
                best_bid = ConsolidatedPriceLevel{price, qty, {{venue, qty}}};
            } else if (price == best_bid.price) {
                best_bid.total_qty += qty;
                best_bid.venues.push_back({venue, qty});
            }
        }

        if (auto ask = books[i]->BestAsk()) {
            auto [price, qty] = *ask;
            if (0 == best_ask.price || price < best_ask.price) {
                best_ask = ConsolidatedPriceLevel{price, qty, {{venue, qty}}};
            } else if (price == best_ask.price) {
                best_ask.total_qty += qty;
                best_ask.venues.push_back({venue, qty});
            }
        }
    }

    if (best_bid.price && best_ask.price) {
        // Compare-then-subtract, never a raw subtraction of two unsigned
        // values that could go negative - same convention as types.h.

        result.crossed = best_bid.price >= best_ask.price;
    } else {
        result.crossed = false;
    }

    return result;
}

}  // namespace market_data
