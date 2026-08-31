#include "binance_provider.h"
#include "binance_parser.h"
#include "binance_rest.h"
#include "continuity.h"
#include "logger/logger.h"
#include "types/venue.h"
#include <fmt/format.h>
#include <algorithm>
#include <cctype>
#include <thread>
#include <utility>

using namespace market_data;

namespace {

std::string ToLowerSymbol(InstrumentId instrument) {
    std::string symbol = VenueConverter::ToInstrumentString(instrument);
    std::transform(symbol.begin(), symbol.end(), symbol.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return symbol;
}

std::string UpperSymbol(InstrumentId instrument) {
    return VenueConverter::ToInstrumentString(instrument);
}

}  // namespace

BinanceProvider::BinanceProvider(const ProviderConfig& config, CallBack callback, QuoteCallBack quote_callback)
    : Provider(config, std::move(callback), std::move(quote_callback)) {
    std::string symbol = ToLowerSymbol(config.instrument);
    depth_path_ = fmt::format("/ws/{}@depth@100ms", symbol);
    bbo_path_ = fmt::format("/ws/{}@bookTicker", symbol);
}

void BinanceProvider::OnReconnect() {
    // Per-connection state only. Binance never re-sends a snapshot on the
    // stream, so without this a resync would leave us in kLive with a stale
    // last_depth_u_ and resync forever.
    sync_state_ = SyncState::kSyncing;
    snapshot_requested_ = false;
    snapshot_attempts_ = 0;
    pending_.clear();
    last_depth_u_ = 0;
}

void BinanceProvider::FetchSnapshotAsync() {
    snapshot_requested_ = true;
    ++snapshot_attempts_;

    // Depth is a REST query parameter on Binance - already resolved to a
    // valid limit (5/10/20/50/100/500/1000/5000) by SelectDepthTier. An
    // arbitrary value here would be rejected by the API.
    std::string target =
        fmt::format("/api/v3/depth?symbol={}&limit={}", UpperSymbol(config.instrument), config.depth);
    std::string host(kBinanceRestHost);
    std::string port(kBinanceRestPort);
    VenueId venue = config.venue_id;
    InstrumentId instrument = config.instrument;

    // Detached thread: HttpsGet blocks, and doing that on the io_context
    // thread would stall the read loop that is currently buffering events.
    std::thread([this, host, port, target, venue, instrument]() {
        auto body = HttpsGet(host, port, target);
        auto snapshot = body ? ParseBinanceDepthSnapshot(*body, venue, instrument) : std::nullopt;

        // Back onto the io_context thread - everything below touches state
        // shared with the message handlers.
        PostToIoContext([this, snapshot = std::move(snapshot)]() mutable {
            if (!snapshot) {
                Logger::Log(LogLevel::kError, "[BINANCE] depth snapshot fetch failed");
                if (snapshot_attempts_ >= kMaxSnapshotAttempts) {
                    RequestResync();
                    return;
                }
                FetchSnapshotAsync();
                return;
            }

            if (!ReconcileSnapshot(std::move(*snapshot))) {
                // Snapshot predates our buffered events - it cannot be
                // joined onto them. Refetch a newer one.
                if (snapshot_attempts_ >= kMaxSnapshotAttempts) {
                    Logger::Log(LogLevel::kError, "[BINANCE] snapshot never caught up after {} attempts - resyncing",
                                snapshot_attempts_);
                    RequestResync();
                    return;
                }
                Logger::Log(LogLevel::kWarning, "[BINANCE] snapshot too old to join buffered events - refetching");
                FetchSnapshotAsync();
            }
        });
    }).detach();
}

bool BinanceProvider::ReconcileSnapshot(BookUpdate snapshot) {
    const uint64_t last_update_id = snapshot.seq;

    // Which buffered event (if any) joins onto this snapshot. nullopt means
    // the snapshot predates the buffer and cannot be joined - refetch.
    auto first = ReconcileBinanceSnapshot(last_update_id, pending_);
    if (!first) {
        return false;
    }

    snapshot.recv_ts_ns = GetCurrentTimeMs() * 1'000'000;
    Emit(snapshot);
    last_depth_u_ = last_update_id;

    for (size_t i = *first; i < pending_.size(); ++i) {
        Emit(pending_[i]);
        last_depth_u_ = pending_[i].seq;
    }
    pending_.clear();

    sync_state_ = SyncState::kLive;
    Logger::Log(LogLevel::kInfo, "[BINANCE] depth synced at lastUpdateId={}, now live", last_update_id);
    return true;
}

void BinanceProvider::OnDepthMessage(const std::string& message, uint32_t conn_index) {
    auto update = ParseBinanceDepthMessage(message, config.venue_id, config.instrument);
    if (!update) {
        return;  // not a depth update (e.g. a control/ack message)
    }
    update->recv_ts_ns = GetCurrentTimeMs() * 1'000'000;

    // Redundant-connection dedup, BEFORE the sync branch below - not after.
    //
    // KEY: during kSyncing every event is buffered into pending_, so a
    // duplicate reaching that branch would be buffered too, and
    // ReconcileSnapshot would emit each event N times when it drains. Worse,
    // pending_ is capped at kMaxPendingEvents: with three connections it
    // would fill three times faster and trip the "buffered too long,
    // resyncing" path during the exact window where the REST snapshot is
    // still in flight.
    //
    // venue_reset is always false here. Binance never sends a snapshot on the
    // stream - the book is seeded from REST - so the id only climbs and the
    // high-water mark never has to move backwards.
    if (!AcceptDepth(update->seq, conn_index, /*venue_reset=*/false)) {
        return;
    }

    if (sync_state_ == SyncState::kSyncing) {
        // Subscribe-and-buffer FIRST, snapshot second (§4.2). The fetch is
        // kicked off from here - the first event proves the stream is
        // actually delivering, and Provider has no "connected" hook.
        if (!snapshot_requested_) {
            FetchSnapshotAsync();
        }
        if (pending_.size() >= kMaxPendingEvents) {
            Logger::Log(LogLevel::kError, "[BINANCE] buffered {} events without a snapshot - resyncing",
                        pending_.size());
            RequestResync();
            return;
        }
        pending_.push_back(std::move(*update));
        return;
    }

    // Live: each event must continue the chain, U == last_u + 1 (§4.3).
    // Binance never sends a snapshot on the stream, so kReset/kIgnore
    // cannot occur here - only kApply or kGap.
    if (CheckBinanceContinuity(*update, last_depth_u_) == ContinuityAction::kGap) {
        Logger::Log(LogLevel::kWarning, "[BINANCE] depth gap: expected U={}, got {} - resyncing", last_depth_u_ + 1,
                    update->prev_seq);
        RequestResync();
        return;
    }
    Emit(*update);
}

void BinanceProvider::OnBboMessage(const std::string& message, uint32_t conn_index) {
    auto quote = ParseBinanceBboMessage(message, config.venue_id, config.instrument);
    if (!quote) {
        return;  // not a bookTicker payload (e.g. a subscribe ack)
    }

    // Separate filter from the depth stream: @bookTicker and @depth carry
    // independent `u` sequences, so a shared high-water mark would silently
    // drop one stream behind the other.
    if (!AcceptBbo(quote->seq, conn_index)) {
        return;
    }

    quote->recv_ts_ns = GetCurrentTimeMs() * 1'000'000;
    EmitQuote(*quote);
}
