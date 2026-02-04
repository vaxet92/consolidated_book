#pragma once

#include <string>
#include <cstdint>
#include "exchange.h"

struct Candle {
    InstrumentId instrument_id;
    Exchange exchange;
    uint64_t interval_ms;
    uint64_t window_start_ms;
    uint64_t window_end_ms;
    double open;
    double high;
    double low;
    double close;
    double base_volume;
    double quote_volume;
};
