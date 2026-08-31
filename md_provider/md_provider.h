#pragma once

#include "ws/ws.h"
#include "seq_dedup.h"
#include "types/venue.h"
#include "md_core/types.h"
#include <vector>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <thread>
#include <atomic>
#include <memory>
#include <chrono>
#include <string>
#include <functional>

namespace market_data {

struct ProviderConfig {
    VenueId venue_id;
    InstrumentId instrument;  // spot only for now
    std::string host;         // e.g. "stream.binance.com" - venue-specific, but data, not baked into the class
    std::string port;         // e.g. "9443"

    // Book depth for THIS venue, already resolved to one of its published
    // tiers by SelectDepthTier (config/config.h). Not the raw --depth value:
    // venues only publish at fixed tiers, and each one rounds up differently.
    // Where it takes effect also differs - a REST query parameter on Binance,
    // the WS topic name on Bybit, and nothing at all on OKX, whose `books`
    // channel is fixed at 400.
    uint32_t depth = 500;

    // Redundant WebSocket connections per stream ("line arbitration"), from
    // ServerConfig::connections. N sockets carry the same messages; the first
    // copy of each wins and the rest are dropped by SeqDedup. The benefit is
    // failover - one socket dying leaves N-1 delivering, so there is no gap
    // and no resync.
    //
    // 1 means no redundancy, and behaves exactly as before the feature
    // existed. Capped at kMaxConnections by config validation.
    uint32_t connections = 1;
};

class Provider {
   public:
    using CallBack = std::function<void(const BookUpdate&)>;
    using QuoteCallBack = std::function<void(const BboQuote&)>;

    explicit Provider(const ProviderConfig& config, CallBack callback, QuoteCallBack quote_callback = nullptr);
    virtual ~Provider();

    // Start the provider (in a separate thread)
    void Start();
    // Stop the provider
    void Stop();

   protected:
    // Depth stream: the real book. Venue-specific parsing.
    //
    // `conn_index` identifies which redundant socket delivered this copy,
    // 0-based. The subclass passes it to SeqDedup, which uses it for
    // connection-health reporting only - never for the accept/reject
    // decision.
    virtual void OnDepthMessage(const std::string& message, uint32_t conn_index) = 0;
    virtual std::string DepthSubscriptionMessage() const = 0;
    virtual const char* GetDepthPath() const = 0;

    // Host/port are config data now, not per-venue behavior - no subclass
    // needs to override these anymore.
    const char* GetHost() const { return config.host.c_str(); }
    const char* GetPort() const { return config.port.c_str(); }

    // Fast-BBO stream (DESIGN_1 §4.4 option 1): a separate, lower-latency
    // publishing path, NOT spliced into the depth book - the two streams
    // are not mutually sequenced, so mixing them corrupts the book (§7).
    // Binance @bookTicker is real-time; Bybit orderbook.1 and OKX bbo-tbt
    // are ~10ms, against 100ms-throttled depth.
    virtual void OnBboMessage(const std::string& message, uint32_t conn_index) = 0;
    virtual std::string BboSubscriptionMessage() const = 0;
    virtual const char* GetBboPath() const = 0;

    // Duplicate rejection for redundant connections. True -> this is the
    // first copy, process it. False -> already seen, drop it.
    //
    // Call AFTER parsing (venue_reset needs the parsed message) and BEFORE
    // the continuity check: a duplicate reaching continuity looks like a
    // sequence break, which resyncs - the exact outage redundancy prevents.
    //
    // `venue_reset` means the VENUE restarted its sequence so the id moves
    // BACKWARDS: Bybit's u == 1 service restart, or OKX's maintenance reset
    // where seqId < prevSeqId. Only those.
    //
    // KEY: an ordinary snapshot is NOT a venue reset and must pass false. Its
    // id moves forward in the venue's numbering, so `<=` already decides
    // correctly - newer than us, apply it; older, drop it as stale. Flagging
    // every snapshot as a reset is what let a late-connecting socket drag the
    // mark backwards and manufacture a gap.
    bool AcceptDepth(uint64_t id, uint32_t conn_index, bool venue_reset);

    // Same, for the fast-BBO stream. No reset flag: these streams push
    // stateless top-of-book quotes and never send a snapshot.
    bool AcceptBbo(uint64_t id, uint32_t conn_index);

    // Called by child classes with a depth-derived BookUpdate.
    // Runs on this provider's own thread - must become a queue push instead
    // of a direct call once providers run concurrently with the
    // consolidator (see our discussion on CallBack being a temporary shape).
    void Emit(const BookUpdate& update);

    // Called by child classes with a top-of-book quote from the fast-BBO
    // stream. Same threading caveat as Emit().
    void EmitQuote(const BboQuote& quote);

