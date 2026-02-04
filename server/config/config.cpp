#include "config.h"
#include <iostream>
#include <sstream>
#include <algorithm>

ServerConfig ServerConfig::ParseFromArgs(int argc, char* argv[]) {
    ServerConfig config;
    
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        
        if (arg.find("--interval_ms=") == 0) {
            config.interval_ms = std::stoi(arg.substr(15));
        }
        else if (arg.find("--exchanges=") == 0) {
            std::string exchanges_str = arg.substr(12);
            std::stringstream ss(exchanges_str);
            std::string exchange;
            while (std::getline(ss, exchange, ',')) {
                config.exchanges.push_back(exchange);
            }
        }
        else if (arg.find("--instruments=") == 0) {
            std::string instruments_str = arg.substr(14);
            std::stringstream ss(instruments_str);
            std::string instrument;
            while (std::getline(ss, instrument, ',')) {
                config.instruments.push_back(instrument);
            }
        }
        else if (arg.find("--grpc_port=") == 0) {
            config.grpc_port = std::stoi(arg.substr(12));
        }
    }
    
    // Set defaults if not provided
    if (config.exchanges.empty()) {
        config.exchanges = {"binance", "bybit", "okx"};
    }
    if (config.instruments.empty()) {
        config.instruments = {"BTCUSDT", "ETHUSDT", "SOLUSDT"};
    }
    
    return config;
}

bool ServerConfig::Validate() const {
    if (interval_ms <= 0) {
        std::cerr << "Error: interval_ms must be positive\n";
        return false;
    }
    if (exchanges.empty()) {
        std::cerr << "Error: at least one exchange required\n";
        return false;
    }
    if (instruments.empty()) {
        std::cerr << "Error: at least one instrument required\n";
        return false;
    }
    return true;
}
