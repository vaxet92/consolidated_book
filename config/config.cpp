#include "config.h"

ServerConfig ServerConfig::ParseFromArgs(int argc, char* argv[]) {
    ServerConfig config;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg.find("--venues=") == 0) {
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
        } else if (arg.find("--depth=") == 0) {
            config.depth = static_cast<uint32_t>(std::stoul(arg.substr(8)));
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
        }
    }

    // Defaults are applied AFTER the loop, not inside it. Inside, they fire
    // on the first iteration - so `--grpc_port=1234 --venues=binance` would
    // default venues to all three on iteration 1, then APPEND binance on
    // iteration 2, yielding {binance,bybit,okx,binance} instead of {binance}.
    if (config.venues.empty()) {
        config.venues = {"binance", "bybit", "okx"};
    }
    if (config.instruments.empty()) {
        // BTCUSDT only: main.cpp builds one provider per venue for a single
        // instrument. ETHUSDT/SOLUSDT exist in InstrumentId, but multi-symbol
        // is designed for rather than exercised (DESIGN_1 §1.2), so
        // defaulting to all three would promise something not wired up.
        config.instruments = {"BTCUSDT"};
    }
    return config;
}

bool ServerConfig::Validate() const {
    if (venues.empty()) {
        std::cerr << "Error: at least one venue required\n";
        return false;
    }
    if (instruments.empty()) {
        std::cerr << "Error: at least one instrument required\n";
        return false;
    }
    if (depth == 0) {
        std::cerr << "Error: --depth must be greater than zero\n";
        return false;
    }
    return true;
}
