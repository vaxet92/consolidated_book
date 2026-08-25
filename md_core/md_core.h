
#pragma once
#include "types/venue.h"
#include "venue_book.h"
#include "types.h"

using VenueBookArray = std::array<std::unique_ptr<VenueBook>, static_cast<size_t>(VenueId::COUNT)>;
using InstrumentBooks = std::unordered_map<InstrumentId, VenueBookArray>;

class MDCore {
   public:
    MDCore() = default;
    ~MDCore() {}

    void init(const MDCoreConfig& config) {
        // init Venues
    }

    // Fed by whoever owns the MDProvider(s) (the wiring layer, e.g. main.cpp).
    // MDCore has no knowledge of providers, sockets, or threads.
    void ApplyUpdate(const BookUpdate& update) {
        auto venue_it = venue_books_.find(update.instrument);
        if (venue_it != venue_books_.end()) {
            // Process the update for the found venue
        } else {
        }
    }

    void Start() {}
    void Stop() {}

   private:
    void Run();

    InstrumentBooks venue_books_;
};