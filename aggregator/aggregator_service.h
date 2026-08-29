#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>

#include <grpcpp/grpcpp.h>

#include "aggregator.grpc.pb.h"
#include "conflated_channel.h"
#include "md_core/consolidated_bbo.h"
#include "types/venue.h"

namespace market_data {

class AggregatorServiceImpl final : public wire::Aggregator::Service {
   public:
    grpc::Status Subscribe(grpc::ServerContext* context, const wire::SubscribeRequest* request,
                           grpc::ServerWriter<wire::Update>* writer) override;

    // Called by whoever owns Core (main.cpp), via Core's BboCallback,
    // whenever a new consolidated BBO is available. Fans out to every
    // currently-subscribed BBO session. May be called from any provider's
    // thread - must be safe to call concurrently.
    void PublishBbo(InstrumentId instrument, const consolidated::BBO& bbo);

   private:
    using Channel = ConflatedChannel<wire::Update>;

    uint64_t RegisterSession(std::shared_ptr<Channel> channel);
    void UnregisterSession(uint64_t session_id);

    std::mutex sessions_mutex_;
    std::unordered_map<uint64_t, std::shared_ptr<Channel>> bbo_sessions_;
    uint64_t next_session_id_ = 0;
    std::atomic<uint64_t> next_seq_{0};
};

}  // namespace market_data
