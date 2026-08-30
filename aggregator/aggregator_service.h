#pragma once

#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "aggregator.grpc.pb.h"
#include "conflated_channel.h"
#include "md_core/consolidated_bbo.h"
#include "md_core/consolidated_book.h"
#include "types/venue.h"

namespace market_data {

class AggregatorServiceImpl final : public wire::Aggregator::Service {
   public:
    grpc::Status Subscribe(grpc::ServerContext* context, const wire::SubscribeRequest* request,
                           grpc::ServerWriter<wire::Update>* writer) override;

    // Called by whoever owns Core (main.cpp) via Core's BboCallback whenever
    // a new consolidated BBO is available. Fans out to every session that
    // asked for BBO. May be called from any provider's thread - must be safe
    // to call concurrently.
    void PublishBbo(InstrumentId instrument, const consolidated::BBO& bbo);

    // Called via Core's BookCallback with a fresh, immutable merged book on
    // every depth update. This is where per-subscriber band selection
    // happens (§8.4): each session's own thresholds are walked against this
    // one shared snapshot, so Core never learns what any client asked for.
    void PublishBook(InstrumentId instrument, std::shared_ptr<const consolidated::Book> book);

   private:
    using Channel = ConflatedChannel<wire::Update>;

    struct Subscription {
        std::shared_ptr<Channel> channel;
        bool wants_bbo = false;
        bool wants_volume_bands = false;
        bool wants_price_bands = false;

        // Sorted ascending - FillToNotionalBands/FillToBpsBands require it
        // for their single forward walk to be correct. Sorted server-side at
        // subscribe time; never trusted from the wire.
        std::vector<uint64_t> notional_bands;
        std::vector<uint32_t> bps_bands;

        // Per-session, NOT global: seq means "position in THIS client's
        // stream", so a gap means this client's own channel conflated
        // something (§9.3). A counter shared across sessions would make
        // every client report gaps caused by traffic sent to other clients.
        // Mutated only under sessions_mutex_, so it needs no atomic.
        uint64_t next_seq = 0;
    };

    uint64_t RegisterSession(Subscription subscription);
    void UnregisterSession(uint64_t session_id);

    std::mutex sessions_mutex_;
    std::unordered_map<uint64_t, Subscription> sessions_;
    uint64_t next_session_id_ = 0;
};

}  // namespace market_data
