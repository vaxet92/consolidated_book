
#pragma once
#include "types/venue.h"
#include "types/venue_registry.h"
#include "provider_message.h"
#include "venue_book.h"
#include "consolidated_bbo.h"
#include "consolidated_book.h"
#include "types.h"
#include "venue_health.h"
#include "logger/logger.h"
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>

namespace market_data {

// Keyed by InstrumentKey, so spot and futures for the same symbol are
// different entries and their books can never be merged together (§1.3).
// InstrumentKeyHash is the identity on the packed uint32_t.
using InstrumentBooks = std::unordered_map<InstrumentKey, VenueBookArray, InstrumentKeyHash>;
using InstrumentQuotes = std::unordered_map<InstrumentKey, VenueQuoteArray, InstrumentKeyHash>;
using InstrumentBbo = std::unordered_map<InstrumentKey, consolidated::BBO, InstrumentKeyHash>;

class Core {
   public:
    using BboCallback = std::function<void(InstrumentKey, const consolidated::BBO&)>;

    // Fired after every depth update with a fresh, immutable merged book.
    // shared_ptr, not a reference: fan-out to N subscribers is N refcount
    // bumps, and the pointer stays valid for whatever the callback does with
    // it after Core moves on to the next update. Core knows nothing about
    // bands or clients - it only produces the merged book; deciding which
    // bands to compute from it, for whom, is the subscriber's job (the
    // aggregator service), which is what makes per-client custom thresholds
    // (§8.4) possible without Core knowing about clients at all.
    using BookCallback = std::function<void(InstrumentKey, std::shared_ptr<const consolidated::Book>)>;

    explicit Core(BboCallback bbo_callback, BookCallback book_callback);

    // Joins the consolidator thread. NOT defaulted: a thread still running
    // when Core is destroyed would touch freed members, and std::thread's own
    // destructor calls std::terminate on a joinable thread.
    ~Core();

    void Init(const CoreConfig& config);

    // --- venue lifecycle (DESIGN.md §17.4) ---------------------------------
    //
    // KEY: venues appear because a provider APPEARS, never because config
    // named one. Init allocates capacity and registers nothing. Registering
    // from config would create slots for venues that may never connect, and
    // registering lazily on the first update would be creating state from a
    // data message - which §17.6 forbids, because an update arriving just
    // after a venue was removed would silently resurrect it.
    //
    // Today main.cpp calls these as it constructs and destroys providers. That
    // is the in-process stand-in for the kHello handshake and the socket close
    // (§17.4: accept is registration). When the process split lands the CALLER
    // changes and nothing inside Core does - which is why the registry is
    // keyed on the venue name rather than on VenueId.

    // Assigns `name` a slot, and creates that venue's VenueBook for every
    // instrument that already exists. Idempotent: a provider that crashed and
    // reconnected gets the SAME slot back, so it resumes the books it had
    // rather than stranding them under a slot nobody feeds.
    //
    // Returns nullopt only when the registry is full (more than kMaxVenues
    // distinct venues). That is a configuration error, not a runtime
    // condition; the caller must log it and refuse, because a venue silently
    // missing from the merge is exactly the failure this design exists to
    // avoid.
    std::optional<VenueSlot> RegisterVenue(std::string_view name);

    // Takes a venue out of service: frees its VenueBook for every instrument
    // and resets both stream verdicts to kNoData, so the next merge simply
    // does not see it. Instruments fed by other venues keep publishing, one
    // venue thinner - which is already the correct behaviour and already what
    // the health path does.
    //
    // KEY: this DEACTIVATES the slot; it does not release it. The name -> slot
    // mapping and venue_count() are untouched. Releasing the slot would give a
    // reconnecting provider a different one, and slot ids are held elsewhere -
    // in-flight updates, published attribution - so the venue's data would be
    // attributed to whatever else later occupied that slot. Retaining it is
    // what makes RegisterVenue's idempotence meaningful.
    //
    // Takes a VenueSlot rather than a name: the caller holds the token
    // RegisterVenue returned, so a venue that was never registered cannot be
    // removed by passing a string that happens to look right.
    void RemoveVenue(VenueSlot slot);

