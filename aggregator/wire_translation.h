#pragma once

#include "md_core/consolidated_bbo.h"
#include "md_core/consolidated_book.h"
#include "aggregator.pb.h"

namespace market_data {

// VenueId (global scope, types/venue.h) has no UNSPECIFIED value; wire::Venue
// does (proto3 enums must have a zero value). VENUE_UNSPECIFIED should never
// actually be produced here - every VenueId case is handled explicitly.
wire::Venue ToWire(VenueId venue);

wire::ConsolidatedPriceLevel ToWire(const consolidated::ConsolidatedPriceLevel& level);

wire::Bbo ToWire(const consolidated::BBO& bbo);

// The band results don't carry their own threshold - the caller holds the
// request's threshold list and zips it positionally with the results, which
// FillToNotionalBands/FillToBpsBands guarantee are in the same order as the
// targets passed in.
wire::VolumeBandResult ToWire(const consolidated::NotionalFill& fill, uint64_t notional_threshold);

wire::PriceBandResult ToWire(const consolidated::BpsFill& fill, uint32_t bps_threshold);

}  // namespace market_data