    // Called by a child class when it detects a sequence gap on the depth
    // stream: the book is now WRONG (DESIGN_1 §4.2 - strictly worse than
    // stale), and for an in-channel-snapshot venue the only way to get a
    // fresh snapshot is to re-subscribe. Tears the sessions down so Run()
    // reconnects.
    //
    // Deliberately NOT routed through HandleReconnection(): a gap is a
    // normal operational event, not a connection failure, so it must not
    // consume the MAX_RECONNECT_ATTEMPTS budget that permanently stops the
    // provider.
    //
    // Safe to call from inside a message handler (it runs on the io_context
    // thread; io_context::stop() is safe there).
    //
    // NOTE: both sessions share one io_context, so a depth gap also drops
    // and reconnects this venue's fast-BBO session. Splitting them needs two
    // io_contexts per provider - not done.
    void RequestResync();

    // Runs `fn` on this provider's io_context thread. Lets a subclass
    // marshal work back from a helper thread (e.g. a REST snapshot fetch)
    // so state touched by message handlers needs no locking.
    void PostToIoContext(std::function<void()> fn);

    // Called on the worker thread just before (re)creating the sessions, so
    // a subclass can drop per-connection state - sync progress, sequence
    // numbers, buffered events. Default: nothing.
    //
    // Required for any venue whose stream does NOT re-send a snapshot on
    // reconnect (Binance): without it, stale sync state survives a resync
    // and the first event on the new connection looks like a continuity
    // violation, resyncing forever. Bybit/OKX self-heal via their
    // in-channel snapshot.
    virtual void OnReconnect() {}

    // Get current timestamp in milliseconds
    static int64_t GetCurrentTimeMs();

    const ProviderConfig config;
    std::atomic<bool> running;

   private:
    void Run();
    void HandleReconnection();

    // Build one redundant socket for a stream and start it. The index is
    // captured in the message lambda, which is how a message arrives with the
    // conn_index that SeqDedup needs.
    //
    // Also decides suppress_next_reset: true when another socket on this
    // stream is already live, so this one's opening snapshot is ignored
    // rather than resetting a book that is already correct. At cold start
    // this falls out on its own - socket 0 sees a live count of zero and
    // seeds the book, sockets 1..N-1 see it non-zero and stay quiet.
    void CreateDepthSession(uint32_t index);
    void CreateBboSession(uint32_t index);

    // Fired by the session itself when it dies for a reason we did not ask
    // for. Runs on the io_context thread.
    void OnDepthSessionClosed(uint32_t index);
    void OnBboSessionClosed(uint32_t index);

    static constexpr uint32_t MAX_RECONNECT_ATTEMPTS = 10;
    static constexpr uint64_t INITIAL_RECONNECT_DELAY_MS = 1000;  // 1 second
    static constexpr uint64_t MAX_RECONNECT_DELAY_MS = 60000;     // 60 seconds
    // Short, fixed delay after a gap resync - enough to avoid a tight
    // reconnect loop, short enough that the book is back quickly.
    // TODO: §4.2 also calls for hysteresis so a flapping feed can't
    // resync-storm; not implemented.
    static constexpr uint64_t RESYNC_DELAY_MS = 200;

    // One redundant connection on one stream.
    //
    // Deliberately carries no per-socket dedup state. An earlier version
    // tracked "this socket reconnected while others were live, so ignore its
    // opening snapshot" - which was wrong: every socket is CREATED before any
    // CONNECTS, so creation order says nothing about who delivers first, and
    // a late socket's stale snapshot still dragged the high-water mark
    // backwards. "Is this snapshot newer than what I hold?" is a property of
    // the DATA, and SeqDedup's `<=` rule already answers it.
    struct SessionSlot {
        WebSocketSessionSSLPtr session;
    };

    std::thread worker_thread;
    net::io_context ioc;
    ssl::context ssl_ctx;

    // Two streams per venue - depth and fast-BBO - each with config.connections
    // redundant sockets. They are NEVER merged: the two streams carry separate
    // sequence numbers and are not ordered against each other (§7).
    std::vector<SessionSlot> depth_sessions_;
    std::vector<SessionSlot> bbo_sessions_;

    // Duplicate rejection, one per stream for the same reason. Binance depth
    // `u` and bookTicker `u` are different id spaces; comparing across them
    // would be meaningless.
    SeqDedup depth_dedup_;
    SeqDedup bbo_dedup_;

    // Sockets created and not yet reported closed, per stream.
    //
    // KEY: OnReconnect() fires on a transition to ZERO, not when a socket
    // dies. It exists to discard sync state we can no longer trust after a
    // disconnect - but if even one socket stayed up we missed nothing, and
    // clearing would throw away a valid book and force a needless REST
    // snapshot.
    uint32_t depth_live_ = 0;
    uint32_t bbo_live_ = 0;

    uint32_t reconnect_count;
    // Set by RequestResync() from the io_context thread, read by Run() on
    // the worker thread after ioc.run() returns - hence atomic.
    std::atomic<bool> resync_requested_{false};
    CallBack callback_;
    QuoteCallBack quote_callback_;
};

}  // namespace market_data