    // --- direct path: NOT THREAD-SAFE ---------------------------------------
    //
    // KEY: these apply immediately, on the calling thread, with NO lock. They
    // are safe only when one thread calls them and the consolidator is not
    // running. Production does not use them - main.cpp calls Enqueue* instead
    // (§7.2: no lock anywhere on the book path). They remain because the tests
    // use them as the ORACLE the queued path is compared against, which is what
    // makes "the two paths cannot diverge" a checked claim.
    //
    // Calling these while Start() is active is a data race.
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

    // --- queued path (DESIGN.md §7.2, §14.2 step 12) -----------------------
    //
    // The producer side. Called on a provider's own thread; takes no lock,
    // resolves nothing, and returns as soon as the message is in that
    // venue's ring.
    //
    // KEY: these take a VenueSlot, not a VenueId. The caller resolved it once
    // when the venue registered and holds it for the life of the connection
    // (§17.4 - accept is registration), so the hot path does no translation
    // and cannot race RegisterVenue/RemoveVenue writing venue_id_to_slot_.
    // That race is exactly what removing the mutex would otherwise expose.
    //
    // KEY: these NEVER block indefinitely. A market-data producer cannot
    // apply backpressure to its source - blocking a provider's io_context
    // thread stops it draining its sockets, the kernel receive buffer fills,
    // and the exchange either disconnects us or we read nothing but stale
    // messages. Blocking converts a bounded queue problem into an unbounded
    // staleness problem, and hides it. Each call makes a bounded attempt
    // (kEnqueueSpinAttempts) and then applies the policy below.
    //
    // KEY: the overflow policy differs by message type, because what is safe
    // to lose differs by message type:
    //
    //   BookUpdate       - returns false. A diff is one link in a sequenced
    //                      chain; losing one makes the book untrustworthy
    //                      from that point on. The CALLER must resync. Core
    //                      does not resync on its own: continuity and
    //                      resynchronization belong to the provider (§4.2,
    //                      §9), which already owns RequestResync().
    //
    //   BboQuote         - dropped, and counted. A quote is a COMPLETE
    //                      top-of-book snapshot, not a delta, so a newer one
    //                      supersedes it entirely. This is the same
    //                      conflation ConflatedChannel does for clients,
    //                      applied at ingest. Returns void: there is nothing
    //                      the caller should do about it.
    //
    //   VenueHealthEvent - returns false. A state TRANSITION, not a sample:
    //                      a lost kStale means Core keeps merging a dead
    //                      venue forever. Escalates the same way as a lost
    //                      diff, and resync re-announces health anyway.
    //
    // Nothing is ever dropped silently - a drop is either counted (quotes)
    // or reported to the caller (everything else).
    //
    // PRECONDITION: something must be draining, or every queue fills and
    // every venue ends up resyncing. Until Start() spawns the consolidator
    // thread that means the caller must call DrainOnce().
    [[nodiscard]] bool EnqueueUpdate(VenueSlot slot, BookUpdate update);
    void EnqueueQuote(VenueSlot slot, BboQuote quote);
    [[nodiscard]] bool EnqueueHealth(VenueSlot slot, VenueHealthEvent event);

    // Overflow observability. Both are per venue slot and monotonic.
    //
    // overflow_count is the number of updates/health events that could not be
    // enqueued - each one should have produced a resync. quote_drop_count is
    // the number of quotes deliberately conflated away. A healthy run leaves
    // both at zero, which is what makes kProviderQueueCapacity checkable
    // rather than assumed.
    uint64_t OverflowCount(VenueSlot slot) const;
    uint64_t QuoteDropCount(VenueSlot slot) const;

    // The consumer side. Drains every venue queue until all are empty,
    // applying each message in the order its venue produced it. Returns how
    // many were processed.
    //
    // Runs the same ProcessUpdate/ProcessQuote/ProcessHealth the synchronous
    // path uses, so the two cannot diverge. Takes no lock: the contract is
    // that exactly one thread calls this.
    //
    // Public because tests drive it directly - a synchronous drain is
    // deterministic in a way a background thread is not, so the book tests
    // stay free of sleeps and retries.
    size_t DrainOnce();

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

