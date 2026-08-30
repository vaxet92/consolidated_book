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
inline constexpr std::array<uint32_t, 8> kBinanceDepthTiers = {5, 10, 20, 50, 100, 500, 1000, 5000};
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

struct ServerConfig {
    std::vector<std::string> venues;       // binance, bybit, okx
    std::vector<std::string> instruments;  // BTCUSDT, ETHUSDT, SOLUSDT
    int grpc_port = 50051;

    // Desired per-venue book depth. Each venue rounds UP to its nearest tier
    // above, falling back to its deepest tier when the request exceeds what
    // it publishes (see the reminder above).
    uint32_t depth = 500;

    // Parse from command line arguments
    static ServerConfig ParseFromArgs(int argc, char* argv[]);

    // Validate configuration
    bool Validate() const;
};
