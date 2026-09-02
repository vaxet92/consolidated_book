#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <iostream>
#include <sstream>
#include <algorithm>

// ---------------------------------------------------------------------------
// REMINDER: venue depth tiers (verified against the live APIs)
//
// Depth is NOT a free parameter - each venue publishes only at fixed tiers,
// and they are chosen in different places per venue. Asking for 300 levels
// means taking the smallest tier that covers it, which differs per venue.
//
//   Binance  REST /api/v3/depth?limit=   5, 10, 20, 50, 100, 500, 1000, 5000
//   Binance  WS {symbol}@depth@{updateSpeed}  500
//   Bybit    WS topic orderbook.N        1, 50, 200, 1000
//   OKX      WS channel                  1 (bbo-tbt), 5 (books5), 400 (books)
//
// KEY: OKX effectively has ONE usable tier. There is nothing between 5 and
// 400, and nothing above 400 without VIP4 (books-l2-tbt / books50-l2-tbt
// return error 64003 otherwise). So depth=800 gives Binance 1000, Bybit 1000,
// and OKX still 400 - a venue cannot always honour the request, and that is
// worth logging rather than silently under-delivering.
//
// Currently hardcoded in the providers, NOT driven by this config:
//   Binance limit=1000, Bybit orderbook.50, OKX books (400).
// ---------------------------------------------------------------------------
inline constexpr std::array<uint32_t, 8> kBinanceDepthTiers = {5, 10, 20};
inline constexpr std::array<uint32_t, 4> kBybitDepthTiers = {1, 50, 200, 1000};
inline constexpr std::array<uint32_t, 3> kOkxDepthTiers = {1, 5, 400};

// Smallest tier that covers `desired`, or the deepest tier when none does.
// Rounds UP so the requested depth is a floor, never silently shallower than
// asked - except when the venue simply cannot go that deep, which the caller
// should log rather than hide.
template <size_t N>
constexpr uint32_t SelectDepthTier(const std::array<uint32_t, N>& tiers, uint32_t desired) {
    for (uint32_t tier : tiers) {
        if (tier >= desired) {
            return tier;
        }
    }
    return tiers.back();  // deepest available - request exceeds what this venue publishes
}

// ---------------------------------------------------------------------------
// REMINDER: venue connection limits are NOT verified.
//
// Redundant connections multiply socket count: connections x venues x 2
// streams (depth + fast-BBO). At the default that is 1 x 3 x 2 = 6 sockets;
// at --connections=3 it is 18, all from one IP.
//
// Every venue caps connections per IP, and OKX additionally rate-limits
// connection ATTEMPTS - which the reconnect loop hits, not just startup.
// Those limits have not been checked against the documentation.
//
// KEY: exceeding a venue's limit does not degrade gracefully. The venue
// refuses or bans the IP, which takes down every connection to it at once -
// the exact failure the redundancy was added to prevent.
// ---------------------------------------------------------------------------

// Default 1: redundancy is OPT-IN. The default configuration behaves exactly
// as it did before this feature existed - one connection per stream, no dedup
// work, no risk of hitting an unverified venue limit. An operator who wants
// failover asks for it explicitly with --connections=N.
inline constexpr uint32_t kDefaultConnections = 1;

// Upper bound on --connections. This is OUR arbitrary safety limit, not a
// number from any venue's documentation. It exists so a typo like
// --connections=300 cannot open 1800 sockets and get the IP banned.
inline constexpr uint32_t kMaxConnections = 8;

struct ServerConfig {
    std::vector<std::string> venues;       // binance, bybit, okx
    std::vector<std::string> instruments;  // BTCUSDT, ETHUSDT, SOLUSDT
    int grpc_port = 50051;

    // Desired per-venue book depth. Each venue rounds UP to its nearest tier
    // above, falling back to its deepest tier when the request exceeds what
    // it publishes (see the reminder above).
    uint32_t depth = 500;

    // Redundant WebSocket connections per stream, per venue - "line
    // arbitration". Every connection carries the same messages; the first
    // copy to arrive wins and the rest are dropped by the dedup filter.
    //
    // The benefit is FAILOVER: one socket dying leaves connections-1 still
    // delivering, so there is no gap, no resync and no REST refetch.
    //
    // A latency benefit is NOT claimed. Real line arbitration wins because
    // the feeds travel independent physical paths; these share one NIC and
    // one route, so any gain is jitter, and it is not measured.
    //
    // Cost is linear: N sockets, N x bandwidth, N x TLS decrypt per stream.
    // Duplicates are dropped before parsing where possible, so the parse
    // cost does not scale with N.
    uint32_t connections = kDefaultConnections;

    // Parse from command line arguments
    static ServerConfig ParseFromArgs(int argc, char* argv[]);

    // Validate configuration
    bool Validate() const;
};