    // Per-update breakdown of where ApplyUpdate's time goes. Diagnostic only.
    //
    // Exists because the live publish latency measured ~140-190 us while
    // bench_md_core predicts ~8-12 us for this work, and three hypotheses for
    // the gap have already been killed by measurement. One number cannot be
    // diagnosed; four can.
    struct ApplyTimings {
        // Time the message waited before being processed. Under the old
        // mutex path this was lock contention; on the queued path it is 0
        // (there is no lock) until the message carries an enqueue stamp.
        int64_t lock_wait_ns = 0;
        int64_t book_apply_ns = 0;  // VenueBook::ApplyUpdate - the delta
        int64_t merge_ns = 0;       // MergeBooks - the k-way merge
        uint32_t merged_depth = 0;  // output levels, bid side
        uint32_t delta_levels = 0;  // levels in the incoming update, both sides
    };

    // KEY: Core still reads no clock. It calls a function it was GIVEN, so the
    // no-I/O rule in §2 survives literally rather than by exception - the same
    // shape as every other seam here (CallBack, BboCallback, BookCallback).
    //
    // Null clock disables instrumentation entirely: the cost when unset is one
    // null check per update, and no timing calls at all.
    using ClockFn = std::function<int64_t()>;
    using TimingsCallback = std::function<void(const ApplyTimings&)>;
    void SetInstrumentation(ClockFn clock, TimingsCallback sink) {
        clock_ = std::move(clock);
        timings_callback_ = std::move(sink);
    }

    // Starts the consolidator thread: the single thread that drains every
    // venue queue and owns every VenueBook from here on (DESIGN.md §7.2/§7.3).
    //
    // KEY: after Start(), the book logic is single-threaded. That is the whole
    // point - not speed, but that a single-threaded book is deterministic,
    // testable with fake input, and TSan-clean by construction rather than by
    // careful locking.
    //
    // PRECONDITION: every venue must be registered BEFORE Start(). Registration
    // mutates state the consolidator reads (active_venues_, venue_id_to_slot_,
    // the per-instrument book arrays) with no lock, which is safe only while no
    // consolidator thread is running. main.cpp already registers up front; when
    // venues become dynamic (§17.4) this needs a real answer, not a comment.
    //
    // Idempotent: calling Start() twice is a no-op, not a second thread.
    void Start();

    // Stops the consolidator thread and joins it, draining whatever is still
    // queued first so a clean shutdown does not silently discard messages the
    // providers already handed over.
    //
    // Safe to call when not started, and called by the destructor - a running
    // thread must never outlive the Core whose members it touches.
    void Stop();

    // How many venue slots are in use. Per-venue loops run to this instead of
    // to kVenueCount, which is what stops "add a venue" from being a recompile
    // (DESIGN.md §17.6).
    //
    // KEY: this is a HIGH-WATER MARK and is never decremented. Slots are dense
    // and assigned in registration order, so removing a venue leaves a HOLE,
    // not a shorter list - decrementing after removing slot 1 of 3 would make
    // the loops stop at 1 and silently drop the venue in slot 2 from the
    // merge, while the published book still looked well-formed.
    //
    // Removal is not a size change. It is already represented by
    // venue_books_[i] == nullptr and health kNoData, and both merge loops
    // already skip that (consolidated_book.cpp). A dead slot costs one test
    // per output level and corrupts nothing.
    //
    // The two repairs that look obvious are worse. Compacting the hole moves a
    // venue to a different slot, and slot ids are held elsewhere - in-flight
    // updates, published attribution - so prices would be attributed to the
    // wrong exchange. A free list reuses the slot, which lets a late update
    // from the old venue land on the new one: the remove/re-add race §17.11
    // avoids by tying lifetime to the connection.
    //
    // It barely grows: Register is idempotent by name, so a reconnecting
    // provider reclaims its old slot. The mark equals distinct venue names
    // ever seen, and is bounded by kMaxVenues.
    size_t venue_count() const { return venue_registry_.size(); }

    // Name of the venue occupying `slot`, or empty if nothing ever registered
    // there. The ONLY way out of Core for a venue's identity.
    //
    // KEY: attribution inside Core carries a slot, never a name (§17.6), so
    // this is called at the WIRE boundary - once per published message to
    // build a slot -> name table - and never per level. A merged book has up
    // to 1000 levels with up to kMaxVenues contributors each; resolving names
    // there would be thousands of lookups to produce at most 8 distinct
    // answers.
    std::string_view VenueName(VenueSlot slot) const { return venue_registry_.Name(slot); }

