#include "bybit_provider.h"
#include <iostream>
#include <simdjson.h>

using namespace simdjson;

BybitProvider::BybitProvider(const ProviderConfig& config)
    : BaseProvider(config) {
}

const char* BybitProvider::GetHost() const {
    return BYBIT_HOST;
}

const char* BybitProvider::GetPort() const {
    return BYBIT_PORT;
}

const char* BybitProvider::GetPath() const {
    return "/v5/public/linear";
}

std::string BybitProvider::GetSubscriptionMessage() const {
    // Build subscription message for Bybit
    // Format: {"op":"subscribe","args":["publicTrade.BTCUSDT","publicTrade.ETHUSDT"]}
    
    std::ostringstream oss;
    oss << "{\"op\":\"subscribe\",\"args\":[";
    
    for (size_t i = 0; i < config.instruments.size(); ++i) {
        if (i > 0) oss << ",";
        oss << "\"publicTrade." << config.instruments[i] << "\"";
    }
    
    oss << "]}";
    
    return oss.str();
}

void BybitProvider::OnMessage(const std::string& message) {
    ParseTrade(message);
}

void BybitProvider::ParseTrade(const std::string& message) {
    try {
        ondemand::parser parser;
        padded_string json = padded_string(message);
        ondemand::document doc = parser.iterate(json);
        
        // Check if this is a trade topic
        std::string_view topic;
        auto topic_result = doc["topic"].get_string();
        if (topic_result.error()) {
            // Not a trade message, might be subscription confirmation
            return;
        }
        topic = topic_result.value();
        
        if (topic.find("publicTrade") == std::string_view::npos) {
            return;
        }
        
        // Parse data array
        auto data_array = doc["data"].get_array();
        if (data_array.error()) {
            return;
        }
        
        // Process each trade in the data array
        for (auto trade_item : data_array.value()) {
            Trade trade;
            trade.exchange = "BYBIT";
            
            // Instrument symbol
            auto symbol_result = trade_item["s"].get_string();
            if (!symbol_result.error()) {
                trade.instrument = std::string(symbol_result.value());
            }
            
            // Trade ID
            auto trade_id_result = trade_item["i"].get_string();
            if (!trade_id_result.error()) {
                trade.trade_id = std::string(trade_id_result.value());
            }
            
            // Price
            auto price_result = trade_item["p"].get_string();
            if (!price_result.error()) {
                trade.price = std::stod(std::string(price_result.value()));
            }
            
            // Quantity
            auto qty_result = trade_item["v"].get_string();
            if (!qty_result.error()) {
                trade.qty = std::stod(std::string(qty_result.value()));
            }
            
            // Exchange timestamp (T field in milliseconds)
            auto ts_result = trade_item["T"].get_int64();
            if (!ts_result.error()) {
                trade.ts_exchange_ms = ts_result.value();
            }
            
            // Local receive timestamp
            trade.ts_recv_ms = GetCurrentTimeMs();
            
            // Push trade to the pipeline
            PushTrade(std::move(trade));
        }
        
    } catch (const simdjson_error& e) {
        std::cerr << "[BYBIT] JSON parse error: " << e.what() << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[BYBIT] Error parsing trade: " << e.what() << std::endl;
    }
}
