#include "consolidate_candle_manager.h"
#include <iostream>

ConsolidateCandleManager::ConsolidateCandleManager(
    MessagePipeManager* pipe_manager,
    int32_t interval_ms,
    const std::vector<std::string>& instruments)
    : pipe_manager(pipe_manager),
      interval_ms(interval_ms),
      instruments(instruments),
      running(false),
      trades_processed(0),
      candles_emitted(0) {
    
    // Pre-create builders for known instruments
    for (const auto& instrument : instruments) {
        builders[instrument] = std::make_unique<CandleBuilder>(instrument, interval_ms);
    }
}

ConsolidateCandleManager::~ConsolidateCandleManager() {
    Stop();
}

void ConsolidateCandleManager::SetCandleCallback(CandleCallback callback) {
    candle_callback = std::move(callback);
}

void ConsolidateCandleManager::Start() {
    if (running.exchange(true)) {
        return;  // Already running
    }
    
    std::cout << "[CandleManager] Starting..." << std::endl;
    
    worker_thread = std::thread([this]() {
        this->Run();
    });
}

void ConsolidateCandleManager::Stop() {
    if (!running.exchange(false)) {
        return;  // Already stopped
    }
    
    std::cout << "[CandleManager] Stopping..." << std::endl;
    
    if (worker_thread.joinable()) {
        worker_thread.join();
    }
    
    // Emit final candles for all instruments
    for (auto& [instrument, builder] : builders) {
        auto final_candle = builder->GetCurrentCandle();
        if (final_candle && candle_callback) {
            candle_callback(*final_candle);
            candles_emitted++;
        }
    }
    
    std::cout << "[CandleManager] Stopped. Stats: " 
              << trades_processed << " trades processed, "
              << candles_emitted << " candles emitted" << std::endl;
}

void ConsolidateCandleManager::Run() {
    std::cout << "[CandleManager] Consumer thread started" << std::endl;
    
    while (running) {
        // Pop trade from the pipe (non-blocking)
        auto trade_opt = pipe_manager->PopTrade();
        
        if (trade_opt) {
            ProcessTrade(*trade_opt);
        } else {
            // Queue is empty, sleep briefly to avoid busy-waiting
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }
    
    std::cout << "[CandleManager] Consumer thread stopped" << std::endl;
}

void ConsolidateCandleManager::ProcessTrade(const Trade& trade) {
    trades_processed++;
    
    // Get or create builder for this instrument
    CandleBuilder* builder = GetOrCreateBuilder(trade.instrument);
    
    if (!builder) {
        std::cerr << "[CandleManager] Failed to get builder for " << trade.instrument << std::endl;
        return;
    }
    
    // Process the trade and check if a candle was completed
    auto completed_candle = builder->ProcessTrade(trade);
    
    if (completed_candle && candle_callback) {
        candle_callback(*completed_candle);
        candles_emitted++;
    }
}

CandleBuilder* ConsolidateCandleManager::GetOrCreateBuilder(const std::string& instrument) {
    auto it = builders.find(instrument);
    
    if (it != builders.end()) {
        return it->second.get();
    }
    
    // Create new builder for this instrument
    std::cout << "[CandleManager] Creating builder for new instrument: " << instrument << std::endl;
    auto builder = std::make_unique<CandleBuilder>(instrument, interval_ms);
    CandleBuilder* builder_ptr = builder.get();
    builders[instrument] = std::move(builder);
    
    return builder_ptr;
}
