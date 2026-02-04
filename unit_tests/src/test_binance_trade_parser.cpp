// test_binance_trade_parser.cpp
#include <gtest/gtest.h>

#include <cstdlib>
#include <string>
#include "candle_manager.h"
#include "types/trade.h"

// Test Binance Spot Parser
TEST(BinanceSpotParser, ParsesSingleTrade) {
    const std::string msg = R"JSON(
        {
            "stream": "btcusdt@trade",
            "data": {
                "e": "trade",
                "E": 1770003054104,
                "s": "BTCUSDT",
                "t": 5858430818,
                "p": "75019.99000000",
                "q": "0.03100000",
                "T": 1770003054103,
                "m": false,
                "M": true
            }
        }
        )JSON";

    BinanceSpotParser parser;
    auto tradesOpt = parser(msg);
    ASSERT_TRUE(tradesOpt.has_value());

    ASSERT_EQ(tradesOpt.value().size(), 1);
    const auto& trade = tradesOpt.value().front();

    EXPECT_EQ(trade.exchange, Exchange::BINANCE);
    EXPECT_EQ(trade.instrument, "BTCUSDT");
    EXPECT_EQ(trade.trade_id, 5858430818ULL);
    EXPECT_DOUBLE_EQ(trade.price, 75019.99);
    EXPECT_DOUBLE_EQ(trade.qty, 0.031);
    EXPECT_EQ(trade.trade_ts, 1770003054103ULL);
    EXPECT_EQ(trade.event_ts, 1770003054104ULL);
    EXPECT_TRUE(trade.recv_ts > 0ULL);
}

// Test Candle Calculti
TEST(BinanceSpotParser, ParsesManyTradesFromArray) {
    std::vector<Trade> trades;
    trades.reserve(3);
    trades.push_back(Trade{
        .exchange = Exchange::BINANCE,
        .instrument = "BTCUSDT",
        .trade_id = 5858430818ULL,
        .price = 75019.99,
        .qty = 0.03100000,
        .event_ts = 1770003054104ULL,
        .trade_ts = 1770003054103ULL,
    });
    trades.push_back(Trade{
        .exchange = Exchange::BINANCE,
        .instrument = "ETHUSDT",
        .trade_id = 5858430817ULL,
        .price = 75019.99,
        .qty = 0.03100000,
        .event_ts = 1770003054104ULL,
        .trade_ts = 1770003054103ULL,
    });
    trades.push_back(Trade{
        .exchange = Exchange::BINANCE,
        .instrument = "SOLUSDT",
        .trade_id = 5858430816ULL,
        .price = 75019.99,
        .qty = 0.03800000,
        .event_ts = 1770003054104ULL,
        .trade_ts = 1770003054103ULL,
    });
    BinanceSpotParser trade_parser;
    std::vector<std::string> instruments = {"BTCUSDT", "ETHUSDT", "SOLUSDT"};
    CandleManager<BinanceSpotParser> candle_manager(std::move(trade_parser), {"BTCUSDT", "ETHUSDT", "SOLUSDT"}, 5000);

    for (const auto& trade : trades) {
        candle_manager.HandleTrade(trade);
    }

    auto candles = candle_manager.GetCandlesSnapshot();
    EXPECT_EQ(candles.size(), 3);

    // Check that all expected instruments are present (order doesn't matter)
    bool found_btc = false, found_eth = false, found_sol = false;
    for (const auto& candle : candles) {
        EXPECT_EQ(candle.exchange, Exchange::BINANCE);
        if (candle.instrument_id == InstrumentId::BTCUSDT) found_btc = true;
        if (candle.instrument_id == InstrumentId::ETHUSDT) found_eth = true;
        if (candle.instrument_id == InstrumentId::SOLUSDT) found_sol = true;
    }
    EXPECT_TRUE(found_btc) << "BTCUSDT candle not found";
    EXPECT_TRUE(found_eth) << "ETHUSDT candle not found";
    EXPECT_TRUE(found_sol) << "SOLUSDT candle not found";

    // EXPECT_EQ(candle_manager.GetCandlesSnapshot().size(), 2);
}
