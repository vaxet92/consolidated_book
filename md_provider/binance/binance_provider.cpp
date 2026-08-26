#include "binance_provider.h"
#include "decimal.h"
#include "logger/logger.h"
#include "types/venue.h"
#include <simdjson.h>
#include <algorithm>
#include <cctype>

using namespace simdjson;
using namespace market_data;

namespace {

std::string ToLowerSymbol(InstrumentId instrument) {
    std::string symbol = VenueConverter::ToInstrumentString(instrument);
    std::transform(symbol.begin(), symbol.end(), symbol.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return symbol;
}

void AppendLevels(ondemand::array levels, std::vector<PriceLevel>& out) {
    for (auto level : levels) {
        auto pair = level.get_array();
        auto it = pair.begin();
        std::string_view price_sv = (*it).get_string();
        ++it;
        std::string_view qty_sv = (*it).get_string();

        PriceLevel level_out;
        level_out.price = ParseScaledDecimal(price_sv);
        level_out.qty = ParseScaledDecimal(qty_sv);
        out.push_back(level_out);
    }
}

}  // namespace

BinanceProvider::BinanceProvider(const ProviderConfig& config, CallBack callback)
    : Provider(config, std::move(callback)) {
    std::string symbol = ToLowerSymbol(config.instrument);
    depth_path_ = "/ws/" + symbol + "@depth@100ms";
    bbo_path_ = "/ws/" + symbol + "@bookTicker";
}

void BinanceProvider::OnBboMessage(const std::string& message) {
    // TODO: fast-BBO correctness oracle (DESIGN_1 §4.4) still not implemented -
    // this only parses and logs the stream so it's visible end-to-end.
    // Comparing against the depth-derived BBO and triggering a resync on
    // disagreement is a separate step (needs provider access to the book).
    try {
        ondemand::parser parser;
        padded_string json(message);
        ondemand::document doc = parser.iterate(json);

        auto bid_price_result = doc["b"].get_string();
        auto bid_qty_result = doc["B"].get_string();
        auto ask_price_result = doc["a"].get_string();
        auto ask_qty_result = doc["A"].get_string();

        if (bid_price_result.error() || bid_qty_result.error() || ask_price_result.error() || ask_qty_result.error()) {
            return;  // not a bookTicker payload (e.g. a subscribe ack)
        }

        PriceTicks bid_price = ParseScaledDecimal(bid_price_result.value());
        QtyUnits bid_qty = ParseScaledDecimal(bid_qty_result.value());
        PriceTicks ask_price = ParseScaledDecimal(ask_price_result.value());
        QtyUnits ask_qty = ParseScaledDecimal(ask_qty_result.value());

        Logger::Log(LogLevel::kInfo, "[BINANCE] BBO bid={}/{} ask={}/{}", bid_price, bid_qty, ask_price, ask_qty);

    } catch (const simdjson_error& e) {
        Logger::Log(LogLevel::kError, "[BINANCE] BBO JSON parse error: {}", e.what());
    }
}

void BinanceProvider::OnDepthMessage(const std::string& message) {
    try {
        ondemand::parser parser;
        padded_string json(message);
        ondemand::document doc = parser.iterate(json);

        auto event_type_result = doc["e"].get_string();
        if (event_type_result.error() || event_type_result.value() != "depthUpdate") {
            return;  // not a depth update (e.g. a control/ack message)
        }

        BookUpdate update{};
        update.venue = config.venue_id;
        update.instrument = config.instrument;
        // TODO: gap detection/resync (DESIGN_1 §4.2) not implemented yet -
        // every message is applied as a plain delta, snapshot sync (REST
        // /api/v3/depth) is not done. A missed or out-of-order message will
        // silently desync this book until that state machine is built.
        update.is_snapshot = false;

        update.recv_ts_ns = GetCurrentTimeMs() * 1'000'000;

        auto event_ts_result = doc["E"].get_int64();
        update.exch_ts_ns = event_ts_result.error() ? 0 : event_ts_result.value() * 1'000'000;

        auto final_id_result = doc["u"].get_uint64();
        update.seq = final_id_result.error() ? 0 : final_id_result.value();

        auto bids_result = doc["b"].get_array();
        if (!bids_result.error()) {
            AppendLevels(bids_result.value(), update.bids);
        }
        auto asks_result = doc["a"].get_array();
        if (!asks_result.error()) {
            AppendLevels(asks_result.value(), update.asks);
        }

        Emit(update);

    } catch (const simdjson_error& e) {
        Logger::Log(LogLevel::kError, "[BINANCE] JSON parse error: {}", e.what());
    }
}
