#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "types/venue.h"

namespace market_data {

// ---------------------------------------------------------------------------
// REMINDER: why these endpoints are what they are.
//
// This block used to live in types/venue.h next to the constants themselves.
// The values moved into venues_config.json; JSON has no comments, so the
// REASONING lives here instead. Deleting it would leave six host strings that
// nobody can defend.
//
// PORTS: 443 everywhere, not the 9443 (Binance) and 8443 (OKX) their docs lead
// with. All three venues serve the same streams on standard HTTPS, and many
// networks permit only 443 outbound - office, hotel and some ISPs.
//
// KEY: a blocked port presents as a TCP connect timeout, which is
// indistinguishable in the logs from the venue being down. Verified with
// `nc -vz <host> 443` against all three.
//
// KEY: OKX on 443 rather than its documented 8443 is the one that is only
// PARTLY verified. TCP connect succeeds because ws.okx.com sits behind
// Cloudflare, but that alone does not prove OKX routes /ws/v5/public there. If
// this ever breaks it will break at the HANDSHAKE, not at connect, and
// reverting that entry to 8443 is the fix - which is now a config edit rather
// than a rebuild.
//
// REST: only Binance needs it at all. Its depth stream is differential-only,
// so the book must be seeded from a REST snapshot (DESIGN_1 §4.3). Bybit and
// OKX send an in-channel snapshot, so their `rest_host` is empty and
// NeedsRest() is false.
//
// MEASURED 2026-09-05, futures vs spot - both differences are real:
//   - Binance futures REST depth is /fapi/v1/depth on fapi.binance.com, and
//     caps at limit=1000. limit=5000 returns {"code":-1130}, while spot
//     accepts it. A futures config must not offer the 5000 tier.
//   - Binance futures depthUpdate carries `pu` (previous event's `u`), which
//     spot does not send. That is a SEQUENCING difference, not an endpoint
//     one, so it is handled in the parser/continuity layer - noted here only
//     because it is the reason spot and futures cannot share a provider
//     wholesale.
// ---------------------------------------------------------------------------

// kSpot and kFutures. Declared here rather than in types/venue.h so that this
// step touches one file; if a third market ever appears, this constant and the
// MarketType enum must change together.
inline constexpr size_t kMarketTypeCount = 2;

// Replaced with the venue-formatted symbol when a path is resolved. Binance
// needs "btcusdt" lowercase, OKX needs "BTC-USDT-SWAP" - the FORMATTING stays
// venue-specific code, only the substitution is shared.
inline constexpr std::string_view kSymbolPlaceholder = "{symbol}";

// One venue's network endpoints for ONE market. Spot and futures get separate
// entries even where every field is identical (OKX), because "these two happen
// to match today" is not a reason to make one derive from the other.
//
// std::string, not std::string_view: these are parsed out of a file at
// runtime, so there is no string literal for a view to point at.
struct VenueEndpoints {
    std::string ws_host;
    std::string ws_port;

    // May contain kSymbolPlaceholder. Binance connects to a per-stream URL
    // ("/ws/btcusdt@depth@100ms") while Bybit and OKX use one fixed path and
    // subscribe over the socket - the placeholder is what lets both shapes be
    // data instead of three different mechanisms in three provider classes.
    std::string depth_path;
    std::string bbo_path;

    // Empty when the venue needs no REST at all - see the REST note above.
    std::string rest_host;
    std::string rest_port;

    // Book snapshot. Binance only: its depth stream is differential and must
    // be seeded. Bybit and OKX send an in-channel snapshot.
    std::string rest_depth_path;

    // Instrument metadata. OKX futures only, and for exactly one number: a
    // swap's contract size (ctVal).
    //
    // KEY: OKX quotes SWAP sizes in CONTRACTS, not in the base currency.
    // BTC-USDT-SWAP has ctVal 0.01 BTC, so a level of "495.94" is 4.9594 BTC,
    // not 495.94. Binance futures and Bybit linear both quote plain BTC. Left
    // unconverted, OKX enters the consolidated futures book 100x oversized and
    // dominates every price level - and because the three futures venues merge
    // into ONE book per InstrumentKey, keeping spot and futures separate does
    // nothing to prevent it.
    //
    // Fetched rather than hardcoded because ctVal is a number OKX publishes
    // and may re-spec; see OKXProvider::OnReconnect.
    std::string rest_instruments_path;

    // True when this venue needs REST at all, for any purpose. The two paths
    // above are independent - Binance has a depth path and no instruments
    // path, OKX futures the reverse - so callers check the specific path they
    // are about to use, not this.
    [[nodiscard]] bool NeedsRest() const { return !rest_host.empty(); }
};

// Always this file, in the working directory - same rule and same reasoning as
// config.h::kConfigFileName. There is no flag to point it elsewhere, so there
// is no second place an operator could aim it and then wonder which endpoints
// actually ran.
inline constexpr std::string_view kVenuesConfigFileName = "venues_config.json";

struct VenuesConfigLoadResult;

// Endpoints for every (venue, market) pair the file defines.
//
// KEY: a pair may legitimately be ABSENT. Configuring binance/spot without
// binance/futures is valid - it means that market is simply not served - so
// Find() returns nullptr rather than treating the gap as an error. The caller
// decides, because only the caller knows which pairs it is about to use.
class VenuesConfig {
   public:
    // Parses a document already in memory. No file access, which is what makes
    // this testable without touching a filesystem - same split as
    // ServerConfig::ParseJson.
    static VenuesConfigLoadResult ParseJson(std::string_view json);

    // Reads `path` and calls ParseJson. A missing or unreadable file reports
    // through the same error string as a malformed one, never a crash.
    static VenuesConfigLoadResult LoadFile(const std::string& path);

    // nullptr = this (venue, market) is not configured.
    //
    // A raw pointer rather than std::optional<VenueEndpoints>: this is
    // non-owning access to an entry that outlives the call, and returning an
    // optional would copy six std::strings on every lookup.
    [[nodiscard]] const VenueEndpoints* Find(VenueId venue, MarketType market) const;

   private:
    // venue-major: [venue * kMarketTypeCount + market]. Fixed-size array rather
    // than a map - there are six slots, and a lookup that cannot allocate or
    // hash is easier to reason about than one that can.
    static constexpr size_t IndexOf(VenueId venue, MarketType market) {
        return static_cast<size_t>(venue) * kMarketTypeCount + static_cast<size_t>(market);
    }

    std::array<std::optional<VenueEndpoints>, kVenueCount * kMarketTypeCount> entries_{};
};

struct VenuesConfigLoadResult {
    VenuesConfig config;
    std::string error;  // empty means success

    [[nodiscard]] bool Ok() const { return error.empty(); }
};

// Substitutes kSymbolPlaceholder in `path_template` with `symbol`.
//
// `symbol` must already be in the venue's own format - this function does not
// know that Binance wants lowercase or that OKX wants hyphens and a -SWAP
// suffix. A template with no placeholder (Bybit, OKX) is returned unchanged,
// which is why all three venues can share one call site.
std::string ResolvePath(std::string_view path_template, std::string_view symbol);

}  // namespace market_data
