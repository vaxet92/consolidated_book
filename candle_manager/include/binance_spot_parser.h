#pragma once
#include "types/trade.h"

#include <simdjson.h>
#include <optional>
#include <string_view>
#include "utils/utilities.h"

/* "stream": "btcusdt@trade",
"data": {
  "e": "trade",
  "E": 1770003054104,
  "s": "BTCUSDT",
  "t": 5858430819,
  "p": "75019.99000000",
  "q": "0.03713000",
  "T": 1770003054103,
  "m": false,
  "M": true
}
 */

struct BinanceSpotParser {
    simdjson::ondemand::parser parser;
    std::string scratch;  // reusable

    std::optional<std::vector<Trade>> operator()(std::string_view msg) noexcept {
        // Ensure padding
        std::vector<Trade> trades;

        scratch.assign(msg.data(), msg.size());
        scratch.resize(msg.size() + simdjson::SIMDJSON_PADDING, '\0');

        simdjson::padded_string_view padded(scratch.data(), msg.size(), scratch.capacity());

        auto doc = parser.iterate(padded);
        if (doc.error()) return std::nullopt;

        // root["data"] as object
        auto root_res = doc.get_object();
        if (root_res.error()) return std::nullopt;
        auto root = root_res.value();

        auto data_res = root["data"].get_object();
        if (data_res.error()) return std::nullopt;
        auto obj = data_res.value();

        Trade t;
        t.exchange = Exchange::BINANCE;
        t.recv_ts = Utilities::GetCurrentTimeMs();

        // integers

        t.event_ts = obj["E"].get_uint64().value();
        // symbol
        auto symbol = obj["s"].get_string().value();
        t.instrument.assign(symbol.data(), symbol.size());

        t.trade_id = obj["t"].get_uint64().value();
        t.trade_ts = obj["T"].get_uint64().value();

        // price / qty strings
        auto p = obj["p"].get_string().value();
        auto q = obj["q"].get_string().value();

        t.price = Utilities::ParseDouble({p.data(), p.size()});
        t.qty = Utilities::ParseDouble({q.data(), q.size()});

        trades.push_back(std::move(t));
        return trades;
    }
};
