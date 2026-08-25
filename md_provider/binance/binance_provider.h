#pragma once

#include "provider/base_provider.h"
#include <string>
#include <sstream>

class BinanceProvider : public BaseProvider {
public:
    explicit BinanceProvider(const ProviderConfig& config);
    ~BinanceProvider() override = default;

protected:
    void OnMessage(const std::string& message) override;
    std::string GetSubscriptionMessage() const override;
    const char* GetHost() const override;
    const char* GetPort() const override;
    const char* GetPath() const override;

private:
    void ParseTrade(const std::string& message);
    
    static constexpr const char* BINANCE_HOST = "fstream.binance.com";
    static constexpr const char* BINANCE_PORT = "443";
};

