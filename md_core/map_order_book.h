#pragma once
#include <array>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>
#include "types.h"
#include "types/venue_registry.h"

namespace market_data {

template <typename Compare>
using OrderBookType = std::map<PriceTicks, QtyUnits, Compare>;

class MapOrderBook {
   public:
    MapOrderBook(VenueId venue, InstrumentKey instrument);

    void ApplyUpdate(const BookUpdate& update);

    std::optional<std::pair<PriceTicks, QtyUnits>> BestBid() const;
    std::optional<std::pair<PriceTicks, QtyUnits>> BestAsk() const;

    VenueId venue() const;
    InstrumentKey instrument() const;
    uint64_t last_seq() const;

    // Arrival time of the last depth update, on OUR monotonic clock. The
    // staleness watchdog reads this and nothing else.
    //
    // KEY: 0 means NEVER HEARD FROM, which is not the same as stale. Every
    // venue is 0 at startup, and a venue that has never spoken needs a
    // different operator response than one that spoke and then stopped -
    // bad config or a rejected subscription, versus a dead feed. The
    // staleness predicate must keep the two apart.
    int64_t last_update_mono_ns() const;

    const auto& bids() const { return bids_; }
    const auto& asks() const { return asks_; }

   private:
    template <typename Compare>
    static void ApplySide(OrderBookType<Compare>& side, const std::vector<PriceLevel>& levels);

    VenueId venue_;
    InstrumentKey instrument_;
    uint64_t last_seq_ = 0;
    int64_t last_update_mono_ns_ = 0;               // 0 = never received anything
    OrderBookType<std::greater<PriceTicks>> bids_;  // descending: begin() = best bid
    OrderBookType<std::less<PriceTicks>> asks_;     // ascending: begin() = best ask
};

// One MapOrderBook per venue. A null entry means that venue isn't configured for
// this instrument.
//
// Sized by kMaxVenues (fixed CAPACITY) rather than kVenueCount (compile-time
// venue LIST) - DESIGN.md §17.6. Sizing by the enum is what makes adding a
// venue a recompile of md_core, and a recompile means a restart, which gaps
// every venue and every client at once.
//
// KEY: this is a capacity change only, and it is safe precisely because the
// array holds null-checkable handles. Slots beyond the configured venues are
// null, which is a state every reader already handles - an unconfigured venue
// has always been null here. Nothing about indexing or ownership changes.
//
// The INDEX is still VenueId today. Migrating it to VenueSlot, and migrating
// the loop bounds from kVenueCount to the registry's runtime size, are
// separate later steps - each one changes behaviour, where this does not.
//
// Cost: 8 slots instead of 3, so 5 extra null pointers (40 bytes) per
// instrument. At 10 instruments that is ~400 bytes in total, which is not
// worth avoiding. It does spread the live entries further apart, which is the
// kind of cache effect §17.9 warns about; not measurable at this size, but
// stated rather than skipped.
using MapOrderBookArray = std::array<std::unique_ptr<MapOrderBook>, kMaxVenues>;

class PrintHelper {
   public:
    static void Level(const char* side, const PriceLevel& level);
    static void BBO(const MapOrderBook& book);
    static void Book(const MapOrderBook& book);
};

}  // namespace market_data
