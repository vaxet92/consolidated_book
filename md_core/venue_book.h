#pragma once
#include <array>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>
#include "types.h"

namespace market_data {

template <typename Compare>
using OrderBookType = std::map<PriceTicks, QtyUnits, Compare>;

class VenueBook {
   public:
    VenueBook(VenueId venue, InstrumentId instrument);

    void ApplyUpdate(const BookUpdate& update);

    std::optional<std::pair<PriceTicks, QtyUnits>> BestBid() const;
    std::optional<std::pair<PriceTicks, QtyUnits>> BestAsk() const;

    VenueId venue() const;
    InstrumentId instrument() const;
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
    InstrumentId instrument_;
    uint64_t last_seq_ = 0;
    int64_t last_update_mono_ns_ = 0;  // 0 = never received anything
    OrderBookType<std::greater<PriceTicks>> bids_;  // descending: begin() = best bid
    OrderBookType<std::less<PriceTicks>> asks_;     // ascending: begin() = best ask
};

// One VenueBook per venue, indexed by VenueId. A null entry means that
// venue isn't configured for this instrument.
using VenueBookArray = std::array<std::unique_ptr<VenueBook>, kVenueCount>;

class PrintHelper {
   public:
    static void Level(const char* side, const PriceLevel& level);
    static void BBO(const VenueBook& book);
    static void Book(const VenueBook& book);
};

}  // namespace market_data
