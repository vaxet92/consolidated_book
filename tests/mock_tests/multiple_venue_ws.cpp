#include "md_core/md_core.h"
#include "logger/logger.h"
#include "md_provider/binance/binance_provider.h"
#include "md_provider/bybit/bybit_provider.h"
#include "md_provider/okx/okx_provider.h"
#include <chrono>
#include <memory>
#include <thread>
#include <vector>
#include "types/venue.h"

int main() {
    Logger::Log(LogLevel::kInfo, "Hello, World!");

    market_data::CoreConfig config = {
        .venues = {VenueId::BINANCE, VenueId::BYBIT, VenueId::OKX},
        .default_instruments = {InstrumentId::BTCUSDT},
    };

    market_data::Core core;
    core.init(config);
    core.Start();

    auto on_update = [&core](const market_data::BookUpdate& update) { core.ApplyUpdate(update); };

    // Providers must outlive Start() - each Start() spawns its own internal
    // thread and returns immediately, so a provider constructed as a
    // temporary would be destroyed (and Stop()'d) right after Start()
    // returns, before any real data arrives. Keeping them here, in main()'s
    // scope, for the whole run is what avoids that.
    std::vector<std::unique_ptr<market_data::Provider>> providers;

    market_data::ProviderConfig binance_config = {
        .venue_id = VenueId::BINANCE,
        .instrument = InstrumentId::BTCUSDT,
        .host = std::string(kBinanceHost),
        .port = std::string(kBinancePort),
    };
    providers.push_back(std::make_unique<market_data::BinanceProvider>(binance_config, on_update));

    market_data::ProviderConfig bybit_config = {
        .venue_id = VenueId::BYBIT,
        .instrument = InstrumentId::BTCUSDT,
        .host = std::string(kBybitHost),
        .port = std::string(kBybitPort),
    };
    providers.push_back(std::make_unique<market_data::BybitProvider>(bybit_config, on_update));

    market_data::ProviderConfig okx_config = {
        .venue_id = VenueId::OKX,
        .instrument = InstrumentId::BTCUSDT,
        .host = std::string(kOkxHost),
        .port = std::string(kOkxPort),
    };
    providers.push_back(std::make_unique<market_data::OKXProvider>(okx_config, on_update));

    for (auto& provider : providers) {
        provider->Start();
    }

    std::this_thread::sleep_for(std::chrono::seconds(3));

    for (auto& provider : providers) {
        provider->Stop();
    }

    return 0;
}
