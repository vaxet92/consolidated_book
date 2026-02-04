#pragma once

#include "candle_builder.h"
#include "../types/trade.h"
#include "../types/candle.h"
#include "../pipe_manager/message_pipe_manager.h"
#include <unordered_map>
#include <memory>
#include <thread>
#include <atomic>
#include <functional>
#include <vector>

class ConsolidateCandleManager {
   public:
    ConsolidateCandleManager(MessagePipeManager* pipe_manage int32_t interval_ms, const std::vector<std::string>& instruments);
    ~ConsolidateCandleManager();

    // Set callback for when a candle is completed
    void SetCandleCallback(CandleCallback callback);

    // Start consuming trades from the pipe
    void Start();

    // Stop the manager
    void Stop();

   private:
    void Run();
    void ProcessTrade(const Trade& trade);
    CandleBuilder* GetOrCreateBuilder(const std::string& instrument);

    MessagePipeManager* pipe_manager;
    const int32_t interval_ms;
    std::vector<std::string> instruments;

    // Map: instrument -> CandleBuilder
    std::unordered_map<std::string, std::unique_ptr<CandleBuilder>> builders;

    OrderBook orderBook;
    std::unique_ptr<MPSCQueue<String>> MPSCQueuePtr;
    std::atomic<bool> stopFlag;
    std::thread worker;
};
