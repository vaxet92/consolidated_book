
#include "venue_book.h"

VenueBook::VenueBook(VenueId venue, InstrumentId instrument) : venue_(venue), instrument_(instrument) {}

std::optional<std::pair<PriceTicks, QtyUnits>> VenueBook::BestBid() const {
    return bids_.empty() ? std::nullopt
                         : std::make_optional(std::make_pair(bids_.begin()->first, bids_.begin()->second));
}
std::optional<std::pair<PriceTicks, QtyUnits>> VenueBook::BestAsk() const {
    return asks_.empty() ? std::nullopt
                         : std::make_optional(std::make_pair(asks_.begin()->first, asks_.begin()->second));
}

VenueId VenueBook::venue() const {
    return venue_;
}
InstrumentId VenueBook::instrument() const {
    return instrument_;
}
uint64_t VenueBook::last_seq() const {
    return last_seq_;
}

template <typename Compare>
void VenueBook::ApplySide(OrderBookType<Compare>& side, const std::vector<PriceLevel>& levels) {
    for (const auto& level : levels) {
        if (level.qty == 0) {
            side.erase(level.price);
        } else {
            side[level.price] = level.qty;
        }
    }
}

void VenueBook::ApplyUpdate(const BookUpdate& update) {
    if (update.is_snapshot) {
        bids_.clear();
        asks_.clear();
    }
    ApplySide(bids_, update.bids);
    ApplySide(asks_, update.asks);
    last_seq_ = update.seq;
}
