#include "aggregator_service.h"
#include "wire_translation.h"
#include "logger/logger.h"

#include <fmt/ranges.h>  // fmt::join

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

namespace market_data {

namespace {

int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// DESIGN_1 §8.2/§8.3 defaults, applied when a client subscribes to a band
// feed but sends an empty threshold list. Notionals are USDT x 1e8, matching
// FillToNotional's target scale.
constexpr uint64_t kMillion = 1'000'000ULL * kScaleFactor;
const std::vector<uint64_t> kDefaultNotionalBands = {
    1 * kMillion};  // 1 * kMillion, 5 * kMillion, 10 * kMillion, 25 * kMillion, 50 * kMillion
const std::vector<uint32_t> kDefaultBpsBands = {500};  // 50, 100, 200, 500, 1000

// Fills the common header every Update carries regardless of payload, so a
// client never needs state from an earlier message to interpret this one
// (§7.4 - a state-publishing API, not an event log).
// TODO(futures): the wire carries the SYMBOL but not the MARKET. A client
// cannot yet tell a spot BTCUSDT update from a futures one - they arrive with
// the same `symbol` string. Core keeps them in separate books correctly; the
// proto needs a market_type field on SubscribeRequest and Update before
// futures is usable end to end. See DESIGN.md futures step F5.
void FillHeader(wire::Update& update, InstrumentKey instrument) {
    update.set_server_ts_ns(NowNs());
    // Symbol() only - ToInstrumentString(InstrumentKey) would emit
    // "BTCUSDT:spot", which is a log format, not a wire symbol.
    update.set_symbol(VenueConverter::ToInstrumentString(instrument.Symbol()));
    update.set_price_scale(kScaleExponent);
    update.set_qty_scale(kScaleExponent);
}

}  // namespace

uint64_t AggregatorServiceImpl::RegisterSession(Subscription subscription) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    uint64_t id = next_session_id_++;
    sessions_[id] = std::move(subscription);
    return id;
}

void AggregatorServiceImpl::UnregisterSession(uint64_t session_id) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    sessions_.erase(session_id);
}

grpc::Status AggregatorServiceImpl::Subscribe(grpc::ServerContext* context, const wire::SubscribeRequest* request,
                                              grpc::ServerWriter<wire::Update>* writer) {
    const std::optional<InstrumentId> instrument = VenueConverter::ToInstrumentId(request->symbol());
    if (!instrument.has_value()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "unknown symbol: " + request->symbol());
    }

    // Feeds are selected by PRESENCE (§8.4): bbo is a bool; the band feeds
    // are optional sub-messages whose presence means "subscribed" and whose
    // (possibly empty) array picks the thresholds.
    Subscription subscription;
    subscription.channel = std::make_shared<Channel>();
    subscription.wants_bbo = request->bbo();
    subscription.wants_volume_bands = request->has_volume_bands();
    subscription.wants_price_bands = request->has_price_bands();

    if (!subscription.wants_bbo && !subscription.wants_volume_bands && !subscription.wants_price_bands) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "subscription requests no feeds");
    }

    if (subscription.wants_volume_bands) {
        const auto& bands = request->volume_bands().notional_bands();
        subscription.notional_bands.assign(bands.begin(), bands.end());
        if (subscription.notional_bands.empty()) {
            subscription.notional_bands = kDefaultNotionalBands;
        }
        // FillToNotionalBands walks forward once and never rewinds, which is
        // only correct if the targets ascend. Sorted here rather than
        // trusted from the wire.
        std::sort(subscription.notional_bands.begin(), subscription.notional_bands.end());
    }
    if (subscription.wants_price_bands) {
        const auto& bands = request->price_bands().bps_bands();
        subscription.bps_bands.assign(bands.begin(), bands.end());
        if (subscription.bps_bands.empty()) {
            subscription.bps_bands = kDefaultBpsBands;
        }
        std::sort(subscription.bps_bands.begin(), subscription.bps_bands.end());
    }

    // Built BEFORE the move below - subscription's vectors are gone after it.
    // Not an if/else chain: a client may subscribe to several feeds at once,
    // and reporting only the first would misrepresent what it asked for.
    std::string feeds;
    if (subscription.wants_bbo) {
        feeds += "BBO ";
    }
    if (subscription.wants_volume_bands) {
        // Printed in whole millions rather than raw scaled integers - the
        // defaults are 1e14..5e15, which are unreadable in a log line.
        std::vector<uint64_t> millions;
        millions.reserve(subscription.notional_bands.size());
        for (uint64_t notional : subscription.notional_bands) {
            millions.push_back(notional / kScaleFactor / 1'000'000);
        }
        feeds += fmt::format("VOLUME_BANDS[{}M] ", fmt::join(millions, "M,"));
    }
    if (subscription.wants_price_bands) {
        feeds += fmt::format("PRICE_BANDS[{}bps] ", fmt::join(subscription.bps_bands, "bps,"));
    }

    auto channel = subscription.channel;  // kept alive for the read loop below
    uint64_t session_id = RegisterSession(std::move(subscription));
    Logger::Log(LogLevel::kInfo, "[Aggregator] session {} subscribed (symbol={}) {}", session_id, request->symbol(),
                feeds);

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

