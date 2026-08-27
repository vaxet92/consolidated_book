#include "md_core.h"
using namespace market_data;

void Core::Init(const CoreConfig& config) {
    for (InstrumentId instrument : config.default_instruments) {
        AddInstrument(instrument, config.venues);
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

    PrintHelper::Book(*book_ptr);

    if (bbo_callback_) {
        bbo_callback_(update.instrument, consolidated::ComputeBBO(venue_it->second));
    }
}

void Core::AddInstrument(InstrumentId instrument, const std::vector<VenueId>& venues) {
    VenueBookArray venue_books;
    for (VenueId venue : venues) {
        venue_books[static_cast<size_t>(venue)] = std::make_unique<VenueBook>(venue, instrument);
    }
    venue_books_[instrument] = std::move(venue_books);
}

void Core::RemoveInstrument(InstrumentId instrument) {
    venue_books_.erase(instrument);
}