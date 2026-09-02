#pragma once
#include <array>
#include <cstdint>
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

// PORTS: 443 everywhere, not the 9443 (Binance) and 8443 (OKX) their docs
// lead with. All three venues serve the same streams on standard HTTPS, and
// many networks permit only 443 outbound - office, hotel and some ISPs.
//
// KEY: a blocked port presents as a TCP connect timeout, which is
// indistinguishable in the logs from the venue being down. Verified here with
// `nc -vz <host> 443` against all three.
inline constexpr std::string_view kBinanceHost = "stream.binance.com";
inline constexpr std::string_view kBinancePort = "443";
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
// 443 rather than the documented 8443. TCP connect succeeds (ws.okx.com is
// behind Cloudflare), but that alone does not prove OKX routes /ws/v5/public
// there - if this ever fails it will fail at the HANDSHAKE, not at connect,
// and reverting to 8443 is the fix.
inline constexpr std::string_view kOkxPort = "443";
static constexpr std::string_view kOkxPath = "/ws/v5/public";

// Staleness backstops, per venue AND per stream (DESIGN_1 §6.2c).
//
// KEY: two of these are DERIVED from behaviour the venue documents about
// itself; the rest are placeholders and are labelled as such. "Bybit L1 must
// speak every 3 seconds because Bybit says it does" is defensible in a
// debrief. "30 seconds felt about right" is not - so where a number is a
// guess, it says so rather than pretending otherwise.
//
// KEY: too SHORT is the dangerous direction. A backstop below a venue's real
// quiet interval marks a healthy feed stale and flaps it in and out of the
// merge, which is worse than having no watchdog at all. Every placeholder
// below is therefore generous. These are BACKSTOPS, not detectors - fast
// detection is meant to come from connection state and, once built,
// cross-venue corroboration (§6.2b signal 3).
namespace staleness {

inline constexpr int64_t kSecondNs = 1'000'000'000;

// DERIVED: Bybit republishes L1 (orderbook.1, our BBO stream) with the same
// `u` after 3s of no book change. Silence past ~3x that is the venue breaking
// a promise it makes about itself, not a quiet market.
inline constexpr int64_t kBybitBboBackstopNs = 10 * kSecondNs;

// PLACEHOLDER: the 3s republish is documented for L1 only. orderbook.50 has
// no documented keepalive, so this is a guess pending measurement.
inline constexpr int64_t kBybitDepthBackstopNs = 30 * kSecondNs;

// DERIVED: OKX sends seqId == prevSeqId with empty sides after roughly 60s of
// no change on the `books` channel. 90s leaves margin for a missed keepalive.
//
// Far too slow to be a detector on its own - a dead OKX depth feed would go
// unnoticed for up to 90s on this signal alone. Connection state catches the
// common case immediately; cross-venue comparison is what will close the rest.
inline constexpr int64_t kOkxDepthBackstopNs = 90 * kSecondNs;

// PLACEHOLDER: whether bbo-tbt has its own keepalive is unverified.
inline constexpr int64_t kOkxBboBackstopNs = 30 * kSecondNs;

// PLACEHOLDER, and the weakest case. Binance publishes NO keepalive on either
// stream - it sends nothing when nothing changes, so silence carries no
// information at all and a quiet market is indistinguishable from a dead feed.
// These numbers cannot be derived from anything, only measured. Until
// cross-venue corroboration exists, connection state is the only fast signal
// Binance gives us.
inline constexpr int64_t kBinanceDepthBackstopNs = 30 * kSecondNs;
inline constexpr int64_t kBinanceBboBackstopNs = 30 * kSecondNs;

}  // namespace staleness

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
