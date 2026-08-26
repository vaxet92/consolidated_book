#include "binance_provider.h"
#include "binance_parser.h"
#include "types/venue.h"
#include <fmt/format.h>
#include <algorithm>
#include <cctype>

using namespace market_data;

namespace {

std::string ToLowerSymbol(InstrumentId instrument) {
    std::string symbol = VenueConverter::ToInstrumentString(instrument);
    std::transform(symbol.begin(), symbol.end(), symbol.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return symbol;
}

}  // namespace

BinanceProvider::BinanceProvider(const ProviderConfig& config, CallBack callback)
    : Provider(config, std::move(callback)) {
    std::string symbol = ToLowerSymbol(config.instrument);
    depth_path_ = fmt::format("/ws/{}@depth@100ms", symbol);
    bbo_path_ = fmt::format("/ws/{}@bookTicker", symbol);
}

void BinanceProvider::OnDepthMessage(const std::string& message) {
    auto update = ParseBinanceDepthMessage(message, config.venue_id, config.instrument);
    if (!update) {
        return;  // not a depth update (e.g. a control/ack message)
    }
    update->recv_ts_ns = GetCurrentTimeMs() * 1'000'000;
    // TODO: gap detection/resync (DESIGN_1 §4.2) not implemented yet -
    // every message is applied as a plain delta, snapshot sync (REST
    // /api/v3/depth) is not done.
    Emit(*update);
}

void BinanceProvider::OnBboMessage(const std::string& message) {
    // TODO: fast-BBO correctness oracle (DESIGN_1 §4.4) not implemented yet -
    // this parses, but nothing compares it against the depth-derived BBO or
    // triggers a resync.
    ParseBinanceBboMessage(message);
}
