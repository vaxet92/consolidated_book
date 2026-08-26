#include "md_core/md_core.h"
#include "logger/logger.h"

int main() {
    Logger::Log(LogLevel::kInfo, "Hello, World!");

    MDCoreConfig config = {
        .venues = {VenueId::BINANCE, VenueId::BYBIT, VenueId::OKX},
        .default_instruments = {InstrumentId::BTCUSDT},
    };

    MDCore core;
    core.init(config);

    return 0;
}