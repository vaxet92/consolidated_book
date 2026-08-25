#include "md_provider.h"
#include <iostream>
#include <thread>
#include <root_certificates.hpp>

MDProvider::MDProvider(const ProviderConfig& config)
    : config(config), running(false), ssl_ctx(ssl::context::tlsv12_client), reconnect_count(0) {
    // Load root certificates for SSL
    load_root_certificates(ssl_ctx);
    ssl_ctx.set_verify_mode(ssl::verify_peer);
}

MDProvider::~MDProvider() {
    Stop();
}

void MDProvider::Start() {
    if (running.exchange(true)) {
        return;  // Already running
    }

    std::cout << "[" << config.exchange_name << "] Starting provider..." << std::endl;

    worker_thread = std::thread([this]() { this->Run(); });
}

void MDProvider::Stop() {
    if (!running.exchange(false)) {
        return;  // Already stopped
    }

    std::cout << "[" << config.exchange_name << "] Stopping provider..." << std::endl;

    if (ws_session) {
        ws_session->Stop();
    }

    ioc.stop();

    if (worker_thread.joinable()) {
        worker_thread.join();
    }
}

void MDProvider::Run() {
    while (running) {
        try {
            // Reset io_context for a new run
            ioc.restart();

            // Create WebSocket session
            ws_session = std::make_shared<WebSocketSessionSSL>(
                ioc, ssl_ctx, [this](const std::string& msg) { this->OnMessage(msg); });

            std::cout << "[" << config.exchange_name << "] Connecting to " << GetHost() << "..." << std::endl;

            // Start the WebSocket connection
            ws_session->Run(GetHost(), GetPort(), GetPath());

            // Run the io_context
            ioc.run();

            if (running) {
                // Connection dropped, attempt reconnection
                HandleReconnection();
            }

        } catch (const std::exception& e) {
            std::cerr << "[" << config.exchange_name << "] Error: " << e.what() << std::endl;

            if (running) {
                HandleReconnection();
            }
        }
    }

    std::cout << "[" << config.exchange_name << "] Provider stopped" << std::endl;
}

void MDProvider::HandleReconnection() {
    if (!running) return;

    reconnect_count++;

    if (reconnect_count > MAX_RECONNECT_ATTEMPTS) {
        std::cerr << "[" << config.exchange_name << "] Max reconnect attempts reached. Stopping." << std::endl;
        running = false;
        return;
    }

    // Exponential backoff with cap
    uint64_t delay_ms = std::min(INITIAL_RECONNECT_DELAY_MS * (1ULL << (reconnect_count - 1)), MAX_RECONNECT_DELAY_MS);

    std::cout << "[" << config.exchange_name << "] Reconnecting in " << delay_ms << "ms (attempt " << reconnect_count
              << ")" << std::endl;

    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
}

void MDProvider::PushTrade(Trade&& trade) {
    if (config.pipe_manager) {
        if (!config.pipe_manager->PushTrade(std::move(trade))) {
            std::cerr << "[" << config.exchange_name << "] Warning: Trade dropped (queue full)" << std::endl;
        }
    }
}

int64_t MDProvider::GetCurrentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}
