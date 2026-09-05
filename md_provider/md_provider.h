#pragma once

#include "ws/ws.h"
#include "seq_dedup.h"
#include "types/venue.h"
#include "md_core/types.h"
#include "md_core/venue_health.h"
#include <vector>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <thread>
#include <atomic>
#include <memory>
#include <chrono>
#include <string>
#include <functional>

namespace market_data {

struct ProviderConfig {
    VenueId venue_id;
    InstrumentKey instrument;  // spot only for now
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

    // How long this venue's stream may stay silent before it is called stale
    // (DESIGN_1 §6.2c). Per venue AND per stream, because the two are not
    // comparable: Bybit republishes L1 with the same `u` after 3s of no
    // change, while OKX only promises seqId == prevSeqId after ~60s.
    //
    // KEY: these are BACKSTOPS, not detectors. Where the venue documents a
    // keepalive the value is derived from it and silence past it is real
    // evidence. Where it does not - Binance publishes no keepalive at all -
    // silence carries no information, and a long backstop is the honest
    // choice: cross-venue corroboration (§6.2b signal 3) is what catches a
    // dead Binance feed quickly, not this number.
    //
    // The defaults here are deliberately generous. Set them per venue at
    // construction; a value too SHORT is the dangerous direction, because it
    // marks a healthy but quiet feed stale and flaps it in and out of the
    // merge - worse than no watchdog at all.
    int64_t depth_backstop_ns = 90'000'000'000;  // 90s - sized for OKX's ~60s keepalive
    int64_t bbo_backstop_ns = 30'000'000'000;    // 30s
};

class Provider {
   public:
    // Takes an rvalue: the update is HANDED OVER, not shown. Core moves it
    // into its queue, and a BookUpdate owns two vectors - passing by const&
    // would copy them on every message, which is the allocation the whole
    // queue design exists to avoid.
    using CallBack = std::function<void(BookUpdate&&)>;
    using QuoteCallBack = std::function<void(const BboQuote&)>;

    // Delivered whenever this venue's verdict for one of its streams CHANGES
    // (DESIGN_1 §6.5). Fires on the provider's io_context thread, in the same
    // call sequence as Emit()/EmitQuote() - so a health event is ordered
    // against this venue's book updates, which is the property that makes it
    // safe. When the per-venue SPSC queues land it becomes a push onto the
    // same queue and this ordering is preserved rather than created.
    using HealthCallback = std::function<void(const VenueHealthEvent&)>;

    explicit Provider(const ProviderConfig& config, CallBack callback, QuoteCallBack quote_callback = nullptr);
    virtual ~Provider();

    // A setter rather than a constructor parameter, unlike the other two
    // callbacks - only so this step does not have to touch all three venue
    // subclasses' constructors. Must be called before Start().
    void SetHealthCallback(HealthCallback callback) { health_callback_ = std::move(callback); }

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

