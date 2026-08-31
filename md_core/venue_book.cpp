
#include "venue_book.h"
#include "logger/logger.h"

namespace market_data {

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
int64_t VenueBook::last_update_mono_ns() const {
    return last_update_mono_ns_;
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
    // Recorded here rather than in Core's dispatch: ApplyUpdate is only
    // reached by a real, sequence-validated depth message. Pings, pongs and
    // subscribe acks never get this far, so the "a heartbeat must not feed
    // the watchdog" rule holds by construction instead of by remembering.
    last_update_mono_ns_ = update.recv_mono_ns;

    if (update.is_snapshot) {
        bids_.clear();
        asks_.clear();
    }
    ApplySide(bids_, update.bids);
    ApplySide(asks_, update.asks);
    last_seq_ = update.seq;
}

void PrintHelper::Level(const char* side, const PriceLevel& level) {
    Logger::Log(LogLevel::kInfo, "  {} {} @ {}", side, level.qty, level.price);
}

void PrintHelper::BBO(const VenueBook& book) {
    Logger::Log(LogLevel::kInfo, "[{} {}] seq={}", VenueConverter::ToVenueString(book.venue()),
                VenueConverter::ToInstrumentString(book.instrument()), book.last_seq());
    if (auto bid = book.BestBid()) {
        Level("BID", PriceLevel{bid->first, bid->second});
    }
    if (auto ask = book.BestAsk()) {
        Level("ASK", PriceLevel{ask->first, ask->second});
    }
}

void PrintHelper::Book(const VenueBook& book) {
    Logger::Log(LogLevel::kInfo, "[{} {}] seq={}", VenueConverter::ToVenueString(book.venue()),
                VenueConverter::ToInstrumentString(book.instrument()), book.last_seq());
    for (const auto& [price, qty] : book.asks()) {
        Level("ASK", PriceLevel{price, qty});
    }
    for (const auto& [price, qty] : book.bids()) {
        Level("BID", PriceLevel{price, qty});
    }
}
}  // namespace market_data