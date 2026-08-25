#pragma once
#include <map>
#include <optional>
#include <utility>
#include <vector>
#include "types.h"

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

   private:
    template <typename Compare>
    static void ApplySide(OrderBookType<Compare>& side, const std::vector<PriceLevel>& levels);

    VenueId venue_;
    InstrumentId instrument_;
    uint64_t last_seq_ = 0;
    OrderBookType<std::greater<PriceTicks>> bids_;  // descending: begin() = best bid
    OrderBookType<std::less<PriceTicks>> asks_;     // ascending: begin() = best ask
};