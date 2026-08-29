#pragma once

#include "md_provider/md_provider.h"
#include <string>
#include <vector>

namespace market_data {

class BinanceProvider : public Provider {
   public:
    explicit BinanceProvider(const ProviderConfig& config, CallBack callback, QuoteCallBack quote_callback = nullptr);
    ~BinanceProvider() override = default;

   protected:
    void OnDepthMessage(const std::string& message) override;
    void OnBboMessage(const std::string& message) override;
    void OnReconnect() override;

    // Binance connects directly to a per-stream URL (e.g. /ws/btcusdt@depth@100ms) -
    // no subscribe frame needed after connecting, unlike Bybit/OKX.
    std::string DepthSubscriptionMessage() const override { return ""; }
    std::string BboSubscriptionMessage() const override { return ""; }

    const char* GetDepthPath() const override { return depth_path_.c_str(); }
    const char* GetBboPath() const override { return bbo_path_.c_str(); }

   private:
    // Binance's depth stream is DIFFERENTIAL only - it never sends a
    // snapshot. The book has to be seeded from REST, and the ordering
    // matters (DESIGN_1 §4.2): subscribe and buffer FIRST, fetch the
    // snapshot SECOND, then reconcile. Fetching first loses every update
    // that arrives during the round-trip.
    enum class SyncState {
        kSyncing,  // buffering WS events, waiting for the REST snapshot
        kLive,     // book seeded, applying deltas with continuity checks
    };

    // Called on the io_context thread once the stream is confirmed live.
    // Spawns a detached thread for the blocking HTTPS GET, because doing it
    // here would stall the very read loop that is buffering events, then
    // posts the result back onto the io_context thread.
    void FetchSnapshotAsync();

    // Runs on the io_context thread. Drops buffered events older than the
    // snapshot, checks that one of the survivors actually joins onto it,
    // and emits snapshot + survivors. Returns false if the snapshot is too
    // old to join (caller refetches).
    bool ReconcileSnapshot(BookUpdate snapshot);

    // Guards pending_ from growing without bound if the fetch hangs or the
    // snapshot keeps failing to join. Well above the ~10 events/sec this
    // stream produces, so hitting it means something is actually wrong.
    static constexpr size_t kMaxPendingEvents = 2000;
    // A snapshot older than the buffered events can't be joined; refetch.
    // Capped so a persistently stale endpoint can't loop forever.
    static constexpr int kMaxSnapshotAttempts = 5;

    std::string depth_path_;
    std::string bbo_path_;

    // All of these are only touched on the io_context thread (the snapshot
    // result is marshalled back there via net::post), so no locking.
    SyncState sync_state_ = SyncState::kSyncing;
    bool snapshot_requested_ = false;
    int snapshot_attempts_ = 0;
    std::vector<BookUpdate> pending_;
    uint64_t last_depth_u_ = 0;
};

}  // namespace market_data
