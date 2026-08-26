
#pragma once
#include "types/venue.h"
#include "venue_book.h"
#include "types.h"
#include "logger/logger.h"

using VenueBookArray = std::array<std::unique_ptr<VenueBook>, static_cast<size_t>(VenueId::COUNT)>;
using InstrumentBooks = std::unordered_map<InstrumentId, VenueBookArray>;

class MDCore {
   public:
    MDCore() = default;
    ~MDCore() = default;

    void init(const MDCoreConfig& config) {
        for (InstrumentId instrument : config.default_instruments) {
            AddInstrument(instrument, config.venues);
        }
    }

    // Fed by whoever owns the MDProvider(s) (the wiring layer, e.g. main.cpp).
    // MDCore has no knowledge of providers, sockets, or threads.
    void ApplyUpdate(const BookUpdate& update) {
        auto venue_it = venue_books_.find(update.instrument);
        if (venue_it == venue_books_.end()) {
            Logger::Log(LogLevel::kWarning, "Received update for unknown instrument: {}",
                        VenueConverter::ToInstrumentString(update.instrument));
            return;
        }

        auto& book = venue_it->second[static_cast<size_t>(update.venue)];
        if (!book) {
            Logger::Log(LogLevel::kWarning, "Received update for unconfigured venue: {}",
                        VenueConverter::ToVenueString(update.venue));
            return;
        }

        book->ApplyUpdate(update);
    }

    void Start() {}
    void Stop() {}

   private:
    void Run();

    // Creates the 3-slot VenueBookArray for `instrument`, one VenueBook per
    // venue in `venues`. Called from init() for each startup instrument; not
    // exposed as a live "subscribe" API yet (out of scope for now, see
    // DESIGN_1 §1.2 - multi-symbol is designed for, not exercised).
    void AddInstrument(InstrumentId instrument, const std::vector<VenueId>& venues);

    void RemoveInstrument(InstrumentId instrument);

    InstrumentBooks venue_books_;
};
