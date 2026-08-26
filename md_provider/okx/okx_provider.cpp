#include "okx_provider.h"
#include "okx_parser.h"
#include "types/venue.h"
#include <fmt/format.h>

using namespace market_data;

namespace {

// OKX uses "BTC-USDT", not "BTCUSDT" - insert a hyphen before the quote
// currency. Simple and matches every symbol this project uses (all end in
// USDT).
std::string ToOkxInstId(InstrumentId instrument) {
    std::string symbol = VenueConverter::ToInstrumentString(instrument);
    size_t usdt_pos = symbol.find("USDT");
    if (usdt_pos != std::string::npos) {
        symbol.insert(usdt_pos, "-");
    }
    return symbol;
}

}  // namespace

OKXProvider::OKXProvider(const ProviderConfig& config, CallBack callback) : Provider(config, std::move(callback)) {}

std::string OKXProvider::DepthSubscriptionMessage() const {
    return fmt::format(R"({{"op":"subscribe","args":[{{"channel":"books","instId":"{}"}}]}})",
                       ToOkxInstId(config.instrument));
}

std::string OKXProvider::BboSubscriptionMessage() const {
    return fmt::format(R"({{"op":"subscribe","args":[{{"channel":"bbo-tbt","instId":"{}"}}]}})",
                       ToOkxInstId(config.instrument));
}

void OKXProvider::OnDepthMessage(const std::string& message) {
    auto update = ParseOkxBooksMessage(message, config.venue_id, config.instrument);
    if (!update) {
        return;  // not a books update (e.g. subscribe ack, pong)
    }
    update->recv_ts_ns = GetCurrentTimeMs() * 1'000'000;
    // TODO: gap detection/resync (DESIGN_1 §4.2) not implemented yet -
    // "action":"snapshot" resets the book but continuity via seqId is not
    // checked, and the CRC32 checksum (§4.3's free end-to-end validation)
    // is not verified.
    Emit(*update);
}

void OKXProvider::OnBboMessage(const std::string& message) {
    // TODO: fast-BBO correctness oracle (DESIGN_1 §4.4) not implemented yet -
    // this parses but nothing compares it against the depth-derived BBO or
    // triggers a resync.
    ParseOkxBooksMessage(message, config.venue_id, config.instrument);
}
