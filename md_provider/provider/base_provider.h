#pragma once

#include "../ws/ws.h"
#include "../../types/trade.h"
#include "../../pipe_manager/message_pipe_manager.h"
#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <thread>
#include <atomic>
#include <memory>
#include <chrono>
#include <string>

struct ProviderConfig {
    std::string> exchange_names;
    std::string exchange_endpoint;
    std::vector<std::string> instruments;
    MessagePipeManager* pipe_manager;
    uint32_t candle_timeframe_ms;
};

    class BaseProvider {
public:
    explicit BaseProvider(const ProviderConfig& config);
    virtual ~BaseProvider();
    
    // Start the provider (in a separate thread)
    void Start();
    // Stop the provider
    void Stop();
    
protected:
    // Pure virtual: child classes must implement message parsing
    virtual void OnMessage(const std::string& message) = 0;
    
    // Pure virtual: child classes must provide subscription message
    virtual std::string SubscriptionMessage() const = 0;
    virtual std::string UnsubscriptionMessage() const = 0;
    
    // Get current timestamp in milliseconds
    static int64_t GetCurrentTimeMs();
    
    const ProviderConfig config;
    std::atomic<bool> running;
    
private:
    
    static constexpr uint32_t MAX_RECONNECT_ATTEMPTS = 10;
    static constexpr uint64_t INITIAL_RECONNECT_DELAY_MS = 1000;  // 1 second
    static constexpr uint64_t MAX_RECONNECT_DELAY_MS = 60000;     // 60 seconds
    
    std::thread worker_thread;
    net::io_context ioc;
    ssl::context ssl_ctx;
    WebSocketSessionSSLPtr ws_session;
    CandleManagerPtr candle_manager;
    uint32_t reconnect_count;
};
