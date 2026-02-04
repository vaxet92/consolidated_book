// test_binance_trade_parser.cpp
#include <gtest/gtest.h>
#include <cstdlib>
#include <string>
#include "candle_manager.h"
#include "types/trade.h"

TEST(BybitSpotParser, ParsesSnapshot_AllFields_AllItems) {
    const std::string msg = R"JSON(
        {
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
              },
              {
                "i": "2280000001559930512",
                "T": 1770197572743,
                "p": "2260.15",
                "v": "0.11",
                "S": "Sell",
                "seq": 159851600390,
                "s": "ETHUSDT",
                "BT": false,
                "RPI": false
              },
              {
                "i": "2280000001559930513",
                "T": 1770197572743,
                "p": "2260.15",
                "v": "0.11",
                "S": "Sell",
                "seq": 159851600390,
                "s": "ETHUSDT",
                "BT": false,
                "RPI": false
              },
              {
                "i": "2280000001559930514",
                "T": 1770197572743,
                "p": "2260.15",
                "v": "0.11",
                "S": "Sell",
                "seq": 159851600390,
                "s": "ETHUSDT",
                "BT": false,
                "RPI": false
              },
              {
                "i": "2280000001559930515",
                "T": 1770197572743,
                "p": "2260.15",
                "v": "0.11",
                "S": "Sell",
                "seq": 159851600390,
                "s": "ETHUSDT",
                "BT": false,
                "RPI": false
              },
              {
                "i": "2280000001559930516",
                "T": 1770197572743,
                "p": "2260.15",
                "v": "0.11",
                "S": "Sell",
                "seq": 159851600390,
                "s": "ETHUSDT",
                "BT": false,
                "RPI": false
              },
              {
                "i": "2280000001559930517",
                "T": 1770197572743,
                "p": "2260.15",
                "v": "0.11",
                "S": "Sell",
                "seq": 159851600390,
                "s": "ETHUSDT",
                "BT": false,
                "RPI": false
              },
              {
                "i": "2280000001559930518",
                "T": 1770197572743,
                "p": "2260.15",
                "v": "0.04162",
                "S": "Sell",
                "seq": 159851600390,
                "s": "ETHUSDT",
                "BT": false,
                "RPI": false
              }
            ]
          }
        )JSON";

    BybitSpotParser parser;
    auto tradesOpt = parser(msg);
    ASSERT_TRUE(tradesOpt.has_value());

    const auto& trades = tradesOpt.value();
    ASSERT_EQ(trades.size(), 9);

    // top-level expected
    const uint64_t expected_event_ts = 1770197572744ULL;
    const std::string expected_symbol = "ETHUSDT";
    const uint64_t expected_trade_ts = 1770197572743ULL;

    // per-item expected
    struct Exp {
        uint64_t id;
        double price;
        double qty;
        uint64_t trade_ts;
    };

    const Exp exp[] = {
        {2280000001559930510ULL, 2260.15, 0.09464, expected_trade_ts},
        {2280000001559930511ULL, 2260.15, 0.11, expected_trade_ts},
        {2280000001559930512ULL, 2260.15, 0.11, expected_trade_ts},
        {2280000001559930513ULL, 2260.15, 0.11, expected_trade_ts},
        {2280000001559930514ULL, 2260.15, 0.11, expected_trade_ts},
        {2280000001559930515ULL, 2260.15, 0.11, expected_trade_ts},
        {2280000001559930516ULL, 2260.15, 0.11, expected_trade_ts},
        {2280000001559930517ULL, 2260.15, 0.11, expected_trade_ts},
        {2280000001559930518ULL, 2260.15, 0.04162, expected_trade_ts},
    };

    for (size_t i = 0; i < trades.size(); ++i) {
        const auto& tr = trades[i];

        EXPECT_EQ(tr.exchange, Exchange::BYBIT);
        EXPECT_EQ(tr.event_ts, expected_event_ts);
        EXPECT_GT(tr.recv_ts, 0ULL);

        EXPECT_EQ(tr.instrument, expected_symbol);

        EXPECT_EQ(tr.trade_id, exp[i].id);
        EXPECT_DOUBLE_EQ(tr.price, exp[i].price);
        EXPECT_DOUBLE_EQ(tr.qty, exp[i].qty);
        EXPECT_EQ(tr.trade_ts, exp[i].trade_ts);
    }
}
