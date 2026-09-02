#pragma once

#include <array>
#include <cstdint>

#include "types/venue.h"

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

    // Every socket for this venue's stream is down. Kept apart from kStale
    // because it is the one verdict made with CERTAINTY: a closed socket is
    // direct evidence, whereas silence is ambiguous - a dead feed and a quiet
    // market look identical to a timer.
    //
    // KEY: the reverse does not hold. A connection that is UP does not mean
    // the data is flowing. TCP stays ESTABLISHED while an exchange's
    // publisher thread wedges or a middlebox holds the socket open, so
    // connection state can only ever CONDEMN a venue, never clear one. That
    // asymmetry is why kStale still exists alongside this.
    kDisconnected,

    // We tore this venue down on purpose and are rebuilding it: a sequence
    // gap was detected, so the book is WRONG rather than merely old, and the
    // provider has dropped its sockets to re-seed from a snapshot.
    //
    // KEY: this is the "no outstanding gap" half of the admission rule. Not
    // covered by kDisconnected, because a deliberate teardown deliberately
    // suppresses the socket-closed notification - otherwise every resync would
    // look like a connection failure and ask to be reconnected twice. So the
    // one moment we are certain the book is invalid is exactly the moment
    // connection state says nothing. Without this state, a venue's known-broken
    // book keeps contributing to the merge for the whole resync window.
    //
    // Sticky: the watchdog does not overwrite it, because a timer has nothing
    // useful to say about a stream we switched off ourselves. It is cleared by
    // the first message that arrives after the venue comes back.
    kResyncing,

    // Silent for longer than the venue's own guarantees allow. What counts as
    // too long differs per venue and is derived, not invented: Bybit
    // republishes L1 with the same `u` after 3s of no change, and OKX sends
    // seqId == prevSeqId after ~60s, so on those streams silence past the
    // documented interval is evidence rather than a guess. Binance publishes
    // no such keepalive, so its silence is disambiguated by comparing against
    // the other venues - a quiet market is market-wide, a dead feed is not.
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

// For logs and, later, the wire's venue_status block. Returns a literal, not
// a std::string: this is called from log lines on the provider thread and has
// no business allocating.
constexpr const char* ToString(VenueHealth health) {
    switch (health) {
        case VenueHealth::kNoData:
            return "NO_DATA";
        case VenueHealth::kLive:
            return "LIVE";
        case VenueHealth::kDisconnected:
            return "DISCONNECTED";
        case VenueHealth::kResyncing:
            return "RESYNCING";
        case VenueHealth::kStale:
            return "STALE";
    }
    return "UNKNOWN";
}

// One health verdict per venue, indexed by VenueId - the same shape as
// VenueBookArray and VenueQuoteArray, so the three are indexed identically.
//
// This is the OUTPUT of the policy, not the policy itself. Core classifies
// each venue and hands the result to the merge; the merge does no
// classification of its own and reads no clock. Keeping admission out of the
// merge is what lets the merge stay a pure function of its inputs.
using VenueHealthArray = std::array<VenueHealth, kVenueCount>;

// Which of a venue's two feeds an event refers to.
//
// KEY: depth and fast-BBO are SEPARATE SOCKETS, so a venue can be dead on one
// and healthy on the other. They therefore carry independent stamps and
// independent verdicts, and this is what keeps them apart. Sharing one verdict
// would hide the exact failure the policy exists to catch (DESIGN_1 §6.2d).
enum class StreamKind : uint8_t {
    kDepth,
    kBbo,
};

// One verdict, decided by the provider and delivered to Core (DESIGN_1 §6.5).
//
// A plain POD on purpose: trivially copyable, fixed size, no heap. Today it
// arrives as a callback; when the per-venue SPSC queues land (§7.2) the same
// struct drops into a ring slot with no changes, travelling in-band with that
// venue's book updates.
//
// KEY: `decided_mono_ns` exists BECAUSE of that queue. The event is consumed
// later than it was produced, so it must carry the moment the provider
// decided rather than letting Core assume the verdict is current. Health that
// arrives through a side channel while the data arrives through a queue gives
// an inconsistent view of one venue - Core would exclude a venue whose
// updates it is still applying.
struct VenueHealthEvent {
    VenueId venue;
    StreamKind stream;
    VenueHealth health;
    int64_t decided_mono_ns;
};

// The full verdict for one feed: connection state first, then the timer.
//
// KEY: the two signals are not symmetric. A closed socket is direct evidence
// and settles the question. A socket that is OPEN settles nothing - TCP stays
// ESTABLISHED while an exchange's publisher thread wedges or a middlebox
// holds the connection open. Connection state can CONDEMN a venue but never
// clear one, which is why it cannot simply replace the timer.
//
// `backstop_ns` is per venue and per stream, and is derived rather than
// invented where the venue documents its own behaviour: Bybit republishes L1
// with the same `u` after 3s of no change, OKX sends seqId == prevSeqId after
// ~60s. Binance publishes no keepalive at all, so its silence carries no
// information and its health has to come from connection state plus
// cross-venue comparison (§6.2b signal 3, not built yet).
//
// Deliberately returns no kDisconnected-with-no-data distinction: an
// unreachable venue reads as kDisconnected regardless of whether it ever
// spoke. In practice startup does not hit that, because CreateDepthSession
// increments the live count at creation, before the connect completes.
constexpr VenueHealth ClassifyFeed(bool connected, int64_t last_message_mono_ns, int64_t now_mono_ns,
                                   int64_t backstop_ns) {
    if (!connected) {
        return VenueHealth::kDisconnected;
    }
    return ClassifyVenue(last_message_mono_ns, now_mono_ns, backstop_ns);
}

}  // namespace market_data
