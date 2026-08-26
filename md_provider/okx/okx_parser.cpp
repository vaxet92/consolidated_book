#include "okx_parser.h"
#include "decimal.h"
#include <simdjson.h>

using namespace simdjson;

namespace market_data {

namespace {

// OKX levels are 4-element arrays [price, qty, deprecated, numOrders] -
// only the first two matter here.
void AppendLevels(ondemand::array levels, std::vector<PriceLevel>& out) {
    for (auto level : levels) {
        auto tuple = level.get_array();
        auto it = tuple.begin();
        std::string_view price_sv = (*it).get_string();
        ++it;
        std::string_view qty_sv = (*it).get_string();

        PriceLevel level_out;
        level_out.price = ParseScaledDecimal(price_sv);
        level_out.qty = ParseScaledDecimal(qty_sv);
        out.push_back(level_out);
    }
}

}  // namespace

std::optional<BookUpdate> ParseOkxBooksMessage(const std::string& message, VenueId venue, InstrumentId instrument) {
    try {
        ondemand::parser parser;
        padded_string json(message);
        ondemand::document doc = parser.iterate(json);

        auto action_result = doc["action"].get_string();
        if (action_result.error()) {
            return std::nullopt;  // not a books update (e.g. subscribe ack, pong)
        }
        bool is_snapshot = action_result.value() == "snapshot";

        auto data_array = doc["data"].get_array();
        if (data_array.error()) {
            return std::nullopt;
        }

        // Exactly one entry expected per instId subscribed.
        for (auto entry : data_array.value()) {
            BookUpdate update{};
            update.venue = venue;
            update.instrument = instrument;
            update.is_snapshot = is_snapshot;

            // Read in real document order: asks, bids, ts, checksum
            // (skipped), seqId.
            auto asks_result = entry["asks"].get_array();
            if (!asks_result.error()) {
                AppendLevels(asks_result.value(), update.asks);
            }
            auto bids_result = entry["bids"].get_array();
            if (!bids_result.error()) {
                AppendLevels(bids_result.value(), update.bids);
            }

            auto ts_result = entry["ts"].get_string();  // OKX sends ts as a string
            update.exch_ts_ns = ts_result.error() ? 0 : ParseScaledDecimal(ts_result.value(), 0) * 1'000'000;

            auto seq_result = entry["seqId"].get_int64();
            update.seq = seq_result.error() ? 0 : static_cast<uint64_t>(seq_result.value());

            return update;
        }

        return std::nullopt;  // empty data array

    } catch (const simdjson_error&) {
        return std::nullopt;
    }
}

}  // namespace market_data
