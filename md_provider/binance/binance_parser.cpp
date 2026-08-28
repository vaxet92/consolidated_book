#include "binance_parser.h"
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

std::optional<BookUpdate> ParseBinanceDepthMessage(const std::string& message, VenueId venue,
                                                    InstrumentId instrument) {
    try {
        ondemand::parser parser;
        padded_string json(message);
        ondemand::document doc = parser.iterate(json);

        auto event_type_result = doc["e"].get_string();
        if (event_type_result.error() || event_type_result.value() != "depthUpdate") {
            return std::nullopt;  // not a depth update (e.g. a control/ack message)
        }

        BookUpdate update{};
        update.venue = venue;
        update.instrument = instrument;
        update.is_snapshot = false;

        auto event_ts_result = doc["E"].get_int64();
        update.exch_ts_ns = event_ts_result.error() ? 0 : event_ts_result.value() * 1'000'000;

        auto final_id_result = doc["u"].get_uint64();
        update.seq = final_id_result.error() ? 0 : final_id_result.value();

        auto bids_result = doc["b"].get_array();
        if (!bids_result.error()) {
            AppendLevels(bids_result.value(), update.bids);
        }
        auto asks_result = doc["a"].get_array();
        if (!asks_result.error()) {
            AppendLevels(asks_result.value(), update.asks);
        }

        return update;

    } catch (const simdjson_error&) {
        return std::nullopt;
    }
}

std::optional<BboQuote> ParseBinanceBboMessage(const std::string& message, VenueId venue, InstrumentId instrument) {
    try {
        ondemand::parser parser;
        padded_string json(message);
        ondemand::document doc = parser.iterate(json);

        // Read in wire order (u, s, b, B, a, A) - simdjson on-demand scans
        // forward, so matching the document's own order is cheapest.
        auto update_id_result = doc["u"].get_uint64();
        if (update_id_result.error()) {
            return std::nullopt;  // not a bookTicker payload (e.g. a subscribe ack)
        }

        auto bid_price_result = doc["b"].get_string();
        auto bid_qty_result = doc["B"].get_string();
        auto ask_price_result = doc["a"].get_string();
        auto ask_qty_result = doc["A"].get_string();

        if (bid_price_result.error() || bid_qty_result.error() || ask_price_result.error() || ask_qty_result.error()) {
            return std::nullopt;
        }

        BboQuote quote;
        quote.venue = venue;
        quote.instrument = instrument;
        quote.seq = update_id_result.value();
        // bookTicker carries no exchange timestamp - exch_ts_ns stays 0.
        quote.bid_price = ParseScaledDecimal(bid_price_result.value());
        quote.bid_qty = ParseScaledDecimal(bid_qty_result.value());
        quote.ask_price = ParseScaledDecimal(ask_price_result.value());
        quote.ask_qty = ParseScaledDecimal(ask_qty_result.value());
        return quote;

    } catch (const simdjson_error&) {
        return std::nullopt;
    }
}

}  // namespace market_data
