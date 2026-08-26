#include "config.h"

ServerConfig ServerConfig::ParseFromArgs(int argc, char* argv[]) {
    ServerConfig config;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg.find("--interval_ms=") == 0) {
            config.interval_ms = std::stoi(arg.substr(15));
        } else if (arg.find("--venues=") == 0) {
            std::string venues_str = arg.substr(9);
            std::stringstream ss(venues_str);
            std::string venue;
            while (std::getline(ss, venue, ',')) {
                config.venues.push_back(venue);
            }
        } else if (arg.find("--instruments=") == 0) {
            std::string instruments_str = arg.substr(14);
            std::stringstream ss(instruments_str);
            std::string instrument;
            while (std::getline(ss, instrument, ',')) {
                config.instruments.push_back(instrument);
            }
        } else if (arg.find("--grpc_port=") == 0) {
            config.grpc_port = std::stoi(arg.substr(12));
        }
    }

    // Set defaults if not provided
    if (config.venues.empty()) {
        config.venues = {"binance", "bybit", "okx"};
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
    if (venues.empty()) {
        std::cerr << "Error: at least one venue required\n";
        return false;
    }
    if (instruments.empty()) {
        std::cerr << "Error: at least one instrument required\n";
        return false;
    }
    return true;
}
