#include "aggregator_service.h"
#include "wire_translation.h"
#include "logger/logger.h"

#include <chrono>

namespace market_data {

namespace {

// Matches PriceTicks/QtyUnits' fixed 1e8 scale - a hardcoded constant, not
// computed, since there's only one scale in this project today.
constexpr uint32_t kPriceScale = 8;
constexpr uint32_t kQtyScale = 8;

int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

uint64_t AggregatorServiceImpl::RegisterSession(std::shared_ptr<Channel> channel) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    uint64_t id = next_session_id_++;
    bbo_sessions_[id] = std::move(channel);
    return id;
}

void AggregatorServiceImpl::UnregisterSession(uint64_t session_id) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    bbo_sessions_.erase(session_id);
}

grpc::Status AggregatorServiceImpl::Subscribe(grpc::ServerContext* context, const wire::SubscribeRequest* request,
                                              grpc::ServerWriter<wire::Update>* writer) {
    for (int i = 0; i < request->feeds_size(); ++i) {
        if (request->feeds(i) != wire::BBO) {
            return grpc::Status(
                grpc::StatusCode::UNIMPLEMENTED,
                "only BBO is implemented so far - VOLUME_BANDS/PRICE_BANDS band math doesn't exist yet");
        }
    }

    InstrumentId instrument = VenueConverter::ToInstrumentId(request->symbol());
    if (instrument == InstrumentId::UNKNOWN) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "unknown symbol: " + request->symbol());
    }

    auto channel = std::make_shared<Channel>();
    uint64_t session_id = RegisterSession(channel);
    Logger::Log(LogLevel::kInfo, "[Aggregator] session {} subscribed (symbol={})", session_id, request->symbol());

    while (!context->IsCancelled()) {
        auto update = channel->WaitAndTake();
        if (!update) {
            break;  // channel closed
        }
        if (!writer->Write(*update)) {
            break;  // client gone
        }
    }

    UnregisterSession(session_id);
    Logger::Log(LogLevel::kInfo, "[Aggregator] session {} disconnected", session_id);
    return grpc::Status::OK;
}

void AggregatorServiceImpl::PublishBbo(InstrumentId instrument, const consolidated::BBO& bbo) {
    wire::Update update;
    update.set_server_ts_ns(NowNs());
    update.set_symbol(VenueConverter::ToInstrumentString(instrument));
    update.set_price_scale(kPriceScale);
    update.set_qty_scale(kQtyScale);
    *update.mutable_bbo() = ToWire(bbo);

    // seq is assigned under the same lock that orders the pushes, so a
    // client can never see seq go backwards. Doing it outside the lock
    // works today only because Core::ApplyUpdate happens to hold its own
    // mutex across this callback - an accidental guarantee, not a local one.
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    update.set_seq(next_seq_.fetch_add(1, std::memory_order_relaxed));
    for (auto& [id, channel] : bbo_sessions_) {
        channel->Push(update);
    }
}

}  // namespace market_data
