#include "hash_manager.h"

/*
 * HashIDManager Memory Management
 * ============================
 *
 * Memory Cleanup Process:
 * ---------------------
 * 1. Main hash set is cleared after MAX_HASH_SIZE messages
 * 2. Recent messages from backup array are preserved
 * 3. Ring buffer maintains last N messages (N = num connections)
 *
 * Ring Buffer Operation:
 * -------------------
 * - Fixed size array (size = num connections)
 * - Circular write pattern
 * - Oldest entries automatically overwritten
 */

void HashIDManager::CheckBackUp() {
    if (++counter < MAX_HASH_SIZE) {
        return;  // Not yet time to cleanup
    }

    // Reset main hash memory and restore from backup
    hashMemory = {};
    hashMemory.insert(backupArr.begin(), backupArr.end());
    counter = 0;
}

/*
    handle ring buffer for backup container
*/
void HashIDManager::UpdateBackUp(const uint64_t hash) {
    // Circular buffer update
    backUpIndex = (backUpIndex + 1) % backupArrCapacity;
    backupArr[backUpIndex] = hash;
}

void HashIDManager::Init(const uint32_t size) {
    backupArrCapacity = size;
    backupArr.resize(backupArrCapacity);
    memset(backupArr.data(), 0, backupArrCapacity);
}
/*
    calculate new hash, check unique status and update backup container
*/
bool HashIDManager::UpdateHash(const uint64_t hash) {
    // Try to insert hash
    auto [_, isNew] = hashMemory.insert(hash);

    if (isNew) {
        // New hash - update backup and check for cleanup
        UpdateBackUp(hash);
        CheckBackUp();
    }

    return isNew;  // Return true if new, false if duplicate
}
