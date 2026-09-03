#include "md_core.h"
namespace market_data {

Core::Core(BboCallback bbo_callback, BookCallback book_callback)
    : bbo_callback_(std::move(bbo_callback)), book_callback_(std::move(book_callback)) {}

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
    for (InstrumentId instrument : config.default_instruments) {
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
    ApplyTimings timings;

    // Sampled BEFORE the lock, so lock_wait_ns captures the time actually
    // spent blocked on other provider threads. Taking it after would measure
    // nothing - that is the whole point of splitting this out.
    const int64_t t_before_lock = instrumented ? clock_() : 0;

    // Interim fix (see the comment on apply_mutex_ in md_core.h) - multiple
    // Provider threads can call this concurrently on the same Core.
    std::lock_guard<std::mutex> lock(apply_mutex_);

    const int64_t t_locked = instrumented ? clock_() : 0;
    if (instrumented) {
        timings.lock_wait_ns = t_locked - t_before_lock;
        timings.delta_levels = static_cast<uint32_t>(update.bids.size() + update.asks.size());
    }

    auto venue_it = venue_books_.find(update.instrument);
    if (venue_it == venue_books_.end()) {
        Logger::Log(LogLevel::kWarning, "Received update for unknown instrument: {}",
                    VenueConverter::ToInstrumentString(update.instrument));
        return;
    }

    // Translate the wire's venue identity into Core's storage index. An
    // unregistered venue has no slot, so its update is DROPPED - Core never
    // creates state from a data message (§17.6). This is also the path an
    // update still in flight when its venue was removed takes.
    const std::optional<VenueSlot> slot = SlotFor(update.venue);
    if (!slot.has_value()) {
        Logger::Log(LogLevel::kWarning, "Received update for unregistered venue: {}",
                    VenueConverter::ToVenueString(update.venue));
        return;
    }

    auto& book_ptr = venue_it->second[VenueSlotIndex(*slot)];
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

        // Recorded per merge rather than sampled from outside, because a
        // sample taken from another thread would need this same lock and
        // would still not correspond to any particular merge.
        for (size_t i = 0; i < kVenueCount; ++i) {
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

void Core::OnVenueHealth(const VenueHealthEvent& event) {
    // Same lock as the book path: this writes state ApplyUpdate reads, and
    // arrives on a different provider's thread. Cheap because the event is
    // edge-triggered - a handful of calls in a healthy run, not per tick.
    std::lock_guard<std::mutex> lock(apply_mutex_);

    // Same translation as ApplyUpdate: the health arrays are indexed by slot,
    // and the event carries a VenueId. A verdict for an unregistered venue has
    // nowhere to go and is discarded rather than written to slot
    // static_cast<size_t>(venue), which would be some other venue's verdict.
    const std::optional<VenueSlot> slot = SlotFor(event.venue);
    if (!slot.has_value()) {
        return;
    }
    const size_t index = VenueSlotIndex(*slot);

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

std::shared_ptr<consolidated::Book> Core::AcquireBookBuffer(InstrumentId instrument) {
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
    std::lock_guard<std::mutex> lock(apply_mutex_);

    auto quotes_it = venue_quotes_.find(quote.instrument);
    if (quotes_it == venue_quotes_.end()) {
        Logger::Log(LogLevel::kWarning, "Received quote for unknown instrument: {}",
                    VenueConverter::ToInstrumentString(quote.instrument));
        return;
    }

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
    const VenueSlot slot = *maybe_slot;

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

void Core::AddInstrument(InstrumentId instrument) {
    // Starts ALL NULL, then fills the slots of venues already registered. An
    // instrument added before any provider has connected is legal and simply
    // publishes nothing until one does - which is the same state a venue that
    // has not spoken yet is in, and the merge already skips it.
    VenueBookArray venue_books;
    for (size_t index = 0; index < active_venues_.size(); ++index) {
        if (active_venues_[index].has_value()) {
            venue_books[index] = std::make_unique<VenueBook>(*active_venues_[index], instrument);
        }
    }
    venue_books_[instrument] = std::move(venue_books);

    // Quotes are value-initialized (all fields 0) - a venue's slot stays
    // "no data yet" until its first fast-BBO message arrives, which is
    // exactly what ComputeBBOFromQuotes skips on.
    venue_quotes_[instrument] = VenueQuoteArray{};
}

std::optional<VenueSlot> Core::RegisterVenue(std::string_view name) {
    // VenueBook's constructor still takes a VenueId, so the name has to be
    // converted back exactly once, here. This is the last dependency Core has
    // on the enum; the step that migrates VenueBook to VenueSlot removes it.
    const VenueId venue = VenueConverter::ToVenueId(std::string(name));
    if (venue == VenueId::COUNT) {
        Logger::Log(LogLevel::kError, "[Core] RegisterVenue: unknown venue name '{}' - refusing", name);
        return std::nullopt;
    }

    std::lock_guard<std::mutex> lock(apply_mutex_);

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
            books[index] = std::make_unique<VenueBook>(venue, existing_instrument);
        }
    }

    Logger::Log(LogLevel::kInfo, "[Core] venue '{}' registered in slot {} ({} active)", name, index,
                venue_registry_.size());
    return slot;
}

void Core::RemoveVenue(VenueSlot slot) {
    const size_t index = VenueSlotIndex(slot);

    std::lock_guard<std::mutex> lock(apply_mutex_);

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

void Core::RemoveInstrument(InstrumentId instrument) {
    venue_books_.erase(instrument);
    venue_quotes_.erase(instrument);
}
}  // namespace market_data