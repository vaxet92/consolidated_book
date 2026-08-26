#pragma once

#include "ws/ws.h"
#include "types/venue.h"
#include "md_core/types.h"
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
};

class Provider {
   public:
    using CallBack = std::function<void(const BookUpdate&)>;

    explicit Provider(const ProviderConfig& config, CallBack callback);
    virtual ~Provider();

    // Start the provider (in a separate thread)
    void Start();
    // Stop the provider
    void Stop();

   protected:
    // Depth stream: the real book. Venue-specific parsing.
    virtual void OnDepthMessage(const std::string& message) = 0;
    virtual std::string DepthSubscriptionMessage() const = 0;
    virtual const char* GetHost() const = 0;
    virtual const char* GetPort() const = 0;
    virtual const char* GetDepthPath() const = 0;

    // Fast-BBO stream: correctness oracle only (DESIGN_1 §4.4) - never
    // published through the CallBack. The >200ms-disagreement resync check
    // itself is not implemented yet (TODO) - this only gets the stream
    // connected and routed so that logic has somewhere to live.
    virtual void OnBboMessage(const std::string& message) = 0;
    virtual std::string BboSubscriptionMessage() const = 0;
    virtual const char* GetBboPath() const = 0;

    // Called by child classes with a depth-derived BookUpdate.
    // Runs on this provider's own thread - must become a queue push instead
    // of a direct call once providers run concurrently with the
    // consolidator (see our discussion on CallBack being a temporary shape).
    void Emit(const BookUpdate& update);

    // Get current timestamp in milliseconds
    static int64_t GetCurrentTimeMs();

    const ProviderConfig config;
    std::atomic<bool> running;

   private:
    void Run();
    void HandleReconnection();

    static constexpr uint32_t MAX_RECONNECT_ATTEMPTS = 10;
    static constexpr uint64_t INITIAL_RECONNECT_DELAY_MS = 1000;  // 1 second
    static constexpr uint64_t MAX_RECONNECT_DELAY_MS = 60000;     // 60 seconds

    std::thread worker_thread;
    net::io_context ioc;
    ssl::context ssl_ctx;
    WebSocketSessionSSLPtr depth_session_;
    WebSocketSessionSSLPtr bbo_session_;
    uint32_t reconnect_count;
    CallBack callback_;
};

}  // namespace market_data