    // "The feed spoke." Called by a child class immediately after a message
    // parses as a channel message for our subscription, and BEFORE
    // AcceptDepth/AcceptBbo.
    //
    // KEY: the position is the whole point. Two of the three venues prove
    // liveness by REPEATING an id - Bybit republishes L1 with the same `u`
    // after 3s of no change, OKX sends seqId == prevSeqId after ~60s. Both
    // carry an id we have already seen, so SeqDedup drops them, and the
    // kIgnore branches in CheckBybitContinuity/CheckOkxContinuity that
    // document these keepalives are unreachable behind it. Stamping here, in
    // front of the filter, is what keeps that signal.
    //
    // A redundant connection's DUPLICATE also lands here, and that is
    // correct: for liveness a duplicate and a keepalive are the same event -
    // the venue put bytes on the wire. Distinguishing them would need to
    // count copies against the connection count, which breaks precisely when
    // a connection dies - the moment the signal is needed most.
    //
    // Protocol frames (ping, pong, subscribe ack) never reach here, because
    // the parser rejects them before this point. A heartbeat proves the
    // socket is open, not that the data is flowing, and must not count.
    // KEY: the kNoData promotion is not an optimization, it closes a startup
    // hole. Core starts every venue at kNoData and admits nothing until told
    // otherwise (fail-safe). The watchdog only ticks once a second, so
    // without this the first second of every run would have books arriving
    // but an EMPTY consolidated book published - a visible outage on every
    // start.
    //
    // Only kNoData is promoted here. Recovery from kStale deliberately waits
    // for the timer: that delay is the closest thing to hysteresis we have
    // until the real one is built, and it stops a feed that is barely alive
    // from flapping in and out of the merge on single messages. kNoData
    // happens exactly once per stream, so promoting it carries no such risk.
    //
    // Cost is one enum comparison per message, and the branch is taken once.
    void NoteDepthActivity() {
        last_depth_message_mono_ns_ = GetMonotonicNs();
        if (NeedsImmediatePromotion(last_depth_health_)) {
            PublishHealth(StreamKind::kDepth, VenueHealth::kLive);
        }
    }
    void NoteBboActivity() {
        last_bbo_message_mono_ns_ = GetMonotonicNs();
        if (NeedsImmediatePromotion(last_bbo_health_)) {
            PublishHealth(StreamKind::kBbo, VenueHealth::kLive);
        }
    }

    // The two states the watchdog will never clear on its own, so the first
    // message has to.
    //
    // kNoData: the watchdog would clear it a second later, and that second is
    // an empty published book on every startup.
    //
    // kResyncing: the watchdog deliberately leaves it alone (see CheckHealth),
    // so without this the venue would stay excluded forever after a gap.
    //
    // kStale is NOT here, on purpose. Recovery from stale waits for the next
    // tick, which is a crude confirmation delay standing in for the hysteresis
    // that is not built yet - it stops a barely-alive feed from flapping in
    // and out of the merge on single messages. kNoData and kResyncing carry no
    // such risk: both happen at a known point and are cleared once.
    static constexpr bool NeedsImmediatePromotion(VenueHealth health) {
        return health == VenueHealth::kNoData || health == VenueHealth::kResyncing;
    }

    // Monotonic clock, 0 = nothing has ever arrived on that stream.
    //
    // Deliberately separate from BookUpdate::recv_mono_ns, which answers a
    // different question: recv_mono_ns is when the BOOK last CHANGED, these
    // are when the FEED last SPOKE. During a quiet market the second keeps
    // advancing while the first does not, which is exactly the case a plain
    // watchdog gets wrong.
    int64_t last_depth_message_mono_ns() const { return last_depth_message_mono_ns_; }
    int64_t last_bbo_message_mono_ns() const { return last_bbo_message_mono_ns_; }

    // Called by child classes with a depth-derived BookUpdate.
    // Runs on this provider's own thread - must become a queue push instead
    // of a direct call once providers run concurrently with the
    // consolidator (see our discussion on CallBack being a temporary shape).
    //
    // Takes a NON-CONST reference because it stamps recv_mono_ns on the way
    // out. Stamping here rather than in each of the six message handlers
    // means no venue can forget it and no venue can do it differently. A
    // const& would force a copy of the update instead - two vectors, one
    // heap allocation per message, which is what §7.5 rules out.
    // Consumes `update` - the caller must not read it afterwards.
    void Emit(BookUpdate&& update);

    // Called by child classes with a top-of-book quote from the fast-BBO
    // stream. Same threading caveat and same stamping reason as Emit().
    void EmitQuote(BboQuote& quote);

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

    // Wall clock, in milliseconds. Feeds BookUpdate/BboQuote::recv_ts_ns,
    // which is compared against the venue's exch_ts_ns and goes on the wire.
    // Note the resolution is 1ms even though the field it fills is named _ns.
    static int64_t GetCurrentTimeMs();

