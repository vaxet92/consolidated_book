#pragma once
#include <string>

enum class InstrumentId : int8_t {
    UNKNOWN = -1,
    BTCUSDT = 0,
    ETHUSDT = 1,
    SOLUSDT = 2,
};

enum class Exchange : int8_t {
    UNKNOWN = -1,
    BINANCE = 0,
    BYBIT = 1,
    OKX = 2,
};

class ExchangeConverter {
   public:
    ExchangeConverter() = delete;

    static inline std::string ToExchangeString(Exchange exchange) {
        switch (exchange) {
            case Exchange::BINANCE:
                return "BINANCE";
            case Exchange::BYBIT:
                return "BYBIT";
            case Exchange::OKX:
                return "OKX";
            default:
                return "UNKNOWN";
        }
    }

    static inline Exchange ToExchange(const std::string& exchange) {
        if (exchange == "BINANCE") {
            return Exchange::BINANCE;
        } else if (exchange == "BYBIT") {
            return Exchange::BYBIT;
        } else if (exchange == "OKX") {
            return Exchange::OKX;
        }
        return Exchange::UNKNOWN;
    }

    static inline std::string ToInstrumentString(InstrumentId instrument_id) {
        switch (instrument_id) {
            case InstrumentId::BTCUSDT:
                return "BTCUSDT";
            case InstrumentId::ETHUSDT:
                return "ETHUSDT";
            case InstrumentId::SOLUSDT:
                return "SOLUSDT";
            default:
                return "UNKNOWN";
        }
    }

    static inline InstrumentId ToInstrumentId(const std::string& instrument) {
        if (instrument == "BTCUSDT") {
            return InstrumentId::BTCUSDT;
        } else if (instrument == "ETHUSDT") {
            return InstrumentId::ETHUSDT;
        } else if (instrument == "SOLUSDT") {
            return InstrumentId::SOLUSDT;
        }
        return InstrumentId::UNKNOWN;
    }
};
