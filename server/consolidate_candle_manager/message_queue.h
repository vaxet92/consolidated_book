#pragma once

#include "types/trade.h"

//------------------------------------------------------------------------------
// MPSCQueue
// https://github.com/grivet/mpsc-queue
//
// This implementation is based on Dmitry Vyukov’s bounded MPSC queue algorithm.
// It uses a fixed-size ring buffer with two atomic counters (head for producers and
// tail for the single consumer) and a per-slot sequence number to synchronize access.
//
// Key properties:
//   - Multiple producers can push concurrently.
//   - A single consumer can pop items.
//   - The capacity must be a power of two for efficient index wrapping.
//   - Memory ordering (acquire/release) ensures correct synchronization.
//------------------------------------------------------------------------------

// Capacity must be a power of 2 for efficient modulo operations.
template <typename T = String, size_t Capacity = 4096>
class MPSCQueue {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of 2");

   public:
    MPSCQueue() : enqueuePos(0), dequeuePos(0) {
        for (size_t i = 0; i < Capacity; ++i) {
            buffer[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    // Multiple producers can call push concurrently.
    // Returns false if the queue is full.
    bool Push(const T& value) noexcept { return push_impl(value); }

    bool Push(T&& value) noexcept { return push_impl(std::move(value)); }

    // Single consumer calls pop.
    // Returns std::nullopt if the queue is empty.
    std::optional<T> Pop() noexcept {
        Cell* cell;
        size_t pos = dequeuePos.load(std::memory_order_relaxed);
        cell = &buffer[pos & (Capacity - 1)];
        size_t seq = cell->sequence.load(std::memory_order_acquire);
        intptr_t diff = (intptr_t)seq - (intptr_t)(pos + 1);
        if (diff != 0) {
            // Queue is empty.
            return std::nullopt;
        }
        T result = std::move(cell->data);
        // Mark the cell as available for reuse.
        cell->sequence.store(pos + Capacity, std::memory_order_release);
        dequeuePos.store(pos + 1, std::memory_order_relaxed);
        return result;
    }

   private:
    // Each slot in the buffer is a Cell with a sequence number and data.
    struct Cell {
        std::atomic<size_t> sequence;
        T data;
    };

    // Internal push implementation that works for both lvalue and rvalue.
    template <typename U>
    bool push_impl(U&& value) noexcept {
        Cell* cell;
        size_t pos = enqueuePos.load(std::memory_order_relaxed);
        while (true) {
            cell = &buffer[pos & (Capacity - 1)];
            size_t seq = cell->sequence.load(std::memory_order_acquire);
            intptr_t diff = (intptr_t)seq - (intptr_t)pos;
            if (diff == 0) {
                // Try to claim the slot by incrementing enqueuePos.
                if (enqueuePos.compare_exchange_weak(pos, pos + 1, std::memory_order_acq_rel, std::memory_order_relaxed)) {
                    break;  // Slot successfully claimed.
                }
            } else if (diff < 0) {
                // Queue is full.
                return false;
            } else {
                pos = enqueuePos.load(std::memory_order_relaxed);
            }
        }
        // Write the data into the claimed slot.
        cell->data = std::forward<U>(value);
        // Publish the data by setting sequence to pos + 1.
        cell->sequence.store(pos + 1, std::memory_order_release);
        return true;
    }

    // Atomic counters for enqueue (head) and dequeue (tail) positions.
    alignas(64) std::atomic<size_t> enqueuePos;
    alignas(64) std::atomic<size_t> dequeuePos;
    // Fixed-size circular buffer.
    std::array<Cell, Capacity> buffer;
};
