#include "md_core.h"
using namespace market_data;

void Core::Init(const CoreConfig& config) {
    // Reserve BEFORE inserting - reserving after the loop is too late, the
    // maps have already grown and rehashed.
    venue_books_.reserve(config.default_instruments.size());
    venue_quotes_.reserve(config.default_instruments.size());
    consolidated_bbo_.reserve(config.default_instruments.size());

    for (InstrumentId instrument : config.default_instruments) {
        AddInstrument(instrument, config.venues);

        // Reserve the attribution vectors once, so the hot path
        // (UpdateBBOWithQuote) never allocates: it only ever clear()s and
        // push_back()s within this capacity, and never assigns a whole level.
        // Sized off VenueId::COUNT, so adding venues needs no change here.
        auto& bbo = consolidated_bbo_[instrument];
        bbo.best_bid.venues.reserve(static_cast<size_t>(VenueId::COUNT));
        bbo.best_ask.venues.reserve(static_cast<size_t>(VenueId::COUNT));
    }
}

// Fed by whoever owns the Provider(s) (the wiring layer, e.g. main.cpp).
// Core has no knowledge of providers, sockets, or threads.
void Core::ApplyUpdate(const BookUpdate& update) {
    // Interim fix (see the comment on apply_mutex_ in md_core.h) - multiple
    // Provider threads can call this concurrently on the same Core.
    std::lock_guard<std::mutex> lock(apply_mutex_);

    auto venue_it = venue_books_.find(update.instrument);
    if (venue_it == venue_books_.end()) {
        Logger::Log(LogLevel::kWarning, "Received update for unknown instrument: {}",
                    VenueConverter::ToInstrumentString(update.instrument));
        return;
    }

    auto& book_ptr = venue_it->second[static_cast<size_t>(update.venue)];
    if (!book_ptr) {
        Logger::Log(LogLevel::kWarning, "Received update for unconfigured venue: {}",
                    VenueConverter::ToVenueString(update.venue));
        return;
    }

    book_ptr->ApplyUpdate(update);

    // No bbo_callback_ here on purpose: the fast-BBO stream publishes the
    // BBO (see ApplyQuote). If both paths published, the client's stream
    // would alternate between depth-derived and quote-derived values from
    // two unsynchronized sources - the mixing §7 rules out, just moved to
    // the output side. The books are maintained here for band math
    // (§8.2/§8.3), which is not built yet.
}

void Core::ApplyQuote(const BboQuote& quote) {
    std::lock_guard<std::mutex> lock(apply_mutex_);

    auto quotes_it = venue_quotes_.find(quote.instrument);
    if (quotes_it == venue_quotes_.end()) {
        Logger::Log(LogLevel::kWarning, "Received quote for unknown instrument: {}",
                    VenueConverter::ToInstrumentString(quote.instrument));
        return;
    }

    // Store BEFORE folding it in: UpdateBBOWithQuote's rescan path re-reads
    // this array, so it must already hold the new quote (see the precondition
    // on that function).
    quotes_it->second[static_cast<size_t>(quote.venue)] = quote;

    consolidated::BBO& bbo = consolidated_bbo_[quote.instrument];
    consolidated::UpdateBBOWithQuote(bbo, quote, quotes_it->second);

    if (bbo_callback_) {
        bbo_callback_(quote.instrument, bbo);
    }
}

void Core::AddInstrument(InstrumentId instrument, const std::vector<VenueId>& venues) {
    VenueBookArray venue_books;
    for (VenueId venue : venues) {
        venue_books[static_cast<size_t>(venue)] = std::make_unique<VenueBook>(venue, instrument);
    }
    venue_books_[instrument] = std::move(venue_books);

    // Quotes are value-initialized (all fields 0) - a venue's slot stays
    // "no data yet" until its first fast-BBO message arrives, which is
    // exactly what ComputeBBOFromQuotes skips on.
    venue_quotes_[instrument] = VenueQuoteArray{};
}

void Core::RemoveInstrument(InstrumentId instrument) {
    venue_books_.erase(instrument);
    venue_quotes_.erase(instrument);
}