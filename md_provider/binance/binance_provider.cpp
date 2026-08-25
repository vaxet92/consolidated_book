#include "binance_provider.h"
#include <iostream>
#include <simdjson.h>

using namespace simdjson;

BinanceProvider::BinanceProvider(const ProviderConfig& config) : BaseProvider(config) {}

const char* BinanceProvider::GetHost() const {
    return BINANCE_HOST;
}

const char* BinanceProvider::GetPort() const {
    return BINANCE_PORT;
}

const char* BinanceProvider::GetPath() const {
    // Path will be set after subscription
    // For now, return a generic stream endpoint
    return "/ws";
}

std::string BinanceProvider::GetSubscriptionMessage() const {
    // Build subscription message for all instruments
    // Format: {"method":"SUBSCRIBE","params":["btcusdt@trade","ethusdt@trade"],"id":1}

    std::ostringstream oss;
    oss << "{\"method\":\"SUBSCRIBE\",\"params\":[";

    for (size_t i = 0; i < config.instruments.size(); ++i) {
        if (i > 0) oss << ",";

        // Convert to lowercase (Binance requirement)
        std::string instrument_lower = config.instruments[i];
        for (char& c : instrument_lower) {
            c = std::tolower(c);
        }

        oss << "\"" << instrument_lower << "@trade\"";
    }

    oss << "],\"id\":1}";

    return oss.str();
}

void BinanceProvider::OnMessage(const std::string& message) {
    ParseTrade(message);
}

void BinanceProvider::ParseTrade(const std::string& message) {
    try {
        ondemand::parser parser;
        padded_string json = padded_string(message);
        ondemand::document doc = parser.iterate(json);

        // Check if this is a trade event
        std::string_view event_type;
        auto event_result = doc["e"].get_string();
        if (event_result.error()) {
            // Not a trade message, might be subscription confirmation
            return;
        }
        event_type = event_result.value();

        if (event_type != "trade") {
            return;
        }

        // Extract trade data
        Trade trade;
        trade.exchange = "BINANCE";

        // Instrument symbol
        auto symbol_result = doc["s"].get_string();
        if (!symbol_result.error()) {
            std::string symbol(symbol_result.value());
            // Convert to uppercase
            for (char& c : symbol) {
                c = std::toupper(c);
            }
            trade.instrument = symbol;
        }

        // Trade ID
        auto trade_id_result = doc["t"].get_int64();
        if (!trade_id_result.error()) {
            trade.trade_id = std::to_string(trade_id_result.value());
        }

        // Price
        auto price_result = doc["p"].get_string();
        if (!price_result.error()) {
            trade.price = std::stod(std::string(price_result.value()));
        }

        // Quantity
        auto qty_result = doc["q"].get_string();
        if (!qty_result.error()) {
            trade.qty = std::stod(std::string(qty_result.value()));
        }

        // Exchange timestamp (T field in milliseconds)
        auto ts_result = doc["T"].get_int64();
        if (!ts_result.error()) {
            trade.ts_exchange_ms = ts_result.value();
        }

        // Local receive timestamp
        trade.ts_recv_ms = GetCurrentTimeMs();

        // Push trade to the pipeline
        PushTrade(std::move(trade));

    } catch (const simdjson_error& e) {
        std::cerr << "[BINANCE] JSON parse error: " << e.what() << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[BINANCE] Error parsing trade: " << e.what() << std::endl;
    }
}

class BinanceMessageParser {
   public:
    explicit BinanceMessageParser();
    ~BinanceMessageParser() = default;

    static Trade ParseTrade(std::string&& message);

   private:
    static Trade ParseTrade(const std::string& message);
};
