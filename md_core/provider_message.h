#pragma once

#include <cstddef>
#include <variant>

#include "spsc_queue.h"
#include "types.h"
#include "venue_health.h"

namespace market_data {

// Everything one venue's provider thread hands to the consolidator, in the
// order it produced it.
//
// KEY: ONE queue per venue carrying all three kinds - not three queues split
// by kind. Depth and fast-BBO share a single io_context thread per provider
// (md_provider.h), so a provider emits updates, quotes and health events as
// one interleaved sequence. Splitting them by kind would let the consolidator
// drain a health event before an update that was produced earlier, and
// "Bybit went stale AFTER Bybit's update #47" would stop being a fact. The
// queue preserves that ordering rather than creating it - see the comment on
// Core::OnVenueHealth and the note above VenueHealthEvent in venue_health.h.
//
// Cross-venue order is deliberately NOT preserved: Binance's update and
// Bybit's health event have no defined relationship, need none, and §9
// forbids comparing sequence numbers across venues anyway.
using ProviderMessage = std::variant<BookUpdate, BboQuote, VenueHealthEvent>;

// Slots per venue queue. sizeof(ProviderMessage) is 112 bytes (measured), so
// this is 28 KB per venue and 224 KB across kMaxVenues.
//
// Sized for the expected BURST, not for the worst imaginable backlog. Steady
// state needs about one slot: the merge costs 8.25 us (measured,
// bench_md_core) against live depth arriving at roughly 9 messages/sec, so
// the consumer is orders of magnitude faster than the producer. The real
// burst is resync - a provider buffers depth updates while its REST snapshot
// is in flight, which at that arrival rate is on the order of 20 messages
// even for a slow two-second fetch. That last figure is derived from the
// measured rate, not itself measured.
//
// KEY: a bigger queue is not free insurance. A deep queue in market data
// means applying STALE updates - if the consolidator falls behind, a
// thousand buffered messages means publishing a book that is seconds old,
// silently. A small queue turns the same condition into visible backpressure
// on that venue's socket read, which is a failure you can see. Full() and
// Empty() exist so instrumentation can check whether the near-empty
// expectation actually holds in production.
inline constexpr std::size_t kProviderQueueCapacity = 256;

// One of these per venue slot. Single producer: that venue's provider
// thread. Single consumer: the consolidator thread.
using ProviderQueue = SpscQueue<ProviderMessage, kProviderQueueCapacity>;

}  // namespace market_data
