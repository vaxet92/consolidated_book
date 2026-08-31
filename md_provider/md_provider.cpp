#include "md_provider.h"
#include "logger/logger.h"
#include <root_certificates.hpp>

using namespace market_data;

Provider::Provider(const ProviderConfig& config, CallBack callback, QuoteCallBack quote_callback)
    : config(config),
      running(false),
      ssl_ctx(ssl::context::tlsv12_client),
      reconnect_count(0),
      callback_(std::move(callback)),
      quote_callback_(std::move(quote_callback)) {
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

    // Stop() marks each session as deliberately closed, which also silences
    // its on-closed callback - otherwise every socket would ask to be
    // reconnected while we are trying to exit.
    for (auto& slot : depth_sessions_) {
        if (slot.session) slot.session->Stop();
    }
    for (auto& slot : bbo_sessions_) {
        if (slot.session) slot.session->Stop();
    }

    ioc.stop();

    if (worker_thread.joinable()) {
        worker_thread.join();
    }
}

void Provider::Run() {
    while (running) {
        try {
            // Let the subclass drop per-connection state before the new
            // sessions start delivering messages.
            OnReconnect();

            // Reset io_context for a new run
            ioc.restart();

            // Full rebuild after a total outage or a resync. The filters
            // start fresh: the venue's ids keep climbing, so carrying the old
            // high-water mark would also work, but a clean filter means
            // socket 0's opening snapshot is unconditionally taken and there
            // is no cross-outage state left to reason about.
            depth_dedup_ = SeqDedup{};
            bbo_dedup_ = SeqDedup{};
            depth_sessions_.clear();
            bbo_sessions_.clear();
            depth_live_ = 0;
            bbo_live_ = 0;

            // config.connections redundant sockets per stream. At the default
            // of 1 this is exactly the previous single-session behaviour.
            const uint32_t connections = std::max<uint32_t>(1, config.connections);

            Logger::Log(LogLevel::kInfo, "[{}] Connecting to {} ({} connection(s) per stream)...",
                        VenueConverter::ToVenueString(config.venue_id), GetHost(), connections);

            depth_sessions_.resize(connections);
            bbo_sessions_.resize(connections);
            for (uint32_t i = 0; i < connections; ++i) {
                CreateDepthSession(i);
                CreateBboSession(i);
            }

            // Run the io_context - drives every session on this one thread,
            // which is why none of the per-session state needs a lock.
            //
            // KEY: this returns only when NO work remains, i.e. every socket
            // is gone. One socket dying leaves the others running, so a
            // partial failure never reaches the teardown below - which is
            // precisely the point of redundancy.
            ioc.run();

            if (running) {
                if (resync_requested_.exchange(false)) {
                    // Deliberate resync after a depth gap - NOT a connection
                    // failure, so it must not consume the reconnect-attempt
                    // budget that permanently stops the provider.
                    Logger::Log(LogLevel::kInfo, "[{}] Resyncing after depth gap",
                                VenueConverter::ToVenueString(config.venue_id));
                    std::this_thread::sleep_for(std::chrono::milliseconds(RESYNC_DELAY_MS));
                } else {
                    // Connection dropped, attempt reconnection
                    HandleReconnection();
                }
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

void Provider::CreateDepthSession(uint32_t index) {
    SessionSlot& slot = depth_sessions_[index];
    ++depth_live_;

    slot.session = std::make_shared<WebSocketSessionSSL>(
        ioc, ssl_ctx, [this, index](const std::string& msg) { this->OnDepthMessage(msg, index); });
    slot.session->SetOnClosed([this, index]() { this->OnDepthSessionClosed(index); });
    slot.session->Run(GetHost(), GetPort(), GetDepthPath(), DepthSubscriptionMessage());
}

void Provider::CreateBboSession(uint32_t index) {
    SessionSlot& slot = bbo_sessions_[index];

    // No reset suppression needed: the fast-BBO streams push stateless
    // top-of-book quotes and never send a snapshot.
    ++bbo_live_;

    slot.session = std::make_shared<WebSocketSessionSSL>(
        ioc, ssl_ctx, [this, index](const std::string& msg) { this->OnBboMessage(msg, index); });
    slot.session->SetOnClosed([this, index]() { this->OnBboSessionClosed(index); });
    slot.session->Run(GetHost(), GetPort(), GetBboPath(), BboSubscriptionMessage());
}

void Provider::OnDepthSessionClosed(uint32_t index) {
    if (depth_live_ > 0) {
        --depth_live_;
    }

    // Logged 1-based: `index` is a vector subscript and a bitmask shift, but
    // an operator reading "connection 1 of 3" should not have to know that.
    Logger::Log(LogLevel::kWarning, "[{}] depth connection {}/{} closed, {} still live",
                VenueConverter::ToVenueString(config.venue_id), index + 1, depth_sessions_.size(), depth_live_);

    // KEY: only a TOTAL outage invalidates sync state. OnReconnect() exists
    // to discard a book we can no longer trust because we do not know what we
    // missed - but if even one socket stayed up, we missed nothing, and
    // clearing would throw away a correct book and force a needless REST
    // snapshot.
    if (depth_live_ == 0) {
        Logger::Log(LogLevel::kError, "[{}] all depth connections down",
                    VenueConverter::ToVenueString(config.venue_id));
        OnReconnect();
    }
}

void Provider::OnBboSessionClosed(uint32_t index) {
    if (bbo_live_ > 0) {
        --bbo_live_;
    }

    Logger::Log(LogLevel::kWarning, "[{}] bbo connection {}/{} closed, {} still live",
                VenueConverter::ToVenueString(config.venue_id), index + 1, bbo_sessions_.size(), bbo_live_);
}

bool Provider::AcceptDepth(uint64_t id, uint32_t conn_index, bool venue_reset) {
    // No per-socket adjustment: venue_reset already means "the id moves
    // backwards", and an ordinary snapshot passes false so the `<=` rule
    // decides on the data alone.
    if (!depth_dedup_.Accept(id, conn_index, venue_reset)) {
        // Logged once per episode, exactly at the threshold. A stuck filter
        // drops EVERY message, so logging each one would bury the alarm in
        // its own noise.
        if (depth_dedup_.ConsecutiveDrops() == SeqDedup::kSuspiciousDropStreak) {
            Logger::Log(LogLevel::kError, "[{}] depth dedup dropped {} in a row - missed a sequence reset?",
                        VenueConverter::ToVenueString(config.venue_id), SeqDedup::kSuspiciousDropStreak);
        }
        return false;
    }
    return true;
}

bool Provider::AcceptBbo(uint64_t id, uint32_t conn_index) {
    if (!bbo_dedup_.Accept(id, conn_index, /*is_reset=*/false)) {
        if (bbo_dedup_.ConsecutiveDrops() == SeqDedup::kSuspiciousDropStreak) {
            Logger::Log(LogLevel::kError, "[{}] bbo dedup dropped {} in a row - non-monotonic quote ids?",
                        VenueConverter::ToVenueString(config.venue_id), SeqDedup::kSuspiciousDropStreak);
        }
        return false;
    }
    return true;
}

void Provider::Emit(const BookUpdate& update) {
    if (callback_) {
        callback_(update);
    }
}

void Provider::EmitQuote(const BboQuote& quote) {
    if (quote_callback_) {
        quote_callback_(quote);
    }
}

void Provider::PostToIoContext(std::function<void()> fn) {
    net::post(ioc, std::move(fn));
}

void Provider::RequestResync() {
    resync_requested_ = true;
    // Every socket on both streams goes down: a gap means the book is WRONG,
    // and no surviving connection can repair that - they all carry the same
    // broken sequence. This is the one case where redundancy does not help.
    for (auto& slot : depth_sessions_) {
        if (slot.session) slot.session->Stop();
    }
    for (auto& slot : bbo_sessions_) {
        if (slot.session) slot.session->Stop();
    }
    ioc.stop();
}

int64_t Provider::GetCurrentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}