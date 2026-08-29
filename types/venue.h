#pragma once
#include <array>
#include <string>
#include <string_view>

enum class InstrumentId : int16_t {
    UNKNOWN = -1,
    BTCUSDT = 0,
    ETHUSDT = 1,
    SOLUSDT = 2,
};

enum class VenueId : uint16_t {
    BINANCE = 0,
    BYBIT = 1,
    OKX = 2,
    COUNT = 3,
};

constexpr size_t kVenueCount = static_cast<size_t>(VenueId::COUNT);

inline constexpr std::array<VenueId, kVenueCount> VenueIdArray{
    VenueId::BINANCE,
    VenueId::BYBIT,
    VenueId::OKX,
};

inline constexpr std::string_view kBinanceHost = "stream.binance.com";
inline constexpr std::string_view kBinancePort = "9443";
// Binance's REST API is a different host/port from its WS stream. Only
// Binance needs REST at all - its depth stream is differential-only, so the
// book must be seeded from GET /api/v3/depth (DESIGN_1 §4.3). Bybit and OKX
// send an in-channel snapshot instead.
inline constexpr std::string_view kBinanceRestHost = "api.binance.com";
inline constexpr std::string_view kBinanceRestPort = "443";

inline constexpr std::string_view kBybitHost = "stream.bybit.com";
inline constexpr std::string_view kBybitPort = "443";
static constexpr std::string_view kByBitPath = "/v5/public/spot";

inline constexpr std::string_view kOkxHost = "ws.okx.com";
inline constexpr std::string_view kOkxPort = "8443";
static constexpr std::string_view kOkxPath = "/ws/v5/public";

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
