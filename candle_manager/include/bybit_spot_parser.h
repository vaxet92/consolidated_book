#pragma once
#include "types/trade.h"

#include <simdjson.h>
#include <optional>
#include <string_view>
#include "utils/utilities.h"

/* {
    "topic": "publicTrade.ETHUSDT",
    "ts": 1770197572744,
    "type": "snapshot",
    "data": [
        {
        "i": "2280000001559930510",
        "T": 1770197572743,
        "p": "2260.15",
        "v": "0.09464",
        "S": "Sell",
        "seq": 159851600390,
        "s": "ETHUSDT",
        "BT": false,
        "RPI": false
        },
        {
        "i": "2280000001559930511",
        "T": 1770197572743,
        "p": "2260.15",
        "v": "0.11",
        "S": "Sell",
        "seq": 159851600390,
        "s": "ETHUSDT",
        "BT": false,
        "RPI": false
        }
    ]
} */

static inline std::string_view ExtractSymbol(std::string_view topic) noexcept {
    constexpr std::string_view prefix = "publicTrade.";
    auto pos = topic.find(prefix);
    if (pos == std::string_view::npos) return {};
    return topic.substr(pos + prefix.size());
}

struct BybitSpotParser {
    simdjson::ondemand::parser parser;
    std::string scratch;

    std::optional<std::vector<Trade>> operator()(std::string_view msg) noexcept {
        // padding
        std::vector<Trade> trades;
        // Bybit snapshot often returns up to ~50 items; reserve a bit
        trades.reserve(128);

        scratch.assign(msg.data(), msg.size());
        scratch.resize(msg.size() + simdjson::SIMDJSON_PADDING, '\0');
        simdjson::padded_string_view padded(scratch.data(), msg.size(), scratch.capacity());

        auto doc = parser.iterate(padded);
        if (doc.error()) return std::nullopt;

        auto root_res = doc.get_object();
        if (root_res.error()) return std::nullopt;
        auto root = root_res.value();

        // top-level fields
        auto topic_res = root["topic"].get_string();
        if (topic_res.error()) return std::nullopt;
        auto symbol = ExtractSymbol(topic_res.value());
        
        auto type_res = root["type"].get_string();
        if (type_res.error()) return std::nullopt;

        auto ts_res = root["ts"].get_uint64();
        if (ts_res.error()) return std::nullopt;
        const uint64_t event_ts = ts_res.value();

        // data is ARRAY
        auto data_res = root["data"].get_array();
        if (data_res.error()) return std::nullopt;

        for (simdjson::ondemand::value item : data_res.value()) {
            auto obj_res = item.get_object();
            if (obj_res.error()) return std::nullopt;
            auto obj = obj_res.value();

            // numbers
            Trade t{};
            t.exchange = Exchange::BYBIT;
            t.event_ts = event_ts;
            t.recv_ts = Utilities::GetCurrentTimeMs();

            t.trade_ts = obj["T"].get_uint64().value();

            t.trade_id = Utilities::ParseUint64(obj["i"].get_string().value());
            t.price = Utilities::ParseDouble(obj["p"].get_string().value());
            t.qty = Utilities::ParseDouble(obj["v"].get_string().value());

            t.instrument.assign(symbol.data(), symbol.size());

            trades.push_back(std::move(t));
        }

        return trades;
    }
};