void AggregatorServiceImpl::PublishBbo(InstrumentKey instrument, const consolidated::BBO& bbo) {
    wire::Update update;
    FillHeader(update, instrument);
    *update.mutable_bbo() = ToWire(bbo, venue_wire_table_);

    std::lock_guard<std::mutex> lock(sessions_mutex_);
    for (auto& [id, subscription] : sessions_) {
        if (!subscription.wants_bbo) {
            continue;  // subscribed to bands only
        }
        // seq is per-session and assigned under the same lock that orders
        // the pushes, so a client can never see it go backwards.
        update.set_seq(subscription.next_seq++);
        subscription.channel->Push(update);
    }
}

void AggregatorServiceImpl::PublishBook(InstrumentKey instrument, std::shared_ptr<const consolidated::Book> book) {
    if (!book) {
        return;
    }

    std::lock_guard<std::mutex> lock(sessions_mutex_);

    // Band math runs PER SESSION, against this one shared snapshot - that is
    // what makes §8.4's per-client thresholds possible without Core knowing
    // anything about clients. The expensive part (the merge) was already
    // paid once, by Core.
    //
    // These vectors allocate per session per publish. Known cost, accepted
    // for the first version: §7.5 wants no allocation on hot paths, and the
    // fix (scratch buffers reused across publishes, safe here because this
    // all runs under sessions_mutex_) is deliberately deferred until a
    // benchmark says it matters.
    std::vector<consolidated::NotionalFill> bid_fills;
    std::vector<consolidated::NotionalFill> ask_fills;
    std::vector<consolidated::BpsFill> bid_bps;
    std::vector<consolidated::BpsFill> ask_bps;

    for (auto& [id, subscription] : sessions_) {
        if (subscription.wants_volume_bands) {
            consolidated::FillToNotionalBands(book->bids, subscription.notional_bands, bid_fills);
            consolidated::FillToNotionalBands(book->asks, subscription.notional_bands, ask_fills);

            wire::Update update;
            FillHeader(update, instrument);
            wire::VolumeBands* bands = update.mutable_volume_bands();
            // Slippage reference: the top of each side, or 0 if that side is
            // empty. Sent once per message, not per band.
            bands->set_best_bid(book->bids.empty() ? 0 : book->bids.front().price);
            bands->set_best_ask(book->asks.empty() ? 0 : book->asks.front().price);
            for (size_t i = 0; i < subscription.notional_bands.size(); ++i) {
                *bands->add_bid_bands() = ToWire(bid_fills[i], subscription.notional_bands[i]);
                *bands->add_ask_bands() = ToWire(ask_fills[i], subscription.notional_bands[i]);
            }
            update.set_seq(subscription.next_seq++);
            subscription.channel->Push(update);
        }

        if (subscription.wants_price_bands) {
            consolidated::FillToBpsBands(book->bids, subscription.bps_bands, /*is_bid=*/true, bid_bps);
            consolidated::FillToBpsBands(book->asks, subscription.bps_bands, /*is_bid=*/false, ask_bps);

            wire::Update update;
            FillHeader(update, instrument);
            wire::PriceBands* bands = update.mutable_price_bands();
            for (size_t i = 0; i < subscription.bps_bands.size(); ++i) {
                *bands->add_bid_bands() = ToWire(bid_bps[i], subscription.bps_bands[i]);
                *bands->add_ask_bands() = ToWire(ask_bps[i], subscription.bps_bands[i]);
            }
            update.set_seq(subscription.next_seq++);
            subscription.channel->Push(update);
        }
    }
}

}  // namespace market_data
