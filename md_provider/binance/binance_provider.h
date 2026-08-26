#pragma once

#include "md_provider/md_provider.h"
#include <string>

namespace market_data {

class BinanceProvider : public Provider {
   public:
    explicit BinanceProvider(const ProviderConfig& config, CallBack callback);
    ~BinanceProvider() override = default;

   protected:
    void OnDepthMessage(const std::string& message) override;
    void OnBboMessage(const std::string& message) override;

    // Binance connects directly to a per-stream URL (e.g. /ws/btcusdt@depth@100ms) -
    // no subscribe frame needed after connecting, unlike Bybit/OKX.
    std::string DepthSubscriptionMessage() const override { return ""; }
    std::string BboSubscriptionMessage() const override { return ""; }

    const char* GetHost() const override { return kHost; }
    const char* GetPort() const override { return kPort; }
    const char* GetDepthPath() const override { return depth_path_.c_str(); }
    const char* GetBboPath() const override { return bbo_path_.c_str(); }

   private:
    static constexpr const char* kHost = "stream.binance.com";  // spot, not fstream.binance.com (futures)
    static constexpr const char* kPort = "9443";

    std::string depth_path_;
    std::string bbo_path_;
};

}  // namespace market_data
