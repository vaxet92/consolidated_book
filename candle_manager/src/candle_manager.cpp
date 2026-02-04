#include "candle_manager.h"
#include <vector>

template <class Parser>
CandleManager<Parser>::CandleManager(Parser parser, const std::vector<std::string>& instruments, uint32_t interval_ms)
    : ParserHandler{std::move(parser)}, instruments(instruments), interval_ms(interval_ms), has_current_candle(false) {}

template <class Parser>
void CandleManager<Parser>::HandleMessage(std::string&& message) {
    auto trades = ParseTrade(std::move(message));
    if (trades.has_value()) {
        for (const auto& trade : trades.value()) {
            HandleTrade(trade);
        }
    }
}

template <class Parser>
std::optional<std::vector<Trade>> CandleManager<Parser>::ParseTrade(std::string&& message) {
    return ParserHandler(std::move(message));
}

template <class Parser>
std::optional<std::vector<Trade>> CandleManager<Parser>::ParseTrade(std::string_view message) {
    return ParserHandler(std::move(message));
}

template <class Parser>
void CandleManager<Parser>::HandleTrade(const Trade& trade) {
    auto is_exist = candles.find(trade.instrument);
    if (is_exist != candles.end()) {
        UpdateCandle(is_exist->second, trade);
    } else {
        candles.insert(std::make_pair(trade.instrument, CreateCandle(trade)));
    }
}

template <class Parser>
void CandleManager<Parser>::UpdateCandle(Candle& candle, const Trade& trade) {
    if (candle.close != trade.price) {
        candle.high = std::max(candle.high, trade.price);
        candle.low = std::min(candle.low, trade.price);
        candle.close = trade.price;
    }
    candle.base_volume += trade.qty;
    candle.quote_volume += trade.price * trade.qty;
}

template <class Parser>
Candle CandleManager<Parser>::CreateCandle(const Trade& trade) {
    return Candle{
        .instrument_id = ExchangeConverter::ToInstrumentId(trade.instrument),
        .exchange = trade.exchange,
        .interval_ms = interval_ms,
        .window_start_ms = trade.event_ts - (trade.event_ts % interval_ms),
        .window_end_ms = 0,
        .open = trade.price,
        .high = trade.price,
        .low = trade.price,
        .close = trade.price,
        .base_volume = trade.qty,
        .quote_volume = trade.price * trade.qty,
    };
};

template <class Parser>
std::vector<Candle> CandleManager<Parser>::GetCandlesSnapshot() const {
    std::vector<Candle> candles_snapshot;
    for (const auto& [instrument, candle] : candles) {
        candles_snapshot.push_back(candle);
    }
    return candles_snapshot;
}

template class CandleManager<BinanceSpotParser>;
template class CandleManager<BybitSpotParser>;
