#include "md_core/md_core.h"
#include "logger/logger.h"
#include "md_provider/binance/binance_provider.h"
#include <chrono>
#include <thread>

int main() {
    Logger::Log(LogLevel::kInfo, "Hello, World!");

    market_data::CoreConfig config = {
        .venues = {VenueId::BINANCE, VenueId::BYBIT, VenueId::OKX},
        .default_instruments = {InstrumentId::BTCUSDT},
    };

    market_data::Core core;
    core.init(config);
    core.Start();

    market_data::ProviderConfig provider_config = {
        .venue_id = VenueId::BINANCE,
        .instrument = InstrumentId::BTCUSDT,
    };

    market_data::BinanceProvider provider(provider_config,
                                          [&core](const market_data::BookUpdate& update) { core.ApplyUpdate(update); });
    provider.Start();
    std::this_thread::sleep_for(std::chrono::seconds(30));
    provider.Stop();
    return 0;
}