   private:
    // Creates the VenueBookArray for `instrument`, with a VenueBook for each
    // venue that is currently ACTIVE and nulls everywhere else.
    //
    // No venue list parameter: venues come from RegisterVenue, not from
    // config. The two directions have to stay in sync - a new instrument gets
    // books for the venues already registered, and a newly registered venue
    // gets books for the instruments that already exist.
    void AddInstrument(InstrumentKey instrument);
    void RemoveInstrument(InstrumentKey instrument);

    // --- the work itself, with no lock and no venue translation -------------
    //
    // These hold everything the public entry points used to do inline. They
    // take a SLOT, already resolved by the caller, and assume the caller has
    // made them safe to run: after Start(), that is the fact that only the
    // consolidator thread calls them. Before Start(), it is the caller being
    // single-threaded - which is how the tests drive them.
    //
    // KEY: splitting these out is what lets the same logic be reached two
    // ways - synchronously under the mutex (today) and from the drained
    // queue on the consolidator thread (next step) - without the book logic
    // itself existing twice. Two copies of a merge that must agree is how
    // the two paths would silently diverge.
    //
    // Not thread-safe on their own, by design. Nothing here locks.
    // Records an enqueue failure for `index` and logs only the FIRST one, so
    // a persistently full queue leaves a counter rather than a flood of
    // identical lines on a path that is by then already failing.
    void NoteOverflow(size_t index, const char* what);

    // The consolidator thread's body: drain, work, sleep, repeat.
    void ConsolidatorLoop();

    // True if any venue queue has something waiting. Used as the condition
    // variable's predicate, so it is checked under doorbell_mutex_ and closes
    // the lost-wakeup window: a message pushed between the drain returning 0
    // and the wait starting is seen by the predicate rather than missed.
    bool HasPending() const;

    // Wakes the consolidator if it is asleep. Called after every successful
    // push.
    //
    // KEY: guarded by consumer_waiting_ so the common case costs one relaxed
    // atomic load and no notify at all. notify_one() on a condition variable
    // is a futex wake - cheap when someone is waiting, pure overhead on every
    // message when nobody is. During a burst the consolidator is awake and
    // draining, which is exactly when the notify would be wasted.
    void RingDoorbell();

    void ProcessUpdate(VenueSlot slot, const BookUpdate& update, int64_t lock_wait_ns);
    void ProcessQuote(VenueSlot slot, const BboQuote& quote);
    void ProcessHealth(VenueSlot slot, const VenueHealthEvent& event);

    // Which slots are live, and the VenueId each one maps to.
    //
    // has_value() is the ACTIVE flag. venue_registry_.size() cannot answer
    // this: it is a high-water mark that deliberately survives RemoveVenue, so
    // a registered-then-removed slot is still inside it and must not be given
    // a book when a new instrument arrives.
    //
    // Holds the VenueId because VenueBook's constructor still takes one. That
    // is the single remaining place Core depends on the enum, kept in one
    // visible spot so the step that migrates VenueBook to VenueSlot can delete
    // exactly this and nothing else.
    std::array<std::optional<VenueId>, kMaxVenues> active_venues_{};

    // VenueId -> slot. The single point where the venue identity carried on an
    // incoming update becomes the index Core stores by.
    //
    // KEY: without this, every array access here is array[VenueId], which is
    // only correct while slot N happens to equal VenueId N - i.e. while venues
    // register in enum order with no gaps. This table is what removes that
    // restriction: register OKX first and it takes slot 0, and every lookup
    // still finds it.
    //
    // Sized by kVenueCount, not kMaxVenues, and that is deliberate: it is a
    // translation table FROM the enum, not storage. Its length is the number
    // of VenueIds that exist, while the arrays it maps into are sized by slot
    // capacity. The two are different quantities that happen to look alike.
    //
    // Disappears entirely once providers carry their own slot on the wire
    // (§17.7) - Core will read the slot directly and translate nothing.
    std::array<std::optional<VenueSlot>, kVenueCount> venue_id_to_slot_{};

