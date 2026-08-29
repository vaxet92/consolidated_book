#pragma once

#include "md_provider/md_provider.h"
#include "types/venue.h"
#include <string>

namespace market_data {

class OKXProvider : public Provider {
   public:
    explicit OKXProvider(const ProviderConfig& config, CallBack callback, QuoteCallBack quote_callback = nullptr);
    ~OKXProvider() override = default;

   protected:
    void OnDepthMessage(const std::string& message) override;
    void OnBboMessage(const std::string& message) override;

    // OKX uses one generic public endpoint - both depth and BBO need an
    // explicit {"op":"subscribe",...} frame after connecting.
    std::string DepthSubscriptionMessage() const override;
    std::string BboSubscriptionMessage() const override;

    const char* GetDepthPath() const override { return kOkxPath.data(); }
    const char* GetBboPath() const override { return kOkxPath.data(); }

   private:
    // Last applied `seqId` on the depth stream (books). OKX chains messages
    // by prevSeqId == previous seqId - the ids are NOT contiguous, so a
    // "+1" check like Bybit's would be wrong here. 0 means "no snapshot
    // yet"; only touched on the io_context thread, so no lock.
    uint64_t last_depth_seq_ = 0;
};

}  // namespace market_data
