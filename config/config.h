#pragma once

#include <string>
#include <vector>
#include <map>
#include <iostream>
#include <sstream>
#include <algorithm>

struct ServerConfig {
    int interval_ms = 5;                   // default 5 seconds
    std::vector<std::string> venues;       // binance, bybit, okx
    std::vector<std::string> instruments;  // BTCUSDT, ETHUSDT, SOLUSDT
    int grpc_port = 50051;

    // Parse from command line arguments
    static ServerConfig ParseFromArgs(int argc, char* argv[]);

    // Validate configuration
    bool Validate() const;
};
