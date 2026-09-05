#pragma once

#include "md_provider/md_provider.h"
#include "bybit_parser.h"
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

   private:
    // Last applied `u` on the depth stream (orderbook.50). Bybit increments
    // it by exactly 1 per delta, so any other step is a gap. 0 means "no
    // snapshot yet" - only touched on the io_context thread, so no lock.
    uint64_t last_depth_u_{};

    // Both OnDepthMessage and OnBboMessage run on the one io_context thread
    // and both use this parser (the orderbook.* shape is the same for the
    // depth and fast-BBO topics). No detached REST-snapshot path on Bybit.
    BybitParser parser_;
};

}  // namespace market_data
