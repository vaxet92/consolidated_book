#pragma once

#include <cstdint>

namespace market_data {

// Staleness classification for one venue's feed (DESIGN_1 §6).
//
// Why this exists at all: the merge takes max(bid) and min(ask). A frozen
// venue never moves, so when the market falls the frozen venue ALWAYS looks
// like the best bid, and when it rises it ALWAYS looks like the best ask.
//
// KEY: staleness is not random error that averages out - the merge actively
// selects for it. One stale venue out of three corrupts the output nearly
// every time the market moves, not one third of the time. That is why a
// stale venue must be excluded from the merge rather than merely reported.
enum class VenueHealth : uint8_t {
    // Never sent a single message. Deliberately NOT the same as kStale: at
    // startup every venue is here, and the operator response differs - bad
    // config or a rejected subscription, versus a feed that worked and then
    // died. Collapsing the two hides a broken subscription behind a message
    // that says the exchange is down.
    kNoData,
    kLive,
    kStale,
};

// Pure function: reads no clock, holds no state. The caller supplies `now`,
// which is what keeps md_core free of I/O and makes every test three
// integers and an expected enum - no sleeping, no injected fake clock.
//
// Both timestamps must come from the SAME monotonic clock (steady_clock, via
// Provider::GetMonotonicNs). Then the clock's arbitrary epoch cancels in the
// subtraction. Passing a wall-clock stamp here is a bug: an NTP step
// backwards blinds the check, and a step forwards marks every venue stale at
// once and publishes an empty book that reads like a total exchange outage.
//
// PRECONDITION: stale_after_ns > 0. A zero or negative threshold marks every
// venue stale on the first call. That is rejected at config load, loudly and
// at startup, rather than guarded here on a path that runs inside the merge.
constexpr VenueHealth ClassifyVenue(int64_t last_mono_ns, int64_t now_mono_ns, int64_t stale_after_ns) {
    // Checked FIRST, before any arithmetic. A venue that never spoke has no
    // age to compute - without this, `now - 0` reports the process uptime as
    // if it were the feed's age, which is meaningless and always huge enough
    // to look stale.
    if (last_mono_ns == 0) {
        return VenueHealth::kNoData;
    }

    // Strictly `>`: a venue exactly at the threshold is still live. Arbitrary
    // either way, but it has to be decided once and tested rather than left
    // to whatever the code happens to do.
    //
    // A NEGATIVE age is handled for free and correctly. Two threads reading
    // steady_clock can produce a `now` marginally older than `last`; the
    // subtraction goes negative, the comparison is false, and the answer is
    // kLive - which is right, since a message from the immediate future is
    // not stale.
    if (now_mono_ns - last_mono_ns > stale_after_ns) {
        return VenueHealth::kStale;
    }

    return VenueHealth::kLive;
}

// Only a live venue may contribute to the consolidated output. kNoData is
// excluded too, though its book is empty anyway - the distinction is carried
// for reporting, not for the merge.
constexpr bool IsAdmissible(VenueHealth health) {
    return health == VenueHealth::kLive;
}

}  // namespace market_data
