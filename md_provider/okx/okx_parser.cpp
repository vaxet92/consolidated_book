#include "okx_parser.h"

#include <simdjson.h>

#include <vector>

#include "decimal.h"

using namespace simdjson;

namespace market_data {

// Price/qty are decimal strings ("8506.96", "256") - scale 8 like every
// other venue. `ts` is integer milliseconds - parsed at scale 0, then
// multiplied to nanoseconds.
constexpr uint64_t kOkxScale = 8;
constexpr uint64_t kOkxTsScale = 0;

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
        level_out.price = ParseScaledDecimal<kOkxScale>(price_sv);
        level_out.qty = ParseScaledDecimal<kOkxScale>(qty_sv);
        out.push_back(level_out);
    }
}

}  // namespace

OkxParser::OkxParser(uint32_t venue_depth) : Parser(venue_depth) {}

std::optional<BookUpdate> OkxParser::ParseBooksMessage(std::string_view message, VenueId venue, InstrumentId instrument) {
    try {
        ondemand::document doc = parser_.iterate(Load(message));

        // Hard gate, deliberately: returning on the first error means a
        // malformed document is never touched again. simdjson's on-demand
        // iterate() is lazy, so THIS is where bad JSON is actually detected -
        // continuing past it leaves the iterator at a broken depth and the
        // next lookup asserts. bbo-tbt (no `action`) has its own method.
        auto action_result = doc["action"].get_string();
        if (action_result.error()) {
            return std::nullopt;  // not a books update (e.g. subscribe ack, pong, malformed)
        }
        bool is_snapshot = action_result.value() == "snapshot";

        auto data_array = doc["data"].get_array();
        if (data_array.error()) {
            return std::nullopt;
        }

        // Exactly one entry expected per instId subscribed.
        for (auto entry : data_array.value()) {
            BookUpdate update{venue, instrument, reserve_levels_, is_snapshot};

            // Read in real document order: asks, bids, ts, checksum
            // (skipped), prevSeqId, seqId. prevSeqId MUST be read before
            // seqId - simdjson on-demand is forward-only.
            auto asks_result = entry["asks"].get_array();
            if (!asks_result.error()) {
                AppendLevels(asks_result.value(), update.asks);
            }
            auto bids_result = entry["bids"].get_array();
            if (!bids_result.error()) {
                AppendLevels(bids_result.value(), update.bids);
            }

            auto ts_result = entry["ts"].get_string();  // OKX sends ts as a string
            update.exch_ts_ns =
                ts_result.error() ? 0 : ParseScaledDecimal<kOkxTsScale>(ts_result.value()) * kTsNsMultiplier;

            // -1 on a snapshot; on an update it is the seqId this message
            // follows, which is what the continuity chain is checked against.
            auto prev_seq_result = entry["prevSeqId"].get_int64();
            update.prev_seq = prev_seq_result.error() ? 0 : prev_seq_result.value();

            auto seq_result = entry["seqId"].get_int64();
            update.seq = seq_result.error() ? 0 : static_cast<uint64_t>(seq_result.value());

            return update;
        }

        return std::nullopt;  // empty data array

    } catch (const simdjson_error&) {
        return std::nullopt;
    }
}

std::optional<BookUpdate> OkxParser::ParseBboMessage(std::string_view message, VenueId venue, InstrumentId instrument) {
    try {
        ondemand::document doc = parser_.iterate(Load(message));

        // `data` is the gate here - always present on bbo-tbt, absent on
        // subscribe acks/pongs, and the first thing to fail on malformed
        // JSON. Nothing is read before it, so a failure here is returned
        // immediately and the iterator is never reused.
        auto data_array = doc["data"].get_array();
        if (data_array.error()) {
            return std::nullopt;
        }

        // Exactly one entry expected per instId subscribed.
        for (auto entry : data_array.value()) {
            BookUpdate update{venue, instrument, reserve_levels_, true};

            // Read in wire order: asks, bids, ts, seqId (no checksum here,
            // unlike the books channel).
            auto asks_result = entry["asks"].get_array();
            if (!asks_result.error()) {
                AppendLevels(asks_result.value(), update.asks);
            }
            auto bids_result = entry["bids"].get_array();
            if (!bids_result.error()) {
                AppendLevels(bids_result.value(), update.bids);
            }

            auto ts_result = entry["ts"].get_string();  // OKX sends ts as a string
            update.exch_ts_ns =
                ts_result.error() ? 0 : ParseScaledDecimal<kOkxTsScale>(ts_result.value()) * kTsNsMultiplier;

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
