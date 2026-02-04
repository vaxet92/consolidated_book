#pragma once

#include "types.h"

/*
 * HashIDManager Implementation
 * ==========================
 * 
 * Purpose:
 * --------
 * Manages message sequence numbers to detect duplicates and ensure message ordering
 * in a websocket stream. Uses a ring buffer pattern for memory efficiency.
 *
 * Key Features:
 * ------------
 * 1. Memory Management:
 *    - Maintains fixed-size hash memory (MAX_HASH_SIZE)
 *    - Uses ring buffer for backup storage
 *    - Periodic cleanup to prevent memory growth
 *
 * 2. Duplicate Detection:
 *    - O(1) lookup using hash set
 *    - Preserves recent message history in backup array
 *
 * 3. Memory Recycling:
 *    - Resets hash memory after MAX_HASH_SIZE messages
 *    - Preserves most recent messages in backup array
 *    - Backup size equals number of websocket connections
 */

#define MAX_HASH_SIZE 120000u  // Reset hash memory after this many messages

using MemoryHashSet = std::unordered_set<uint64_t>;
using MemoryHashArray = std::vector<uint64_t>;

class HashIDManager {
   public:
    HashIDManager() = default;
    ~HashIDManager() = default;

    // Returns true if hash is new, false if duplicate
    bool UpdateHash(const uint64_t hash);
    
    // Initialize backup array size based on number of connections
    void Init(const uint32_t size);

   private:
    MemoryHashSet hashMemory;      // Main hash storage
    MemoryHashArray backupArr;     // Ring buffer for recent messages
    
    size_t backUpIndex{0};         // Current position in ring buffer
    size_t counter{0};             // Messages processed since last reset
    size_t backupArrCapacity{0};   // Size of ring buffer (= num connections)

    void CheckBackUp();            // Handles periodic memory cleanup
    void UpdateBackUp(const uint64_t hash);  // Updates ring buffer
};