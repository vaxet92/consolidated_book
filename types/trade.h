#pragma once
#include <string>
#include <cstdint>
#include "exchange.h"

struct Trade {
    Exchange exchange;       // BINANCE, BYBIT, OKX
    std::string instrument;  // BTCUSDT, ETHUSDT, etc.
    uint64_t trade_id;       // for dedup
    double price;
    double qty;
    uint64_t event_ts;  // event ts from exchange
    uint64_t trade_ts;  // trade ts from exchange
    uint64_t recv_ts;   // local receive time
};