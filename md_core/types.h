#pragma once

#include <cstdint>
#include <vector>

#include "types/venue.h"

struct MDCoreConfig {};

using PriceTicks = uint64_t;         // price x 1e8, integral on the canonical grid. Never negative.
using QtyUnits = uint64_t;           // base quantity x 1e8. Never negative.
using Notional = unsigned __int128;  // ticks * units - needs the extra width. Never negative.

// Any place that needs a *signed* difference between two PriceTicks/QtyUnits
// (e.g. a crossed book: bid - ask) must compute it explicitly as
// (bool sign, PriceTicks magnitude) - compare first, then subtract the
// smaller from the larger. Never subtract two of these directly; the result
// can silently wrap instead of going negative.

struct PriceLevel {
    PriceTicks price;
    QtyUnits qty;  // absolute qty AT this price; qty == 0 means "remove this level"
};

struct BookUpdate {
    VenueId venue;
    InstrumentId instrument;
    uint64_t seq;        // venue-native monotonic sequence number
    int64_t recv_ts_ns;  // CLOCK_MONOTONIC, ours. Signed: used in drift subtraction.
    int64_t exch_ts_ns;  // venue's own timestamp - drift estimation only, never compared across venues.
    bool is_snapshot;    // true = full replace, false = incremental delta
    std::vector<PriceLevel> bids;
    std::vector<PriceLevel> asks;
};