    // The slot this venue's data belongs in, or nullopt if it is not
    // registered. Callers drop the message rather than guessing - see the
    // "never create state from a data message" rule in §17.6.
    std::optional<VenueSlot> SlotFor(VenueId venue) const {
        const size_t index = static_cast<size_t>(venue);
        if (index >= venue_id_to_slot_.size()) {
            return std::nullopt;
        }
        return venue_id_to_slot_[index];
    }

    // Maps venue names to the dense slots that index venue_books_,
    // venue_quotes_, depth_health_ and bbo_health_ (DESIGN.md §17.6).
    //
    // Populated in Init() today, from CoreConfig::venues. Once providers dial
    // in over a socket (§17.4) the same call moves to the kHello handshake and
    // nothing else here changes - which is why the registry is keyed on the
    // venue NAME rather than on VenueId.
    //
    // Core still indexes by VenueId elsewhere. Migrating the loop bounds and
    // then the index itself are separate later steps; nothing reads this yet.
    VenueRegistry venue_registry_;

    // One SPSC ring per venue slot. Single producer: that venue's provider
    // thread. Single consumer: whoever calls DrainOnce - today a test, from
    // the next step the consolidator thread.
    //
    // Fixed array, not a vector: kMaxVenues rings cost 224 KB total
    // (256 slots x 112 bytes x 8, measured), and a vector would invalidate a
    // producer's reference to its own queue the moment another venue
    // registered and forced a reallocation - while that producer was
    // mid-push.
    //
    // Indexed by slot, so a dead slot simply holds an empty ring nobody
    // pushes to. ProviderQueue holds atomics, so it is neither copyable nor
    // movable - which makes Core non-movable too, and that is correct: a
    // Core whose queues were being read while it moved would be a race.
    std::array<ProviderQueue, kMaxVenues> queues_;

    // Written by producer threads, read by whoever reports. Relaxed ordering
    // throughout: these are counters, not synchronization - a reader that
    // sees a slightly stale value draws the same conclusion, and making them
    // ordered would put a barrier on the path this design exists to keep
    // clear.
    std::array<std::atomic<uint64_t>, kMaxVenues> overflow_count_{};
    std::array<std::atomic<uint64_t>, kMaxVenues> quote_drop_count_{};

    // --- the consolidator thread and its doorbell --------------------------
    //
    // The wakeup is COALESCED (§7.3): producers do not signal per message,
    // they signal only when the consumer is actually asleep, and one wakeup
    // covers every message that arrived meanwhile. Self-clocking - on a single
    // update in a quiet market it publishes immediately, and during a burst
    // the updates arriving while it works collapse into the next pass. No
    // timer to tune, and no artificial delay when idle.
    //
    // KEY: the consumer SLEEPS rather than spinning. Live depth arrives at
    // roughly 9 messages/sec per venue (measured), so a busy-poll loop would
    // burn a full core 24/7 to do nothing - a real defect in a system meant to
    // run continuously, not a micro-optimization.
    std::thread consolidator_;
    std::atomic<bool> running_{false};

    std::mutex doorbell_mutex_;
    std::condition_variable doorbell_;

    // Set only while the consolidator is inside the wait. Lets RingDoorbell
    // skip the notify entirely when the consumer is awake and draining.
    std::atomic<bool> consumer_waiting_{false};


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
    // Written and read only on the consolidator thread (via ProcessHealth /
    // ProcessUpdate), so no lock guards them. That single-threadedness is the
    // property Start() establishes, and it is why §7.2 can say there is no
    // lock on the book path.
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
    std::unordered_map<InstrumentKey, uint64_t, InstrumentKeyHash> bbo_health_version_seen_;

    BboCallback bbo_callback_;
    BookCallback book_callback_;

    // Both null unless SetInstrumentation was called. Read on the hot path,
    // so the null check is the entire cost in a normal run.
    ClockFn clock_;
    TimingsCallback timings_callback_;
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
    std::unordered_map<InstrumentKey, std::vector<std::shared_ptr<consolidated::Book>>, InstrumentKeyHash>
        book_pools_;

    // Returns a Book buffer for `instrument` that no subscriber currently
    // holds, reusing one from the pool if possible. Not thread-safe on its
    // own - called only on the consolidator thread.
    std::shared_ptr<consolidated::Book> AcquireBookBuffer(InstrumentKey instrument);
};

}  // namespace market_data
