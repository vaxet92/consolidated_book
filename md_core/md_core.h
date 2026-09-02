
#pragma once
#include "types/venue.h"
#include "venue_book.h"
#include "consolidated_bbo.h"
#include "consolidated_book.h"
#include "types.h"
#include "venue_health.h"
#include "logger/logger.h"
#include <functional>
#include <memory>
#include <mutex>

namespace market_data {

using InstrumentBooks = std::unordered_map<InstrumentId, VenueBookArray>;
using InstrumentQuotes = std::unordered_map<InstrumentId, VenueQuoteArray>;
using InstrumentBbo = std::unordered_map<InstrumentId, consolidated::BBO>;

class Core {
   public:
    using BboCallback = std::function<void(InstrumentId, const consolidated::BBO&)>;

    // Fired after every depth update with a fresh, immutable merged book.
    // shared_ptr, not a reference: fan-out to N subscribers is N refcount
    // bumps, and the pointer stays valid for whatever the callback does with
    // it after Core moves on to the next update. Core knows nothing about
    // bands or clients - it only produces the merged book; deciding which
    // bands to compute from it, for whom, is the subscriber's job (the
    // aggregator service), which is what makes per-client custom thresholds
    // (§8.4) possible without Core knowing about clients at all.
    using BookCallback = std::function<void(InstrumentId, std::shared_ptr<const consolidated::Book>)>;

    explicit Core(BboCallback bbo_callback, BookCallback book_callback);

    ~Core() = default;

    void Init(const CoreConfig& config);

    // Fed by whoever owns the Provider(s) (the wiring layer, e.g. main.cpp).
    // Core has no knowledge of providers, sockets, or threads.
    //
    // Depth path: applies the update to the per-venue book, then EAGERLY
    // rebuilds the full k-way merge across all configured venues (DESIGN_1
    // §5.2) and fires book_callback_ with the result - on every single
    // update, immediately, no throttle timer (same eager-publish decision
    // already made for BBO). This is provisional pending a real benchmark
    // (§14 step 8): if depth update rate ever outpaces the merge cost, the
    // fallback is throttling the merge to a fixed cadence instead of firing
    // on every update. Does NOT publish the BBO - the fast-BBO stream does
    // that (see ApplyQuote), a separate trigger at a separate rate; the two
    // are never combined into one callback (§7 - never mix the streams).
    void ApplyUpdate(const BookUpdate& update);

    // Fast-BBO path (DESIGN_1 §4.4 option 1) - this is what drives the
    // published BBO. Never touches VenueBook: the two streams are not
    // mutually sequenced, so mixing them corrupts the book (§7).
    void ApplyQuote(const BboQuote& quote);

    // A venue's staleness verdict changed (DESIGN_1 §6.5). Pushed by the
    // provider that owns that venue, on that provider's thread, in the same
    // call sequence as its ApplyUpdate/ApplyQuote calls - so the event is
    // ordered against that venue's own data, which is what makes acting on
    // it safe. When the SPSC queues land this becomes a queue message and the
    // ordering is preserved rather than created.
    //
    // Edge-triggered: only changes arrive, so this is called a handful of
    // times in a healthy run, not per tick.
    //
    // KEY: Core may only make a pushed verdict WORSE, never better. The
    // provider can see things Core cannot - its own sockets - so a venue it
    // reports as kDisconnected is disconnected, full stop. Cross-venue
    // corroboration (§6.2b signal 3, not built) will layer on top of this by
    // demoting a kLive venue that is silent while its peers are busy; it will
    // never promote one.
    void OnVenueHealth(const VenueHealthEvent& event);

    void Start() {}
    void Stop() {}

   private:
    // Creates the 3-slot VenueBookArray for `instrument`, one VenueBook per
    // venue in `venues`. Called from init() for each startup instrument; not
    // exposed as a live "subscribe" API yet (out of scope for now, see
    // DESIGN_1 §1.2 - multi-symbol is designed for, not exercised).
    void AddInstrument(InstrumentId instrument, const std::vector<VenueId>& venues);
    void RemoveInstrument(InstrumentId instrument);

    // Interim fix, not the final architecture: guards venue_books_ and
    // venue_quotes_ against concurrent ApplyUpdate()/ApplyQuote() calls from
    // multiple Provider threads. The designed fix (DESIGN_1 §7.3) is
    // per-venue SPSC queues drained by one consolidator thread, with no lock
    // on the book path at all - not built yet. This mutex is correct but
    // costs contention the real design wouldn't have; remove it once the
    // SPSC queue replaces this.
    std::mutex apply_mutex_;

    // Latest verdict per venue, one array per stream because depth and
    // fast-BBO are separate sockets and fail independently (§6.2d).
    //
    // KEY: initialized to kNoData, not kLive - fail-safe. Nothing is admitted
    // to the merge until a provider has affirmatively said its feed is alive.
    // The cost of being wrong in this direction is one publish with a thinner
    // book; the cost of the other direction is publishing prices from a venue
    // we have never heard from. The provider promotes a stream out of kNoData
    // on its very first message, so this costs no startup delay.
    //
    // Guarded by apply_mutex_ like the books: OnVenueHealth arrives on a
    // provider thread and ApplyUpdate reads these on another.
    VenueHealthArray depth_health_{};
    VenueHealthArray bbo_health_{};

    // Bumped whenever a BBO-stream verdict changes. Compared against the
    // per-instrument value below to decide whether the next quote can be
    // folded in incrementally or needs a full rescan.
    //
    // KEY: the merged Book needs no equivalent, because MergeBooks rebuilds
    // from scratch every pass - change the admission rule and the next output
    // is already correct. The BBO is different: UpdateBBOWithQuote maintains
    // PERSISTENT state, so a stale venue's price is already inside
    // consolidated_bbo_, and the venue sends nothing more to displace it.
    // Skipping its future quotes cannot remove a price that is already there.
    //
    // A version counter rather than a flag because health is per VENUE while
    // the BBO is per INSTRUMENT: one venue going stale invalidates every
    // instrument's BBO, and a counter says so without iterating them.
    uint64_t bbo_health_version_ = 0;
    std::unordered_map<InstrumentId, uint64_t> bbo_health_version_seen_;

    BboCallback bbo_callback_;
    BookCallback book_callback_;
    InstrumentBooks venue_books_;
    InstrumentQuotes venue_quotes_;

    // The running consolidated BBO per instrument. This is persistent state,
    // maintained incrementally by UpdateBBOWithQuote rather than recomputed
    // from scratch - which is exactly why the oracle test matters: a bug in
    // the incremental update doesn't fail loudly, it accumulates here
    // silently across thousands of updates.
    InstrumentBbo consolidated_bbo_;

    // Free-list of merged-book buffers per instrument, so the eager
    // per-update merge does not allocate after warm-up. A buffer is reused
    // only when use_count() == 1 - no subscriber still holds a reference to
    // it - which shared_ptr's own atomic refcount already tracks, so there
    // is no seqlock or manual synchronization here (per §7: no lock-free
    // structures without a measured reason). "Assume no slow subscriber" -
    // if that assumption breaks, the pool simply grows by one buffer rather
    // than corrupting anything.
    std::unordered_map<InstrumentId, std::vector<std::shared_ptr<consolidated::Book>>> book_pools_;

    // Returns a Book buffer for `instrument` that no subscriber currently
    // holds, reusing one from the pool if possible. Not thread-safe on its
    // own - called only from within apply_mutex_.
    std::shared_ptr<consolidated::Book> AcquireBookBuffer(InstrumentId instrument);
};

}  // namespace market_data
