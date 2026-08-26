#include "md_provider.h"
#include "logger/logger.h"
#include <root_certificates.hpp>

using namespace market_data;

Provider::Provider(const ProviderConfig& config, CallBack callback)
    : config(config),
      running(false),
      ssl_ctx(ssl::context::tlsv12_client),
      reconnect_count(0),
      callback_(std::move(callback)) {
    // Load root certificates for SSL
    load_root_certificates(ssl_ctx);
    ssl_ctx.set_verify_mode(ssl::verify_peer);
}

Provider::~Provider() {
    Stop();
}

void Provider::Start() {
    if (running.exchange(true)) {
        return;  // Already running
    }

    Logger::Log(LogLevel::kInfo, "[{}] Starting provider...", VenueConverter::ToVenueString(config.venue_id));

    worker_thread = std::thread([this]() { this->Run(); });
}

void Provider::Stop() {
    if (!running.exchange(false)) {
        return;  // Already stopped
    }

    Logger::Log(LogLevel::kInfo, "[{}] Stopping provider...", VenueConverter::ToVenueString(config.venue_id));

    if (depth_session_) {
        depth_session_->Stop();
    }
    if (bbo_session_) {
        bbo_session_->Stop();
    }

    ioc.stop();

    if (worker_thread.joinable()) {
        worker_thread.join();
    }
}

void Provider::Run() {
    while (running) {
        try {
            // Reset io_context for a new run
            ioc.restart();

            // Depth session: the real book.
            depth_session_ = std::make_shared<WebSocketSessionSSL>(
                ioc, ssl_ctx, [this](const std::string& msg) { this->OnDepthMessage(msg); });

            // Fast-BBO session: correctness oracle only (§4.4).
            bbo_session_ = std::make_shared<WebSocketSessionSSL>(
                ioc, ssl_ctx, [this](const std::string& msg) { this->OnBboMessage(msg); });

            Logger::Log(LogLevel::kInfo, "[{}] Connecting to {}...", VenueConverter::ToVenueString(config.venue_id),
                        GetHost());

            depth_session_->Run(GetHost(), GetPort(), GetDepthPath());
            bbo_session_->Run(GetHost(), GetPort(), GetBboPath());

            // Run the io_context - drives both sessions on this one thread.
            ioc.run();

            if (running) {
                // Connection dropped, attempt reconnection
                HandleReconnection();
            }

        } catch (const std::exception& e) {
            Logger::Log(LogLevel::kError, "[{}] Error: {}", VenueConverter::ToVenueString(config.venue_id), e.what());

            if (running) {
                HandleReconnection();
            }
        }
    }

    Logger::Log(LogLevel::kInfo, "[{}] Provider stopped", VenueConverter::ToVenueString(config.venue_id));
}

void Provider::HandleReconnection() {
    if (!running) return;

    if (++reconnect_count > MAX_RECONNECT_ATTEMPTS) {
        Logger::Log(LogLevel::kError, "[{}] Max reconnect attempts reached. Stopping.",
                    VenueConverter::ToVenueString(config.venue_id));
        running = false;
        return;
    }

    // Exponential backoff with cap
    uint64_t delay_ms = std::min(INITIAL_RECONNECT_DELAY_MS * (1ULL << (reconnect_count - 1)), MAX_RECONNECT_DELAY_MS);

    Logger::Log(LogLevel::kWarning, "[{}] Reconnecting in {}ms (attempt {})",
                VenueConverter::ToVenueString(config.venue_id), delay_ms, reconnect_count);

    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
}

void Provider::Emit(const BookUpdate& update) {
    if (callback_) {
        callback_(update);
    }
}

int64_t Provider::GetCurrentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}