#pragma once
#include <string>

enum class InstrumentId : int8_t {
    UNKNOWN = -1,
    BTCUSDT = 0,
    ETHUSDT = 1,
    SOLUSDT = 2,
};

enum class VenueId : size_t {
    BINANCE = 0,
    BYBIT = 1,
    OKX = 2,
    COUNT = 3,
};

class VenueConverter {
   public:
    VenueConverter() = delete;

    static inline std::string ToVenueString(VenueId venue_id) {
        switch (venue_id) {
            case VenueId::BINANCE:
                return "BINANCE";
            case VenueId::BYBIT:
                return "BYBIT";
            case VenueId::OKX:
                return "OKX";
            default:
                return "UNKNOWN";
        }
    }

    static inline VenueId ToVenueId(const std::string& venue_id) {
        if (venue_id == "BINANCE") {
            return VenueId::BINANCE;
        } else if (venue_id == "BYBIT") {
            return VenueId::BYBIT;
        } else if (venue_id == "OKX") {
            return VenueId::OKX;
        }
        return VenueId::COUNT;
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
