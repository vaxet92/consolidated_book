#pragma once

#include "md_provider/md_provider.h"
#include "types/venue.h"
#include <string>

namespace market_data {

class BybitProvider : public Provider {
   public:
    explicit BybitProvider(const ProviderConfig& config, CallBack callback, QuoteCallBack quote_callback = nullptr);
    ~BybitProvider() override = default;

   protected:
    void OnDepthMessage(const std::string& message, uint32_t conn_index) override;
    void OnBboMessage(const std::string& message, uint32_t conn_index) override;

    // Bybit uses one generic public endpoint - both depth and BBO need an
    // explicit {"op":"subscribe",...} frame after connecting.
    std::string DepthSubscriptionMessage() const override;
    std::string BboSubscriptionMessage() const override;

    const char* GetDepthPath() const override { return kByBitPath.data(); }
    const char* GetBboPath() const override { return kByBitPath.data(); }

   private:
    // Last applied `u` on the depth stream (orderbook.50). Bybit increments
    // it by exactly 1 per delta, so any other step is a gap. 0 means "no
    // snapshot yet" - only touched on the io_context thread, so no lock.
    uint64_t last_depth_u_{};
};

}  // namespace market_data
