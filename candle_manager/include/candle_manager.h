#pragma once

#include <optional>
#include <cstdint>
#include <unordered_map>
#include "../types/trade.h"
#include "../types/candle.h"
#include "../types/exchange.h"
#include "binance_spot_parser.h"
#include "bybit_spot_parser.h"
#include "utils/utilities.h"
#include <vector>

template <class Parser>
class CandleManager {
   public:
    explicit CandleManager(Parser parser, const std::vector<std::string>& instruments, uint32_t interval_ms);
    ~CandleManager() {}

    std::vector<Candle> GetCandlesSnapshot() const;

    void HandleMessage(std::string&& message);
    void HandleTrade(const Trade& trade);

   private:
    std::optional<std::vector<Trade>> ParseTrade(std::string&& message);
    std::optional<std::vector<Trade>> ParseTrade(std::string_view message);
    void UpdateCandle(Candle& candle, const Trade& trade);
    Candle CreateCandle(const Trade& trade);
    // std::optional<Candle> FinalizeCandle();

    const std::vector<std::string> instruments;
    const uint64_t interval_ms;

    Parser ParserHandler;

    // Current candle state
    bool has_current_candle;
    std::unordered_map<std::string, Candle> candles;  // instrument -> Candle
};