    // KEY: steady_clock, NOT system_clock. Feeds recv_mono_ns and nothing
    // else. The staleness watchdog subtracts two of these, so the clock must
    // never jump: a wall-clock step backwards would blind the watchdog, and a
    // step forwards would mark every venue stale at once. Its epoch is
    // arbitrary and machine-local - only DIFFERENCES between two readings
    // mean anything, so never put this value on the wire or in a log where it
    // could be read as a time of day.
    static int64_t GetMonotonicNs();

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

    // How often the watchdog re-evaluates both streams.
    //
    // Must be well below the shortest backstop, or the check itself becomes
    // the detection delay: at a 10s backstop and a 10s interval, a dead feed
    // could go unnoticed for 20s. 1s against the shortest realistic backstop
    // (~10s, derived from Bybit's 3s L1 republish) gives ~10 chances to
    // notice, and costs three timer wakeups per second across all venues -
    // not a number worth optimizing.
    //
    // KEY: this timer is also what closes the TOTAL-OUTAGE hole. Health
    // evaluated only when an update arrives can never fire when every venue
    // goes silent - which is the case that matters most. The timer runs on
    // the provider's own io_context, so it fires whether or not data arrives,
    // with no extra thread.
    static constexpr uint64_t HEALTH_CHECK_INTERVAL_MS = 1000;

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

    // Declared AFTER ioc: members initialize in declaration order, and this
    // binds to ioc's executor at construction. ioc.restart() between reconnect
    // attempts does not invalidate that binding, so it is constructed once and
    // rearmed per run rather than rebuilt.
    net::steady_timer health_timer_;

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

    // Last verdict PUBLISHED for each stream, so the timer can fire only on a
    // change. Staleness is an edge, not a level: a venue's health is the same
    // on almost every tick, and re-sending it would turn a rare event into a
    // per-second stream Core has to filter.
    //
    // Both start at kNoData, which is the truth at construction and matches
    // what Core assumes before it has heard anything. So the first real
    // verdict - normally kLive once data starts flowing - is itself a change
    // and gets delivered.
    VenueHealth last_depth_health_ = VenueHealth::kNoData;
    VenueHealth last_bbo_health_ = VenueHealth::kNoData;

    // Set by NoteDepthActivity()/NoteBboActivity() - see those for why they
    // sit in front of the dedup filter rather than behind it.
    //
    // KEY: deliberately NOT atomic. Every access is on this provider's own
    // io_context thread - the message handlers write them, and the watchdog
    // timer that reads them is posted to the same io_context. Single thread,
    // no race, no synchronization needed.
    //
    // These were briefly atomic, under a design where Core pulled status
    // across threads to classify venues itself. DESIGN_1 §6.5 replaced that
    // with the provider deciding its own verdict and pushing it, so the
    // cross-thread read no longer exists. An atomic whose justification has
    // gone is worse than a plain member: it implies a sharing pattern that
    // is not there.
    int64_t last_depth_message_mono_ns_ = 0;
    int64_t last_bbo_message_mono_ns_ = 0;

    uint32_t reconnect_count;
    // Set by RequestResync() from the io_context thread, read by Run() on
    // the worker thread after ioc.run() returns - hence atomic.
    std::atomic<bool> resync_requested_{false};
    CallBack callback_;
    QuoteCallBack quote_callback_;
    HealthCallback health_callback_;

    // Rearms health_timer_ and schedules the next CheckHealth(). Called once
    // per run, from Run(), so the timer's lifetime matches the io_context's.
    void ScheduleHealthCheck();

    // Classifies both streams and publishes only what changed. Runs on the
    // io_context thread, which is also where the liveness stamps and the live
    // socket counts are written - so it needs no synchronization to read them.
    void CheckHealth();

    // Delivers one verdict to Core, and records it so the next tick can tell
    // whether anything changed.
    void PublishHealth(StreamKind stream, VenueHealth health);
};

}  // namespace market_data
