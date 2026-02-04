// test_binance_trade_parser.cpp
#include <gtest/gtest.h>
#include <algorithm>
#include <numeric>

#include "binance_spot_parser.h"  // <-- поправь под свой путь/имя
#include "candle_manager.h"

#include <vector>
#include <string>
#include <cstdint>

#define MAX_PRICE 105.0
#define MIN_PRICE 96.0
#define OPEN_PRICE 100.1
#define CLOSE_PRICE 100.0

std::vector<Trade> MakeTrades20() {
    std::vector<Trade> trades;
    trades.reserve(20);

    // 20 цен (сделал так, чтобы high/low были не на концах)
    const double prices[20] = {OPEN_PRICE, 101.0, 99.5,  102.0, 100.5, 98.0,      98.5,  103.0,     101.5, 97.0,
                               97.5,       104.0, 103.5, 99.0,  100.0, MAX_PRICE, 104.5, MIN_PRICE, 96.5,  CLOSE_PRICE};

    for (int i = 0; i < 20; ++i) {
        Trade t{};
        t.exchange = Exchange::BINANCE;
        t.instrument = "BTCUSDT";
        t.trade_id = 1000 + i;
        t.price = prices[i];
        t.qty = 1.0;  // чтобы volume = 20, а notional = сумма цен
        t.event_ts = 1'770'000'000'000ULL + i;
        t.trade_ts = 1'770'000'000'000ULL + i;
        t.recv_ts = 1'770'000'000'500ULL + i;
        trades.push_back(std::move(t));
    }
    return trades;
}

TEST(CandleManager, UpdateCandleAggregatesCorrectly) {
    BinanceSpotParser parser;
    CandleManager<BinanceSpotParser> candleManager(std::move(parser), {"BTCUSDT"}, 5000);

    auto trades = MakeTrades20();
    ASSERT_EQ(trades.size(), 20u);

    for (const auto& t : trades) {
        candleManager.HandleTrade(t);
    }

    auto candles = candleManager.GetCandlesSnapshot();
    ASSERT_EQ(candles.size(), 1u);

    const auto& finalCandle = candles.front();

    const double expected_base_volume =
        std::accumulate(trades.begin(), trades.end(), 0.0, [](double s, const Trade& t) { return s + t.qty; });

    const double expected_quote_volume =
        std::accumulate(trades.begin(), trades.end(), 0.0, [](double s, const Trade& t) { return s + t.price * t.qty; });

    EXPECT_DOUBLE_EQ(finalCandle.open, OPEN_PRICE);
    EXPECT_DOUBLE_EQ(finalCandle.close, CLOSE_PRICE);
    EXPECT_DOUBLE_EQ(finalCandle.low, MIN_PRICE);
    EXPECT_DOUBLE_EQ(finalCandle.high, MAX_PRICE);

    EXPECT_DOUBLE_EQ(finalCandle.base_volume, expected_base_volume);
    EXPECT_DOUBLE_EQ(finalCandle.quote_volume, expected_quote_volume);
}
