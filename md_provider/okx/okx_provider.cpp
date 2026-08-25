#include "okx_provider.h"
#include <iostream>
#include <simdjson.h>

using namespace simdjson;

OKXProvider::OKXProvider(const ProviderConfig& config)
    : BaseProvider(config) {
}

const char* OKXProvider::GetHost() const {
    return OKX_HOST;
}

const char* OKXProvider::GetPort() const {
    return OKX_PORT;
}

const char* OKXProvider::GetPath() const {
    return "/ws/v5/public";
}

std::string OKXProvider::GetSubscriptionMessage() const {
    // Build subscription message for OKX
    // Format: {"op":"subscribe","args":[{"channel":"trades","instId":"BTC-USDT"}]}
    
    std::ostringstream oss;
    oss << "{\"op\":\"subscribe\",\"args\":[";
    
    for (size_t i = 0; i < config.instruments.size(); ++i) {
        if (i > 0) oss << ",";
        
        // Convert BTCUSDT to BTC-USDT format
        std::string instrument = config.instruments[i];
        // Simple conversion: insert hyphen before USDT
        size_t usdt_pos = instrument.find("USDT");
        if (usdt_pos != std::string::npos) {
            instrument.insert(usdt_pos, "-");
        }
        
        oss << "{\"channel\":\"trades\",\"instId\":\"" << instrument << "\"}";
    }
    
    oss << "]}";
    
    return oss.str();
}

void OKXProvider::OnMessage(const std::string& message) {
    ParseTrade(message);
}

void OKXProvider::ParseTrade(const std::string& message) {
    try {
        ondemand::parser parser;
        padded_string json = padded_string(message);
        ondemand::document doc = parser.iterate(json);
        
        // Check if this has data field
        auto data_array = doc["data"].get_array();
        if (data_array.error()) {
            // Not a trade message, might be subscription confirmation
            return;
        }
        
        // Check arg for channel
        auto arg_result = doc["arg"]["channel"].get_string();
        if (!arg_result.error()) {
            std::string_view channel = arg_result.value();
            if (channel != "trades") {
                return;
            }
        }
        
        // Process each trade in the data array
        for (auto trade_item : data_array.value()) {
            Trade trade;
            trade.exchange = "OKX";
            
            // Instrument symbol (convert BTC-USDT back to BTCUSDT)
            auto inst_id_result = trade_item["instId"].get_string();
            if (!inst_id_result.error()) {
                std::string inst_id = std::string(inst_id_result.value());
                // Remove hyphen
                inst_id.erase(std::remove(inst_id.begin(), inst_id.end(), '-'), inst_id.end());
                trade.instrument = inst_id;
            }
            
            // Trade ID
            auto trade_id_result = trade_item["tradeId"].get_string();
            if (!trade_id_result.error()) {
                trade.trade_id = std::string(trade_id_result.value());
            }
            
            // Price
            auto price_result = trade_item["px"].get_string();
            if (!price_result.error()) {
                trade.price = std::stod(std::string(price_result.value()));
            }
            
            // Quantity
            auto qty_result = trade_item["sz"].get_string();
            if (!qty_result.error()) {
                trade.qty = std::stod(std::string(qty_result.value()));
            }
            
            // Exchange timestamp (ts field in milliseconds)
            auto ts_result = trade_item["ts"].get_string();
            if (!ts_result.error()) {
                trade.ts_exchange_ms = std::stoll(std::string(ts_result.value()));
            }
            
            // Local receive timestamp
            trade.ts_recv_ms = GetCurrentTimeMs();
            
            // Push trade to the pipeline
            PushTrade(std::move(trade));
        }
        
    } catch (const simdjson_error& e) {
        std::cerr << "[OKX] JSON parse error: " << e.what() << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[OKX] Error parsing trade: " << e.what() << std::endl;
    }
}
