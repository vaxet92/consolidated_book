#pragma once

#include "../base_provider.h"
#include <string>
#include <sstream>

class OKXProvider : public BaseProvider {
public:
    explicit OKXProvider(const ProviderConfig& config);
    ~OKXProvider() override = default;

protected:
    void OnMessage(const std::string& message) override;
    std::string GetSubscriptionMessage() const override;
    const char* GetHost() const override;
    const char* GetPort() const override;
    const char* GetPath() const override;

private:
    void ParseTrade(const std::string& message);
    
    static constexpr const char* OKX_HOST = "ws.okx.com";
    static constexpr const char* OKX_PORT = "8443";
};
