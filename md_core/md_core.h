
#pragma once
#include "types/venue.h"
#include "venue_book.h"
#include "types.h"
#include "logger/logger.h"

namespace market_data {

using VenueBookArray = std::array<std::unique_ptr<VenueBook>, static_cast<size_t>(VenueId::COUNT)>;
using InstrumentBooks = std::unordered_map<InstrumentId, VenueBookArray>;

class Core {
   public:
    Core() = default;
    ~Core() = default;

    void init(const CoreConfig& config) {
        for (InstrumentId instrument : config.default_instruments) {
            AddInstrument(instrument, config.venues);
        }
    }

    // Fed by whoever owns the Provider(s) (the wiring layer, e.g. main.cpp).
    // Core has no knowledge of providers, sockets, or threads.
    void ApplyUpdate(const BookUpdate& update) {
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
    }

    void Start() {}
    void Stop() {}

   private:
    // Creates the 3-slot VenueBookArray for `instrument`, one VenueBook per
    // venue in `venues`. Called from init() for each startup instrument; not
    // exposed as a live "subscribe" API yet (out of scope for now, see
    // DESIGN_1 §1.2 - multi-symbol is designed for, not exercised).
    void AddInstrument(InstrumentId instrument, const std::vector<VenueId>& venues);

    void RemoveInstrument(InstrumentId instrument);

    InstrumentBooks venue_books_;
};

}  // namespace market_data
