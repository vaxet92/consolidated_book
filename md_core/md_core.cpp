#include "md_core.h"

#include <variant>

#if defined(__x86_64__)
#include <immintrin.h>
#endif

namespace market_data {

namespace {

// A CPU hint that this thread is spinning: it lets the core back off, saves
// power, and on SMT hardware releases issue slots to the sibling thread.
//
// KEY: this is NOT std::this_thread::yield(). That one is a SYSCALL
// (sched_yield) that hands the core to the OS scheduler, costing hundreds of
// nanoseconds and possibly descheduling this thread entirely - unbounded,
// unpredictable jitter on a path where the whole point is a predictable
// upper bound. This is a single instruction with no scheduler involvement.
//
// The ARM instruction is confusingly also spelled "yield", but it is a
// hint like x86's PAUSE, not a system call.
// 64-bit targets only, both of them: __aarch64__ is what this machine and an
// Apple-Silicon Docker build produce, __x86_64__ is what Docker produces on
// any Intel/AMD host (Dockerfile pins no platform). 32-bit guards are
// deliberately absent - this project will not run on them.
inline void CpuPause() noexcept {
#if defined(__x86_64__)
    _mm_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield" ::: "memory");
#else
#error "Unsupported architecture - add a pause primitive for this target"
#endif
}

// How many times to retry before applying the overflow policy.
//
// NOT MEASURED. This is a bounded-wait constant, not a tuned one: its only
// job is to give the consumer a brief window to free a slot without ever
// letting the producer wait without limit. If overflow_count_ is ever
// non-zero in production, that is the signal to revisit this with data -
// the counter exists so the question can be answered rather than guessed.
constexpr int kEnqueueSpinAttempts = 64;

// How many CpuPause iterations the consolidator spins after an empty drain
// before falling back to sleeping. See the KEY in ConsolidatorLoop for the
// sizing rule: this must exceed the expected gap between messages, or the
// consumer sleeps and every message pays the ~37 us wakeup.
//
// Sizing, with the measured part separated from the estimated part:
//   MEASURED: one bare CpuPause is 0.66 ns on this machine (Apple M4, stable
//   from 10k to 1M iterations). 200k bare pauses would be only ~132 us.
//   NOT MEASURED: each spin iteration also runs an empty DrainOnce() - a
//   venue_count() acquire load plus two atomic loads per queue - which
//   dominates the pause. That puts the real window somewhere in the low
//   milliseconds, not 132 us, but the per-iteration cost has not been
//   isolated.
// The live book_publish median is the check that matters: if it stays near
// the mutex-era number, the window is long enough and the consumer is not
// sleeping. If it sits at the +37 us sleep penalty, raise this.
//
// Two deliberate settings:
//   raise toward infinity -> pure pinned-core HFT spin, never sleeps
//   set to 0              -> sleep immediately, the pre-spin behaviour
constexpr uint32_t kConsolidatorSpinLimit = 200'000;

// One bounded attempt to hand `message` to the ring. Returns false if the
// queue stayed full for the whole window; the caller then applies the policy
// for that message type.
//
// KEY: SpscQueue::TryPush checks for space and returns false BEFORE it
// touches `message`, so a failed attempt leaves it intact and the next
// iteration re-sends the same object rather than an empty husk. That
// property is load-bearing here and is covered by
// SpscQueueTest.TryPushOnFullDoesNotConsumeTheValue.
[[nodiscard]] bool TryEnqueueBounded(ProviderQueue& queue, ProviderMessage message) {
    for (int attempt = 0; attempt < kEnqueueSpinAttempts; ++attempt) {
        if (queue.TryPush(std::move(message))) {
            return true;
        }
        CpuPause();
    }
    return false;
}

}  // namespace

Core::Core(BboCallback bbo_callback, BookCallback book_callback)
    : bbo_callback_(std::move(bbo_callback)), book_callback_(std::move(book_callback)) {}

Core::~Core() { Stop(); }

void Core::Start() {
    if (running_.exchange(true, std::memory_order_acq_rel)) {
        return;  // already running - do not spawn a second consolidator
    }
    consolidator_ = std::thread([this] { ConsolidatorLoop(); });
}

void Core::Stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;  // never started, or already stopped
    }

    // Under the lock so the store to running_ above cannot land between the
    // consolidator's predicate check and its wait - the classic lost-wakeup
    // on shutdown, which would hang the join forever.
    {
        std::lock_guard<std::mutex> lock(doorbell_mutex_);
        doorbell_.notify_all();
    }

    if (consolidator_.joinable()) {
        consolidator_.join();
    }
}

