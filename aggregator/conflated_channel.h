#pragma once

#include <condition_variable>
#include <mutex>
#include <optional>

namespace market_data {

// Depth-1 pending slot with overwrite semantics (DESIGN_1 §7.4): if a new
// value arrives while the previous one hasn't been read yet, it replaces
// it - nothing ever queues, nothing upstream ever blocks on a slow reader.
template <typename T>
class ConflatedChannel {
   public:
    // Producer side (e.g. Core, after computing a new ConsolidatedBBO).
    // Never blocks.
    void Push(T value) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_ = std::move(value);
        }
        cv_.notify_one();
    }

    // Consumer side (one gRPC session's handler thread). Blocks until a
    // value is available, then returns it and clears the slot. Returns
    // std::nullopt only if Close() was called while waiting.
    std::optional<T> WaitAndTake() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return pending_.has_value() || closed_; });
        if (!pending_) {
            return std::nullopt;  // closed_, nothing pending
        }
        T value = std::move(*pending_);
        pending_.reset();
        return value;
    }

    // Wakes any thread blocked in WaitAndTake() so it can exit - used when
    // a session is being torn down (server shutdown, forced disconnect).
    void Close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        cv_.notify_one();
    }

   private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::optional<T> pending_;
    bool closed_ = false;
};

}  // namespace market_data
