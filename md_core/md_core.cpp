#include "md_core.h"

void MDCore::AddInstrument(InstrumentId instrument, const std::vector<VenueId>& venues) {
    VenueBookArray venue_books;
    for (VenueId venue : venues) {
        venue_books[static_cast<size_t>(venue)] = std::make_unique<VenueBook>(venue, instrument);
    }
    venue_books_[instrument] = std::move(venue_books);
}

void MDCore::RemoveInstrument(InstrumentId instrument) {
    venue_books_.erase(instrument);
}