bool Core::HasPending() const {
    const size_t count = venue_count();
    for (size_t index = 0; index < count; ++index) {
        if (!queues_[index].Empty()) {
            return true;
        }
    }
    return false;
}

void Core::RingDoorbell() {
    // Fast path: the consolidator is awake and will see this message on its
    // current or next drain pass, so there is nothing to wake.
    if (!consumer_waiting_.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(doorbell_mutex_);
    doorbell_.notify_one();
}

void Core::ConsolidatorLoop() {
    uint32_t idle_spins = 0;

    while (running_.load(std::memory_order_acquire)) {
        // Drain outside any lock - this does the real work (book apply, merge,
        // publish callbacks). Holding doorbell_mutex_ across a subscriber
        // callback would invite a deadlock and would block every producer's
        // notify for the length of a merge.
        if (DrainOnce() > 0) {
            idle_spins = 0;
            continue;  // more may have arrived while we worked
        }

        // --- the fast idle path: spin, no mutex, no syscall ----------------
        //
        // KEY: this is the whole latency story. Sleeping the moment a drain
        // comes back empty means the NEXT message has to wake a descheduled
        // thread - a futex syscall plus a scheduler round trip, MEASURED at
        // ~37 us of added end-to-end latency (becnhmark_results.md). Spinning
        // instead costs one CpuPause per iteration and sees the message on the
        // next poll.
        //
        // Sized to be LONGER than the expected gap between messages, which is
        // what makes the consumer never reach the sleep below under live load:
        //
        //   3 venues, spot only        ~240 msg/sec   gap ~4.2 ms
        //   3 venues, spot + futures   ~480 msg/sec   gap ~2.1 ms
        //   8 venues, spot + futures  ~1300 msg/sec   gap ~0.8 ms
        //
        // (240/sec is measured: ~215 BBO + ~25 depth publishes per second
        // across three venues. The rest scale from it.)
        if (idle_spins < kConsolidatorSpinLimit) {
            ++idle_spins;
            CpuPause();
            continue;
        }

        // --- the dead-feed path: only reached when nothing arrived for the
        // whole spin window ------------------------------------------------
        //
        // Every venue disconnected, or the market genuinely stopped. Sleeping
        // here is right: a 24/7 process must not pin a core for hours because
        // a feed is down. Under live load this is unreachable, so the mutex
        // below is off the message path entirely - producers only touch it
        // while consumer_waiting_ is true, which live traffic never sets.
        std::unique_lock<std::mutex> lock(doorbell_mutex_);
        consumer_waiting_.store(true, std::memory_order_release);

        // The predicate is re-evaluated under the lock, which is what makes a
        // message pushed between the spin above and this wait impossible to
        // miss.
        //
        // The timeout is a backstop, not the mechanism: if a notify is ever
        // lost the cost is 50 ms of extra latency on one message rather than a
        // permanently wedged consolidator.
        doorbell_.wait_for(lock, std::chrono::milliseconds(50),
                           [this] { return !running_.load(std::memory_order_acquire) || HasPending(); });

        consumer_waiting_.store(false, std::memory_order_release);
        idle_spins = 0;
    }

    // Final drain: whatever the providers already handed over gets applied
    // rather than silently discarded at shutdown.
    DrainOnce();
}

void Core::Init(const CoreConfig& config) {
    // Reserve BEFORE inserting - reserving after the loop is too late, the
    // maps have already grown and rehashed.
    venue_books_.reserve(config.default_instruments.size());
    venue_quotes_.reserve(config.default_instruments.size());
    consolidated_bbo_.reserve(config.default_instruments.size());

    // KEY: Init registers NO venues. It allocates capacity and nothing else.
    // Venues appear when a provider appears (Core::RegisterVenue), never
    // because config named one - see md_core.h. config.venues is still read by
    // main.cpp to decide which providers to construct; Core no longer uses it,
    // and it is removed from CoreConfig in a later step.
    for (InstrumentKey instrument : config.default_instruments) {
        AddInstrument(instrument);

        // Reserve the attribution vectors once, so the hot path
        // (UpdateBBOWithQuote) never allocates: it only ever clear()s and
        // push_back()s within this capacity, and never assigns a whole level.
        //
        // Sized off kMaxVenues, not the venues registered so far: this is
        // reserved once at startup, when nothing has registered yet, and any
        // number of venues up to the cap may contribute to a level later.
        // Under-reserving would put an allocation back on the hot path the
        // first time a venue joined.
        auto& bbo = consolidated_bbo_[instrument];
        bbo.best_bid.venues.reserve(kMaxVenues);
        bbo.best_ask.venues.reserve(kMaxVenues);
    }
}

// Fed by whoever owns the Provider(s) (the wiring layer, e.g. main.cpp).
// Core has no knowledge of providers, sockets, or threads.
void Core::ApplyUpdate(const BookUpdate& update) {
    // Instrumentation is opt-in and off by default; `instrumented` collapses
    // to a constant-false branch in a normal run.
    const bool instrumented = static_cast<bool>(clock_);

    // Sampled BEFORE the lock, so lock_wait_ns captures the time actually
    // spent blocked on other provider threads. Taking it after would measure
    // nothing - that is the whole point of splitting this out.
    const int64_t t_before_lock = instrumented ? clock_() : 0;

    const int64_t lock_wait_ns = instrumented ? clock_() - t_before_lock : 0;

    // Translate the wire's venue identity into Core's storage index. An
    // unregistered venue has no slot, so its update is DROPPED - Core never
    // creates state from a data message (§17.6). This is also the path an
    // update still in flight when its venue was removed takes.
    //
    // Resolved HERE rather than inside ProcessUpdate: once providers carry
    // their own slot (§17.7) this translation disappears entirely, and the
    // queued path already skips it - the caller knows its slot for the life
    // of its connection.
    const std::optional<VenueSlot> slot = SlotFor(update.venue);
    if (!slot.has_value()) {
        Logger::Log(LogLevel::kWarning, "Received update for unregistered venue: {}",
                    VenueConverter::ToVenueString(update.venue));
        return;
    }

    ProcessUpdate(*slot, update, lock_wait_ns);
}

// Runs with no lock of its own - see the declaration in md_core.h.
void Core::ProcessUpdate(VenueSlot slot, const BookUpdate& update, int64_t lock_wait_ns) {
    const bool instrumented = static_cast<bool>(clock_);
    ApplyTimings timings;

    const int64_t t_locked = instrumented ? clock_() : 0;
    if (instrumented) {
        timings.lock_wait_ns = lock_wait_ns;
        timings.delta_levels = static_cast<uint32_t>(update.bids.size() + update.asks.size());
    }

    auto venue_it = venue_books_.find(update.instrument);
    if (venue_it == venue_books_.end()) {
        Logger::Log(LogLevel::kWarning, "Received update for unknown instrument: {}",
                    VenueConverter::ToInstrumentString(update.instrument));
        return;
    }

    auto& book_ptr = venue_it->second[VenueSlotIndex(slot)];
    if (!book_ptr) {
        Logger::Log(LogLevel::kWarning, "Received update for unconfigured venue: {}",
                    VenueConverter::ToVenueString(update.venue));
        return;
    }

    book_ptr->ApplyUpdate(update);

    const int64_t t_applied = instrumented ? clock_() : 0;
    if (instrumented) {
        timings.book_apply_ns = t_applied - t_locked;
    }

    // No bbo_callback_ here on purpose: the fast-BBO stream publishes the
    // BBO (see ApplyQuote). If both paths published, the client's stream
    // would alternate between depth-derived and quote-derived values from
    // two unsynchronized sources - the mixing §7 rules out, just moved to
    // the output side.
    //
    // Eager full merge, on every update (see the comment on ApplyUpdate in
    // md_core.h for why, and its provisional status pending a benchmark).
    // Core decides nothing about bands here - it only produces the merged
    // book; which bands to compute from it, for whom, is the subscriber's
    // job (§8.4).
    if (book_callback_) {
        std::shared_ptr<consolidated::Book> merged = AcquireBookBuffer(update.instrument);
        // The staleness verdict finally takes effect here. Passing
        // &depth_health_ rather than nullptr is the single line that turns
        // the whole policy from inert into live: a venue whose verdict is not
        // kLive stops contributing levels, attribution and depth.
        //
        // depth_health_, not bbo_health_ - this is the depth book, and the
        // two streams are separate sockets that fail independently (§6.2d).
        consolidated::MergeBooks(venue_it->second, venue_count(), *merged, consolidated::kDefaultMaxDepth,
                                 &depth_health_);

        // Set AFTER MergeBooks: Clear() resets the vectors and the merge knows
        // nothing about provenance, so this has to be stamped here, by the one
        // component that holds both the update and the output.
        //
        // Carries the ORIGINATING update's arrival time, not "now" - the point
        // is to measure how long this snapshot took to get here, which
        // includes the handoff we are about to replace.
        merged->source_mono_ns = update.recv_mono_ns;

        // Recorded per merge rather than sampled from outside: a sample taken
        // from another thread would not correspond to any particular merge.
        //
        // venue_count(), not kVenueCount: this loop was missed by the
        // venue-slot migration (§17.6) and stopped at 3 while venue_levels is
        // sized kMaxVenues, so a venue in slot 3 or beyond reported depth 0
        // forever. Diagnostic-only - it feeds LatencyRecorder's depth report,
        // never the published book - but it is the same "registers fine, then
        // never appears" failure the migration existed to remove.
        for (size_t i = 0; i < venue_count(); ++i) {
            const auto& venue_book = venue_it->second[i];
            merged->venue_levels[i] = venue_book ? static_cast<uint32_t>(venue_book->bids().size()) : 0;
        }

        if (instrumented) {
            // Sampled before book_callback_, so the merge figure is the merge
            // alone and does not absorb whatever the publisher does.
            timings.merge_ns = clock_() - t_applied;
            timings.merged_depth = static_cast<uint32_t>(merged->bids.size());
        }

        book_callback_(update.instrument, merged);
    }

    if (instrumented && timings_callback_) {
        timings_callback_(timings);
    }
}

// --- queued path ------------------------------------------------------------

// Counts an overflow and logs only the FIRST one for that slot. Overflow is
// meant to be rare; if it is not, one line plus a counter says so without
// the log spam a per-message warning would produce on a path that is by
// then already failing.
void Core::NoteOverflow(size_t index, const char* what) {
    const uint64_t previous = overflow_count_[index].fetch_add(1, std::memory_order_relaxed);
    if (previous == 0) {
        Logger::Log(LogLevel::kError, "[Core] slot {} queue full - {} could not be enqueued, resync required", index,
                    what);
    }
}

bool Core::EnqueueUpdate(VenueSlot slot, BookUpdate update) {
    const size_t index = VenueSlotIndex(slot);
    if (index >= queues_.size()) {
        // Only reachable with a fabricated slot - the registry never issues
        // one past kMaxVenues.
        Logger::Log(LogLevel::kError, "[Core] EnqueueUpdate: slot {} out of range", index);
        return false;
    }

    if (TryEnqueueBounded(queues_[index], ProviderMessage{std::move(update)})) {
        RingDoorbell();
        return true;
    }

    // The diff chain is broken from here on. Core deliberately does NOT
    // resync itself - it does not own a socket and does not know how this
    // venue recovers (§9). Reporting the failure is the whole contract; the
    // provider calls RequestResync().
    NoteOverflow(index, "depth update");
    return false;
}

void Core::EnqueueQuote(VenueSlot slot, BboQuote quote) {
    const size_t index = VenueSlotIndex(slot);
    if (index >= queues_.size()) {
        Logger::Log(LogLevel::kError, "[Core] EnqueueQuote: slot {} out of range", index);
        return;
    }

    if (TryEnqueueBounded(queues_[index], ProviderMessage{quote})) {
        RingDoorbell();
        return;
    }

    // Safe to lose: a quote is a complete top-of-book snapshot, so the next
    // one replaces it whole. Counted, never logged per message - a busy
    // conflating stream would otherwise be the noisiest thing in the log.
    quote_drop_count_[index].fetch_add(1, std::memory_order_relaxed);
}

bool Core::EnqueueHealth(VenueSlot slot, VenueHealthEvent event) {
    const size_t index = VenueSlotIndex(slot);
    if (index >= queues_.size()) {
        Logger::Log(LogLevel::kError, "[Core] EnqueueHealth: slot {} out of range", index);
        return false;
    }

    if (TryEnqueueBounded(queues_[index], ProviderMessage{event})) {
        RingDoorbell();
        return true;
    }

    // A transition, not a sample. Losing kStale would leave Core merging a
    // venue that has gone quiet, indefinitely - the one failure this whole
    // staleness design exists to prevent.
    NoteOverflow(index, "health event");
    return false;
}

uint64_t Core::OverflowCount(VenueSlot slot) const {
    const size_t index = VenueSlotIndex(slot);
    return index < overflow_count_.size() ? overflow_count_[index].load(std::memory_order_relaxed) : 0;
}

uint64_t Core::QuoteDropCount(VenueSlot slot) const {
    const size_t index = VenueSlotIndex(slot);
    return index < quote_drop_count_.size() ? quote_drop_count_[index].load(std::memory_order_relaxed) : 0;
}

size_t Core::DrainOnce() {
    // Adding an alternative to ProviderMessage must not silently fall
    // through the dispatch below - this fails the build and points here.
    static_assert(std::variant_size_v<ProviderMessage> == 3,
                  "DrainOnce dispatches exactly three alternatives - handle the new one");

    size_t drained = 0;

    // venue_count(), not kMaxVenues: a message can only be in queue i if
    // something enqueued to slot i, which requires a slot the registry
    // issued, so i is always below the high-water mark. Reading it here is
    // safe against a concurrent RegisterVenue - VenueRegistry::size() is an
    // acquire load, and a venue registering mid-drain simply gets picked up
    // on the next pass.
    const size_t count = venue_count();

    for (size_t index = 0; index < count; ++index) {
        const VenueSlot slot = static_cast<VenueSlot>(index);
        ProviderMessage message;

        // Drains this venue fully before moving on. Not starvation: each
        // ring holds at most kProviderQueueCapacity messages, so a busy
        // venue can delay a quiet one by a bounded amount. The capacity is
        // doing the job a per-source read budget would otherwise do
        // (DESIGN.md §17.6).
        while (queues_[index].TryPop(message)) {
            if (auto* update = std::get_if<BookUpdate>(&message)) {
                // lock_wait_ns = 0: there is no lock on this path. When the
                // consolidator thread lands, the useful figure here becomes
                // time spent waiting IN the queue, which is a different
                // measurement and needs a stamp on the message to compute.
                ProcessUpdate(slot, *update, /*lock_wait_ns=*/0);
            } else if (auto* quote = std::get_if<BboQuote>(&message)) {
                ProcessQuote(slot, *quote);
            } else if (auto* health = std::get_if<VenueHealthEvent>(&message)) {
                ProcessHealth(slot, *health);
            } else {
                Logger::Log(LogLevel::kError, "[Core] DrainOnce: unhandled message kind {}", message.index());
            }
            ++drained;
        }
    }

    return drained;
}

void Core::OnVenueHealth(const VenueHealthEvent& event) {
    // Same translation as ApplyUpdate: the health arrays are indexed by slot,
    // and the event carries a VenueId. A verdict for an unregistered venue has
    // nowhere to go and is discarded rather than written to slot
    // static_cast<size_t>(venue), which would be some other venue's verdict.
    const std::optional<VenueSlot> slot = SlotFor(event.venue);
    if (!slot.has_value()) {
        return;
    }

    ProcessHealth(*slot, event);
}

// Runs with no lock of its own - see the declaration in md_core.h.
void Core::ProcessHealth(VenueSlot slot, const VenueHealthEvent& event) {
    const size_t index = VenueSlotIndex(slot);

    VenueHealthArray& target = (event.stream == StreamKind::kDepth) ? depth_health_ : bbo_health_;
    if (target[index] == event.health) {
        return;  // provider is edge-triggered already, but do not depend on it
    }
    target[index] = event.health;

    // Only the BBO needs invalidating. The merged Book is rebuilt from
    // scratch on every update, so it picks the new verdict up for free.
    if (event.stream == StreamKind::kBbo) {
        ++bbo_health_version_;
    }

    // Deliberately does NOT republish. A health change alters what the NEXT
    // merge produces, and the next update is normally milliseconds away. The
    // one case that argument fails is a venue going stale in a quiet market,
    // where "the next update" may never come - the client then keeps a book
    // that still includes the venue we just excluded.
    //
    // TODO: publish on a health change once the total-outage path is settled.
    // Left out here because republishing needs an instrument, and this event
    // is per VENUE - Core would have to fan it out across every instrument,
    // which is the right design only after multi-symbol is real (§16.1).
}

std::shared_ptr<consolidated::Book> Core::AcquireBookBuffer(InstrumentKey instrument) {
    auto& pool = book_pools_[instrument];
    for (auto& buffer : pool) {
        if (buffer.use_count() == 1) {
            // No subscriber still holds this one - safe to overwrite.
            // MergeBooks' internal Clear() keeps capacity, so reusing it
            // costs no allocation after warm-up (§7.5).
            return buffer;
        }
    }
    // Every buffer is still referenced (or the pool is empty on the first
    // call) - grow by one. Under "assume no slow subscriber" this should
    // stabilize at a small size almost immediately, not grow unbounded.
    pool.push_back(std::make_shared<consolidated::Book>());
    return pool.back();
}

void Core::ApplyQuote(const BboQuote& quote) {
    // Same translation as ApplyUpdate. A quote from an unregistered venue is
    // dropped: storing it at static_cast<size_t>(quote.venue) would overwrite
    // whatever venue actually holds that slot, and the BBO would then publish
    // one venue's price under another's name.
    const std::optional<VenueSlot> maybe_slot = SlotFor(quote.venue);
    if (!maybe_slot.has_value()) {
        Logger::Log(LogLevel::kWarning, "Received quote for unregistered venue: {}",
                    VenueConverter::ToVenueString(quote.venue));
        return;
    }

    ProcessQuote(*maybe_slot, quote);
}

// Runs with no lock of its own - see the declaration in md_core.h.
void Core::ProcessQuote(VenueSlot slot, const BboQuote& quote) {
    auto quotes_it = venue_quotes_.find(quote.instrument);
    if (quotes_it == venue_quotes_.end()) {
        Logger::Log(LogLevel::kWarning, "Received quote for unknown instrument: {}",
                    VenueConverter::ToInstrumentString(quote.instrument));
        return;
    }

    // Store BEFORE folding it in: UpdateBBOWithQuote's rescan path re-reads
    // this array, so it must already hold the new quote (see the precondition
    // on that function).
    quotes_it->second[VenueSlotIndex(slot)] = quote;

    consolidated::BBO& bbo = consolidated_bbo_[quote.instrument];

    // KEY: a health change forces a FULL rescan, it is not merely filtered
    // forward. UpdateBBOWithQuote carries persistent state, so a venue that
    // has gone stale still has its price sitting inside `bbo` - and having
    // gone quiet, it will never send another quote to displace it. Only a
    // rescan of the whole quote array can remove it.
    //
    // Rare by construction: bbo_health_version_ moves a handful of times in a
    // healthy run, so the O(venues) rescan is not on the hot path.
    uint64_t& seen = bbo_health_version_seen_[quote.instrument];
    if (seen != bbo_health_version_) {
        // The new quote is already in `quotes` (stored above), so the rescan
        // includes it - there is nothing left to fold in afterwards.
        consolidated::ComputeBBOFromQuotesInto(quotes_it->second, bbo, &bbo_health_);
        seen = bbo_health_version_;
    } else {
        consolidated::UpdateBBOWithQuote(bbo, quote, slot, quotes_it->second, &bbo_health_);
    }

    if (bbo_callback_) {
        bbo_callback_(quote.instrument, bbo);
    }
}

void Core::AddInstrument(InstrumentKey instrument) {
    // Starts ALL NULL, then fills the slots of venues already registered. An
    // instrument added before any provider has connected is legal and simply
    // publishes nothing until one does - which is the same state a venue that
    // has not spoken yet is in, and the merge already skips it.
    FlatBookArray venue_books;
    for (size_t index = 0; index < active_venues_.size(); ++index) {
        if (active_venues_[index].has_value()) {
            venue_books[index] = std::make_unique<FlatOrderBook>(*active_venues_[index], instrument);
        }
    }
    venue_books_[instrument] = std::move(venue_books);

    // Quotes are value-initialized (all fields 0) - a venue's slot stays
    // "no data yet" until its first fast-BBO message arrives, which is
    // exactly what ComputeBBOFromQuotes skips on.
    venue_quotes_[instrument] = VenueQuoteArray{};
}

std::optional<VenueSlot> Core::RegisterVenue(std::string_view name) {
    // FlatOrderBook's constructor still takes a VenueId, so the name has to be
    // converted back exactly once, here. This is the last dependency Core has
    // on the enum; the step that migrates the book to VenueSlot removes it.
    const VenueId venue = VenueConverter::ToVenueId(std::string(name));
    if (venue == VenueId::COUNT) {
        Logger::Log(LogLevel::kError, "[Core] RegisterVenue: unknown venue name '{}' - refusing", name);
        return std::nullopt;
    }

    const std::optional<VenueSlot> slot = venue_registry_.Register(name);
    if (!slot.has_value()) {
        Logger::Log(LogLevel::kError, "[Core] RegisterVenue: registry full at {} venues, refusing '{}'", kMaxVenues,
                    name);
        return std::nullopt;
    }
    const size_t index = VenueSlotIndex(*slot);

    // Slot and VenueId are now INDEPENDENT. Registering OKX first gives it
    // slot 0, and venue_id_to_slot_ is what makes every later lookup find it.
    // There is deliberately no check that the two agree - requiring that was
    // the restriction this table removes.
    //
    // Idempotent: a provider that crashed and reconnected re-registers the
    // same name, gets the same slot, and finds its books already there.
    active_venues_[index] = venue;
    venue_id_to_slot_[static_cast<size_t>(venue)] = *slot;
    for (auto& [existing_instrument, books] : venue_books_) {
        if (!books[index]) {
            books[index] = std::make_unique<FlatOrderBook>(venue, existing_instrument);
        }
    }

    Logger::Log(LogLevel::kInfo, "[Core] venue '{}' registered in slot {} ({} active)", name, index,
                venue_registry_.size());
    return slot;
}

void Core::RemoveVenue(VenueSlot slot) {
    const size_t index = VenueSlotIndex(slot);

    if (index >= venue_registry_.size() || !active_venues_[index].has_value()) {
        Logger::Log(LogLevel::kWarning, "[Core] RemoveVenue: slot {} is not active - ignoring", index);
        return;
    }

    // Deactivate, do not release. venue_registry_ keeps the name -> slot
    // mapping so a reconnecting provider lands back here (see md_core.h).
    // venue_id_to_slot_ IS cleared, so an update that arrives after this point
    // finds no slot and is dropped rather than landing in a freed book.
    venue_id_to_slot_[static_cast<size_t>(*active_venues_[index])].reset();
    active_venues_[index].reset();

    for (auto& [instrument, books] : venue_books_) {
        books[index].reset();
    }

    // KEY: the quote must be CLEARED, not just orphaned. consolidated_bbo_ is
    // maintained incrementally, so this venue's last price is already inside
    // it and the venue will never send another quote to displace it. Leaving
    // the slot populated would let a full rescan pick the price of a venue
    // that no longer exists.
    for (auto& [instrument, quotes] : venue_quotes_) {
        quotes[index] = BboQuote{};
    }

    depth_health_[index] = VenueHealth::kNoData;
    bbo_health_[index] = VenueHealth::kNoData;

    // Forces every instrument's next quote onto the full-rescan path, for the
    // same reason: an incremental update cannot remove a price that is already
    // in the running BBO.
    ++bbo_health_version_;

    Logger::Log(LogLevel::kInfo, "[Core] venue slot {} removed", index);
}

void Core::RemoveInstrument(InstrumentKey instrument) {
    venue_books_.erase(instrument);
    venue_quotes_.erase(instrument);
}
}  // namespace market_data