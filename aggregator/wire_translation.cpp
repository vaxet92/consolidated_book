#include "wire_translation.h"

namespace market_data {

wire::Venue ToWire(VenueId venue) {
    switch (venue) {
        case VenueId::BINANCE:
            return wire::BINANCE;
        case VenueId::BYBIT:
            return wire::BYBIT;
        case VenueId::OKX:
            return wire::OKX;
        case VenueId::COUNT:
            break;  // not a real venue - falls through to VENUE_UNSPECIFIED
    }
    return wire::VENUE_UNSPECIFIED;
}

wire::ConsolidatedPriceLevel ToWire(const consolidated::ConsolidatedPriceLevel& level) {
    wire::ConsolidatedPriceLevel wire_level;
    // PriceTicks/QtyUnits are unsigned; proto fields are int64. Safe: real
    // scaled prices/quantities never approach 2^63.
    wire_level.set_price(static_cast<int64_t>(level.price));
    wire_level.set_total_qty(static_cast<int64_t>(level.total_qty));
    for (const auto& venue_quote : level.venues) {
        wire::VenueQuote* wire_quote = wire_level.add_venues();
        wire_quote->set_venue(ToWire(venue_quote.venue));
        wire_quote->set_qty(static_cast<int64_t>(venue_quote.qty));
    }
    return wire_level;
}

wire::Bbo ToWire(const consolidated::BBO& bbo) {
    wire::Bbo wire_bbo;
    *wire_bbo.mutable_best_bid() = ToWire(bbo.best_bid);
    *wire_bbo.mutable_best_ask() = ToWire(bbo.best_ask);
    wire_bbo.set_crossed(bbo.crossed);
    return wire_bbo;
}

}  // namespace market_data
