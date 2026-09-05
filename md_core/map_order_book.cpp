
#include "map_order_book.h"
#include "logger/logger.h"

namespace market_data {

MapOrderBook::MapOrderBook(VenueId venue, InstrumentKey instrument) : venue_(venue), instrument_(instrument) {}

std::optional<std::pair<PriceTicks, QtyUnits>> MapOrderBook::BestBid() const {
    return bids_.empty() ? std::nullopt
                         : std::make_optional(std::make_pair(bids_.begin()->first, bids_.begin()->second));
}
std::optional<std::pair<PriceTicks, QtyUnits>> MapOrderBook::BestAsk() const {
    return asks_.empty() ? std::nullopt
                         : std::make_optional(std::make_pair(asks_.begin()->first, asks_.begin()->second));
}

VenueId MapOrderBook::venue() const {
    return venue_;
}
InstrumentKey MapOrderBook::instrument() const {
    return instrument_;
}
uint64_t MapOrderBook::last_seq() const {
    return last_seq_;
}
int64_t MapOrderBook::last_update_mono_ns() const {
    return last_update_mono_ns_;
}

// Applies one side of a delta, chaining an insertion hint from level to level.
//
// The venues send their level arrays SORTED, so consecutive levels land next
// to each other in the tree. Feeding the previous result back as the hint lets
// std::map skip the descent from the root: insert_or_assign with a correct
// hint is O(1) amortised instead of O(log n). For a 237-level Binance message
// that replaces ~237 full tree descents with ~237 pointer steps.
//
// KEY: the hint is a performance suggestion ONLY. If the input ever arrives
// unsorted the hint is simply wrong, std::map falls back to a normal search,
// and the result is still correct - just no faster than before. That is why
// this shape was chosen over walking the book with an iterator, which IS
// correctness-dependent on sorted input and silently drops updates when the
// assumption breaks.
template <typename Compare>
void MapOrderBook::ApplySide(OrderBookType<Compare>& side, const std::vector<PriceLevel>& levels) {
    auto hint = side.begin();

    for (const auto& level : levels) {
        if (level.qty == 0) {
            // KEY: erase(iterator), not erase(key). erase(key) would leave
            // `hint` dangling whenever it pointed at the erased element, and
            // passing a dangling iterator as a hint is undefined behaviour.
            // The iterator overload returns the FOLLOWING element, which is
            // both valid and already the right hint for the next price.
            //
            // In a sorted delta the chained hint usually ALREADY points at the
            // level being removed, so check it before paying for a descent.
            // std::map has no hinted find or erase(key), so this is the only
            // way to skip the search. Measured: it turns 19 of the 20 erases
            // in bench_md_core's churn case into pure pointer work.
            //
            // Same advisory property as the insert hint - a miss costs a
            // normal search and nothing else, so an unsorted delta is slower
            // but never wrong.
            if (hint != side.end() && hint->first == level.price) {
                hint = side.erase(hint);
                continue;
            }

            auto it = side.find(level.price);
            if (it != side.end()) {
                hint = side.erase(it);
            }
            continue;
        }

        auto written = side.insert_or_assign(hint, level.price, level.qty);
        hint = std::next(written);  // the position k' will occupy
    }
}

void MapOrderBook::ApplyUpdate(const BookUpdate& update) {
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

void PrintHelper::BBO(const MapOrderBook& book) {
    Logger::Log(LogLevel::kInfo, "[{} {}] seq={}", VenueConverter::ToVenueString(book.venue()),
                VenueConverter::ToInstrumentString(book.instrument()), book.last_seq());
    if (auto bid = book.BestBid()) {
        Level("BID", PriceLevel{bid->first, bid->second});
    }
    if (auto ask = book.BestAsk()) {
        Level("ASK", PriceLevel{ask->first, ask->second});
    }
}

void PrintHelper::Book(const MapOrderBook& book) {
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