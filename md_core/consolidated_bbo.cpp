#include "consolidated_bbo.h"

#include <algorithm>

namespace market_data {
namespace consolidated {

BBO ComputeBBO(const VenueBookArray& books) {
    BBO result;
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

// Resets a level to a single venue WITHOUT reallocating: clear() keeps the
// vector's capacity, so after Core reserves once at init this never
// allocates again (DESIGN_1 §7.5 - no allocation on hot paths). Assigning a
// whole ConsolidatedPriceLevel instead would move-steal the temporary's
// 1-element buffer and drop the reserved one.
static void SetSingleVenue(ConsolidatedPriceLevel& level, PriceTicks price, VenueId venue, QtyUnits qty) {
    level.price = price;
    level.total_qty = qty;
    level.venues.clear();
    level.venues.push_back({venue, qty});
}

// One side's full scan, factored out so ComputeBBOFromQuotes and the rescan
// path share exactly one implementation - and so a rescan only recomputes
// the side that actually collapsed, not both. Fills `out` in place rather
// than returning by value, so the rescan path also reuses its buffer.
static void ScanBestBid(const VenueQuoteArray& quotes, ConsolidatedPriceLevel& out) {
    out.price = 0;
    out.total_qty = 0;
    out.venues.clear();
    for (size_t i = 0; i < quotes.size(); ++i) {
        const BboQuote& quote = quotes[i];
        if (quote.bid_price == 0) {
            continue;  // venue has sent no quote yet
        }
        VenueId venue = static_cast<VenueId>(i);
        if (out.price == 0 || quote.bid_price > out.price) {
            SetSingleVenue(out, quote.bid_price, venue, quote.bid_qty);
        } else if (quote.bid_price == out.price) {
            out.total_qty += quote.bid_qty;
            out.venues.push_back({venue, quote.bid_qty});
        }
    }
}

static void ScanBestAsk(const VenueQuoteArray& quotes, ConsolidatedPriceLevel& out) {
    out.price = 0;
    out.total_qty = 0;
    out.venues.clear();
    for (size_t i = 0; i < quotes.size(); ++i) {
        const BboQuote& quote = quotes[i];
        if (quote.ask_price == 0) {
            continue;
        }
        VenueId venue = static_cast<VenueId>(i);
        if (out.price == 0 || quote.ask_price < out.price) {  // asks: lower is better
            SetSingleVenue(out, quote.ask_price, venue, quote.ask_qty);
        } else if (quote.ask_price == out.price) {
            out.total_qty += quote.ask_qty;
            out.venues.push_back({venue, quote.ask_qty});
        }
    }
}

// New price == best
// - venue already in array -> update qty, total = total - old + new
// - venue not in array     -> append, total += qty
//
// Takes venue+qty explicitly rather than a quote: a BboQuote has separate
// bid_qty and ask_qty, and this is called for both sides.
static void UpdateEqualPrice(ConsolidatedPriceLevel& bbo_level, VenueId venue, QtyUnits qty) {
    auto exist_it = std::find_if(bbo_level.venues.begin(), bbo_level.venues.end(),
                                 [venue](const VenueQuote& vq) { return vq.venue == venue; });
    if (exist_it != bbo_level.venues.end()) {
        bbo_level.total_qty -= exist_it->qty;
        exist_it->qty = qty;
        bbo_level.total_qty += qty;
    } else {
        bbo_level.total_qty += qty;
        bbo_level.venues.push_back({venue, qty});
    }
}

// New price worse
//  venue not in array -> ignore
//  venue in array     -> erase, total -= its OLD qty (not the incoming one)
//
// Returns true if the venue was present, so the caller can detect the level
// losing its last venue and trigger a rescan.
static bool UpdateWorstPrice(ConsolidatedPriceLevel& bbo_level, VenueId venue) {
    auto exist_it = std::find_if(bbo_level.venues.begin(), bbo_level.venues.end(),
                                 [venue](const VenueQuote& vq) { return vq.venue == venue; });
    if (exist_it == bbo_level.venues.end()) {
        return false;
    }
    bbo_level.total_qty -= exist_it->qty;
    bbo_level.venues.erase(exist_it);
    return true;
}

static bool IsCrossed(const ConsolidatedPriceLevel& bid, const ConsolidatedPriceLevel& ask) {
    return (bid.price && ask.price) && bid.price >= ask.price;
}

BBO ComputeBBOFromQuotes(const VenueQuoteArray& quotes) {
    // Returns by value: this is the oracle and the initial build, not a hot
    // path, so the fresh vectors here are fine. The hot path is
    // UpdateBBOWithQuote, which mutates in place.
    BBO result;
    ScanBestBid(quotes, result.best_bid);
    ScanBestAsk(quotes, result.best_ask);
    result.crossed = IsCrossed(result.best_bid, result.best_ask);
    return result;
}

void UpdateBBOWithQuote(BBO& current, const BboQuote& update, const VenueQuoteArray& quotes) {
    // ---- bid side: better means HIGHER ----
    ConsolidatedPriceLevel& best_bid = current.best_bid;
    if (update.bid_price == 0) {
        // Venue has no bid at all. If it was holding the best level, it left.
        if (UpdateWorstPrice(best_bid, update.venue) && best_bid.venues.empty()) {
            ScanBestBid(quotes, best_bid);
        }
    } else if (best_bid.price == 0 || update.bid_price > best_bid.price) {
        // Strictly better (or first data): this venue alone owns the level.
        SetSingleVenue(best_bid, update.bid_price, update.venue, update.bid_qty);
    } else if (update.bid_price == best_bid.price) {
        UpdateEqualPrice(best_bid, update.venue, update.bid_qty);
    } else {
        // Worse than the best. Only matters if this venue WAS holding the
        // best level and has now moved off it.
        if (UpdateWorstPrice(best_bid, update.venue) && best_bid.venues.empty()) {
            // The best level lost its last venue. The new best is not
            // recoverable from `current` alone - it only ever kept the top
            // level - so rescan the per-venue array.
            ScanBestBid(quotes, best_bid);
        }
    }

    // ---- ask side: better means LOWER ----
    ConsolidatedPriceLevel& best_ask = current.best_ask;
    if (update.ask_price == 0) {
        if (UpdateWorstPrice(best_ask, update.venue) && best_ask.venues.empty()) {
            ScanBestAsk(quotes, best_ask);
        }
    } else if (best_ask.price == 0 || update.ask_price < best_ask.price) {
        SetSingleVenue(best_ask, update.ask_price, update.venue, update.ask_qty);
    } else if (update.ask_price == best_ask.price) {
        UpdateEqualPrice(best_ask, update.venue, update.ask_qty);
    } else {
        if (UpdateWorstPrice(best_ask, update.venue) && best_ask.venues.empty()) {
            ScanBestAsk(quotes, best_ask);
        }
    }

    current.crossed = IsCrossed(best_bid, best_ask);
}

}  // namespace consolidated
}  // namespace market_data