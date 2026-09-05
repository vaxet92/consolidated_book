#include "consolidated_book.h"

namespace market_data {
namespace consolidated {

void MergeBooks(const VenueBookArray& books, size_t venue_count, Book& out, size_t max_depth,
                const VenueHealthArray* health) {
    out.Clear();  // keeps capacity - no allocation after warm-up

    // Every loop below runs to `venue_count`, never to kVenueCount. Bounding
    // by the enum is what made a fourth venue register successfully and then
    // never appear in the output - no error, just a venue quietly missing
    // from the merge (DESIGN.md §17.6).
    //
    // The caller passes Core's high-water mark, so a slot whose venue was
    // removed is still iterated and skipped as a null book. That is correct:
    // slots are dense and a removed venue leaves a HOLE, so stopping early
    // would drop every venue above it.
    const size_t count = std::min(venue_count, books.size());

    // Admission is decided once, here, and then read by both sides. Deciding
    // it per side could let bids and asks disagree about which venues are in,
    // which would produce a merged book whose two halves come from different
    // sets of venues - a crossed or inverted book with no single cause to
    // trace it back to.
    //
    // nullptr admits everything: an absent policy is not the same as a policy
    // that excludes, and a pure merge with no verdict supplied must merge
    // what it was given.
    std::array<bool, kMaxVenues> admitted{};

    // No venue-id lookup here any more. An earlier version read
    // books[i]->venue() to attribute each level, which cost a MEASURED
    // ~3.5-4 us per merge when done per level, and one array read per level
    // after being hoisted here (becnhmark_results.md). Carrying a VenueSlot in
    // VenueQuote removes both: the merge index already IS the slot.
    for (size_t i = 0; i < count; ++i) {
        admitted[i] = (health == nullptr) || IsAdmissible((*health)[i]);
    }

    // --- bids: highest price first ---
    {
        using BidMap = OrderBookType<std::greater<PriceTicks>>;

        std::array<BidMap::const_iterator, kMaxVenues> it{};
        std::array<BidMap::const_iterator, kMaxVenues> end{};
        std::array<bool, kMaxVenues> active{};
        for (size_t i = 0; i < count; ++i) {
            // A venue that is not admitted is simply never made active, so
            // the k-way merge below never looks at it. No branch is added to
            // the inner loop - the exclusion costs nothing per level.
            if (books[i] && admitted[i]) {
                it[i] = books[i]->bids().begin();
                end[i] = books[i]->bids().end();
                active[i] = it[i] != end[i];
            }
        }

        QtyUnits cum_qty = 0;
        Notional cum_notional = 0;
        while (out.bids.size() < max_depth) {
            PriceTicks best = 0;
            bool found = false;
            for (size_t i = 0; i < count; ++i) {
                if (active[i] && (!found || it[i]->first > best)) {
                    best = it[i]->first;
                    found = true;
                }
            }
            if (!found) {
                break;  // every venue exhausted
            }

            out.bids.emplace_back();
            MergedLevel& level = out.bids.back();
            level.price = best;

            // This level's own quantity is a local: MergedLevel stores only
            // the running total, and LevelQty() recovers the per-level value.
            QtyUnits level_qty = 0;
            for (size_t i = 0; i < count; ++i) {
                if (active[i] && it[i]->first == best) {
                    const QtyUnits qty = it[i]->second;
                    level_qty += qty;
                    // Attribution IS the loop index: `i` is the slot, and
                    // VenueQuote carries a slot rather than a VenueId, so
                    // there is nothing to look up. The name is resolved once
                    // at the wire boundary (Core::VenueName), never per level.
                    //
                    // The bound is provable - the inner loop visits each of
                    // `count` slots at most once, and count <= kMaxVenues - so
                    // this guard should never fire. It is here because the
                    // proof depends on two constants agreeing, and they have
                    // already disagreed once: overrunning corrupts the next
                    // MergedLevel in the vector, which is silent, whereas
                    // losing one level's attribution is visible and bounded.
                    if (level.venue_count < level.venues.size()) {
                        level.venues[level.venue_count++] = {static_cast<VenueSlot>(i), qty};
                    }
                    ++it[i];
                    active[i] = it[i] != end[i];
                }
            }
            cum_qty += level_qty;
            cum_notional += static_cast<Notional>(best) * level_qty;
            level.cum_qty = cum_qty;
            level.cum_notional = cum_notional;
        }
    }

    // --- asks: lowest price first. Same shape as bids, opposite comparison.
    // The two sides are different types (different map comparators), so they
    // cannot share one function without a template. ---
    {
        using AskMap = OrderBookType<std::less<PriceTicks>>;
        std::array<AskMap::const_iterator, kMaxVenues> it{};
        std::array<AskMap::const_iterator, kMaxVenues> end{};
        std::array<bool, kMaxVenues> active{};
        for (size_t i = 0; i < count; ++i) {
            if (books[i] && admitted[i]) {
                it[i] = books[i]->asks().begin();
                end[i] = books[i]->asks().end();
                active[i] = it[i] != end[i];
            }
        }

        QtyUnits cum_qty = 0;
        Notional cum_notional = 0;
        while (out.asks.size() < max_depth) {
            PriceTicks best = 0;
            bool found = false;
            for (size_t i = 0; i < count; ++i) {
                if (active[i] && (!found || it[i]->first < best)) {  // asks: lower is better
                    best = it[i]->first;
                    found = true;
                }
            }
            if (!found) {
                break;  // every venue exhausted
            }

            out.asks.emplace_back();
            MergedLevel& level = out.asks.back();
            level.price = best;

            QtyUnits level_qty = 0;
            for (size_t i = 0; i < count; ++i) {
                if (active[i] && it[i]->first == best) {
                    const QtyUnits qty = it[i]->second;
                    level_qty += qty;
                    // Attribution IS the loop index: `i` is the slot, and
                    // VenueQuote carries a slot rather than a VenueId, so
                    // there is nothing to look up. The name is resolved once
                    // at the wire boundary (Core::VenueName), never per level.
                    //
                    // The bound is provable - the inner loop visits each of
                    // `count` slots at most once, and count <= kMaxVenues - so
                    // this guard should never fire. It is here because the
                    // proof depends on two constants agreeing, and they have
                    // already disagreed once: overrunning corrupts the next
                    // MergedLevel in the vector, which is silent, whereas
                    // losing one level's attribution is visible and bounded.
                    if (level.venue_count < level.venues.size()) {
                        level.venues[level.venue_count++] = {static_cast<VenueSlot>(i), qty};
                    }
                    ++it[i];
                    active[i] = it[i] != end[i];
                }
            }
            cum_qty += level_qty;
            cum_notional += static_cast<Notional>(best) * level_qty;
            level.cum_qty = cum_qty;
            level.cum_notional = cum_notional;
        }
    }
}

// Builds one band's result given the index of the level that crosses
// `target_raw` (or side.size() if the book is exhausted first). Shared by the
// single- and multi-band entry points so the partial-fill arithmetic exists
// in exactly one place.
static NotionalFill FillFromCrossing(const std::vector<MergedLevel>& side, Notional target_raw, size_t crossing) {
    NotionalFill result;
    if (side.empty()) {
        result.insufficient_depth = target_raw > 0;
        return result;
    }
    if (target_raw == 0) {
        return result;
    }

    if (crossing >= side.size()) {
        // Book exhausted before the target - a legitimate answer on BTCUSDT,
        // not an error (§5.2).
        const MergedLevel& last = side.back();
        result.filled_qty = last.cum_qty;
        result.filled_notional = static_cast<uint64_t>(last.cum_notional / kScaleFactor);
        result.worst_price = last.price;
        result.level_count = static_cast<uint32_t>(side.size());
        result.vwap = last.cum_qty ? static_cast<PriceTicks>(last.cum_notional / last.cum_qty) : 0;
        result.insufficient_depth = true;
        return result;
    }

    // This level straddles the target: take only part of it.
    const Notional filled_before = (crossing == 0) ? Notional{0} : side[crossing - 1].cum_notional;
    const QtyUnits qty_before = (crossing == 0) ? QtyUnits{0} : side[crossing - 1].cum_qty;
    const Notional still_needed = target_raw - filled_before;

    // needed / price: 1e16 / 1e8 = 1e8, already a QtyUnits. Integer division
    // truncates, so we under-fill by a sub-satoshi amount rather than over.
    QtyUnits partial_qty = static_cast<QtyUnits>(still_needed / side[crossing].price);
    const QtyUnits available = LevelQty(side, crossing);
    if (partial_qty > available) {
        partial_qty = available;  // guard against rounding overshoot
    }

    const Notional filled_raw = filled_before + static_cast<Notional>(side[crossing].price) * partial_qty;
    result.filled_qty = qty_before + partial_qty;
    result.filled_notional = static_cast<uint64_t>(filled_raw / kScaleFactor);
    result.worst_price = side[crossing].price;
    result.level_count = static_cast<uint32_t>(crossing + 1);
    result.vwap = result.filled_qty ? static_cast<PriceTicks>(filled_raw / result.filled_qty) : 0;
    return result;
}

NotionalFill FillToNotional(const std::vector<MergedLevel>& side, uint64_t target_notional) {
    // Target arrives as USDT x 1e8; cum_notional is raw price x qty (x 1e16).
    const Notional target_raw = static_cast<Notional>(target_notional) * kScaleFactor;

    // Prefix sums make this a search for the crossing level, not an
    // accumulation loop.
    size_t i = 0;
    while (i < side.size() && side[i].cum_notional < target_raw) {
        ++i;
    }
    return FillFromCrossing(side, target_raw, i);
}

void FillToNotionalBands(const std::vector<MergedLevel>& side, const std::vector<uint64_t>& targets,
                         std::vector<NotionalFill>& out) {
    out.clear();  // keeps capacity
    out.reserve(targets.size());

    // ONE walk for all targets. Because the targets are sorted ascending and
    // cum_notional is monotonic, the crossing index for target N+1 can never
    // be before the crossing index for target N - so `i` never rewinds and
    // the whole loop is O(levels + bands), not O(levels x bands).
    size_t i = 0;
    for (uint64_t target : targets) {
        const Notional target_raw = static_cast<Notional>(target) * kScaleFactor;
        while (i < side.size() && side[i].cum_notional < target_raw) {
            ++i;
        }
        out.push_back(FillFromCrossing(side, target_raw, i));
    }
}

BpsFill FillToBps(const std::vector<MergedLevel>& side, uint32_t bps, bool is_bid) {
    BpsFill result;
    if (side.empty()) {
        return result;
    }

    // A bid band wider than 100% would underflow (10000 - bps); clamp it to
    // a limit of zero, which simply includes the whole side.
    const uint32_t clamped_bps = (is_bid && bps > 10000) ? 10000 : bps;
    const PriceTicks top = side.front().price;
    const Notional scaled = static_cast<Notional>(top) * (is_bid ? (10000 - clamped_bps) : (10000 + clamped_bps));
    result.limit_price = static_cast<PriceTicks>(scaled / 10000);

    // Unlike FillToNotional there is no partial level - a level is either
    // inside the boundary or outside it.
    size_t i = 0;
    while (i < side.size()) {
        const bool inside = is_bid ? (side[i].price >= result.limit_price) : (side[i].price <= result.limit_price);
        if (!inside) {
            break;
        }
        ++i;
    }
    // Exiting because the book ended, rather than because a level fell
    // outside the boundary, means the totals are a lower bound.
    result.insufficient_depth = (i == side.size());
    if (i == 0) {
        return result;  // nothing within the band
    }

    const MergedLevel& last = side[i - 1];
    result.cum_qty = last.cum_qty;
    result.cum_notional = static_cast<uint64_t>(last.cum_notional / kScaleFactor);
    result.level_count = static_cast<uint32_t>(i);
    result.vwap = last.cum_qty ? static_cast<PriceTicks>(last.cum_notional / last.cum_qty) : 0;
    return result;
}

void FillToBpsBands(const std::vector<MergedLevel>& side, const std::vector<uint32_t>& bps_bands, bool is_bid,
                    std::vector<BpsFill>& out) {
    out.clear();  // keeps capacity
    out.reserve(bps_bands.size());

    if (side.empty()) {
        out.resize(bps_bands.size());  // all default-constructed: nothing in any band
        return;
    }

    const PriceTicks top = side.front().price;

    // ONE walk for all bands. Wider bands always include narrower ones, so
    // with bps_bands sorted ascending the boundary index only ever moves
    // forward - `i` never rewinds.
    size_t i = 0;
    for (uint32_t bps : bps_bands) {
        const uint32_t clamped_bps = (is_bid && bps > 10000) ? 10000 : bps;
        const Notional scaled = static_cast<Notional>(top) * (is_bid ? (10000 - clamped_bps) : (10000 + clamped_bps));
        const PriceTicks limit = static_cast<PriceTicks>(scaled / 10000);

        while (i < side.size()) {
            const bool inside = is_bid ? (side[i].price >= limit) : (side[i].price <= limit);
            if (!inside) {
                break;
            }
            ++i;
        }

        BpsFill result;
        result.limit_price = limit;
        result.insufficient_depth = (i == side.size());
        if (i > 0) {
            const MergedLevel& last = side[i - 1];
            result.cum_qty = last.cum_qty;
            result.cum_notional = static_cast<uint64_t>(last.cum_notional / kScaleFactor);
            result.level_count = static_cast<uint32_t>(i);
            result.vwap = last.cum_qty ? static_cast<PriceTicks>(last.cum_notional / last.cum_qty) : 0;
        }
        out.push_back(result);
    }
}

}  // namespace consolidated
}  // namespace market_data
