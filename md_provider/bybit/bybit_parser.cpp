#include "bybit_parser.h"
#include "decimal.h"
#include <simdjson.h>

using namespace simdjson;

namespace market_data {

namespace {

void AppendLevels(ondemand::array levels, std::vector<PriceLevel>& out) {
    for (auto level : levels) {
        auto pair = level.get_array();
        auto it = pair.begin();
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

std::optional<BookUpdate> ParseBybitOrderbookMessage(const std::string& message, VenueId venue,
                                                      InstrumentId instrument) {
    // Contract: never throws. simdjson_error (malformed JSON, unexpected
    // shape) becomes std::nullopt, same as "not an orderbook message" -
    // callers must not need a try/catch of their own.
    try {
        ondemand::parser parser;
        padded_string json(message);
        ondemand::document doc = parser.iterate(json);

        auto topic_result = doc["topic"].get_string();
        if (topic_result.error()) {
            return std::nullopt;  // not an orderbook message (e.g. subscribe ack, pong)
        }

        auto type_result = doc["type"].get_string();
        bool is_snapshot = !type_result.error() && type_result.value() == "snapshot";

        // ts is read before data - it comes first in the real message.
        auto ts_result = doc["ts"].get_int64();
        int64_t exch_ts_ns = ts_result.error() ? 0 : ts_result.value() * 1'000'000;

        auto data = doc["data"].get_object();
        if (data.error()) {
            return std::nullopt;
        }

        BookUpdate update{};
        update.venue = venue;
        update.instrument = instrument;
        update.is_snapshot = is_snapshot;
        update.exch_ts_ns = exch_ts_ns;

        auto seq_result = data["u"].get_uint64();
        update.seq = seq_result.error() ? 0 : seq_result.value();

        auto bids_result = data["b"].get_array();
        if (!bids_result.error()) {
            AppendLevels(bids_result.value(), update.bids);
        }
        auto asks_result = data["a"].get_array();
        if (!asks_result.error()) {
            AppendLevels(asks_result.value(), update.asks);
        }

        return update;

    } catch (const simdjson_error&) {
        return std::nullopt;
    }
}

}  // namespace market_data
