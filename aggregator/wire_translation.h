#pragma once

#include "md_core/consolidated_bbo.h"
#include "aggregator.pb.h"

namespace market_data {

// VenueId (global scope, types/venue.h) has no UNSPECIFIED value; wire::Venue
// does (proto3 enums must have a zero value). VENUE_UNSPECIFIED should never
// actually be produced here - every VenueId case is handled explicitly.
wire::Venue ToWire(VenueId venue);

wire::ConsolidatedPriceLevel ToWire(const consolidated::ConsolidatedPriceLevel& level);

wire::Bbo ToWire(const consolidated::BBO& bbo);

}  // namespace market_data
