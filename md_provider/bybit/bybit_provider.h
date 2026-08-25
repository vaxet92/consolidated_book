#pragma once

#include "../base_provider.h"
#include <string>
#include <sstream>

class BybitProvider : public BaseProvider {
public:
    explicit BybitProvider(const ProviderConfig& config);
    ~BybitProvider() override = default;

protected:
    void OnMessage(const std::string& message) override;
    std::string GetSubscriptionMessage() const override;
    const char* GetHost() const override;
    const char* GetPort() const override;
    const char* GetPath() const override;

private:
    void ParseTrade(const std::string& message);
    
    static constexpr const char* BYBIT_HOST = "stream.bybit.com";
    static constexpr const char* BYBIT_PORT = "443";
};


// wss://stream.bybit.com/v5/public/spot

// {
//     "op": "subscribe",
//     "args": [
//       "publicTrade.BTCUSDT",
//       "publicTrade.ETHUSDT",
//       "publicTrade.SOLUSDT"
//     ]
//   }