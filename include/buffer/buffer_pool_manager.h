#pragma once
/**
 * =============================================================================
 * CourtDB - Buffer Pool Manager
 * =============================================================================
 *
 * Purpose:
 *   Manages a fixed-size pool of in-memory page frames, acting as a cache
 *   between the disk manager and higher-level components. All page accesses
 *   go through the buffer pool — no component reads/writes disk directly.
 *
 * Responsibilities:
 *   - Fetch pages from disk into memory frames
 *   - Track which pages are in memory (page table)
 *   - Pin/unpin pages (reference counting)
 *   - Track dirty pages (modified since last disk write)
 *   - Evict unpinned pages using LRU replacement policy
 *   - Flush dirty pages to disk
 *   - Allocate new pages through the disk manager
 *
 * Time Complexity:
 *   - FetchPage: O(1) average (hash table lookup + LRU update)
 *   - UnpinPage: O(1)
 *   - FlushPage: O(1) (single disk write)
 *   - NewPage: O(1) amortized
 *   - Eviction: O(1) (LRU tail removal)
 *
 * Important Invariants:
 *   - A page can be in at most one frame at any time
 *   - A pinned page (pin_count > 0) is NEVER evicted
 *   - A dirty page is written to disk before its frame is reused
 *   - The buffer pool size is fixed at construction time
 *   - All public methods are thread-safe (mutex-protected)
 *
 * Design Decisions:
 *   - LRU replacement is the standard choice for analytical workloads
 *   - Pin counting (not binary) supports concurrent access patterns
 *   - Dirty flag avoids unnecessary writes for read-only pages
 *   - Page table uses unordered_map for O(1) lookup
 *   - LRU list uses std::list for O(1) splice/erase
 *
 * =============================================================================
 */

#include "disk/disk_manager.h"
#include "storage/page.h"

#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace courtdb {

// =============================================================================
// Frame Metadata
// =============================================================================

/**
 * Metadata associated with each frame in the buffer pool.
 * Tracks the page currently loaded, its pin count, and dirty status.
 */
struct FrameMetadata {
    PageId page_id = INVALID_PAGE_ID;  ///< Which page occupies this frame
    uint32_t pin_count = 0;             ///< Number of active references
    bool is_dirty = false;              ///< Modified since last flush?

    [[nodiscard]] bool IsOccupied() const { return page_id != INVALID_PAGE_ID; }
    [[nodiscard]] bool IsEvictable() const { return IsOccupied() && pin_count == 0; }
};

/// Frame identifier (index into the frame array)
using FrameId = uint32_t;
static constexpr FrameId INVALID_FRAME_ID = 0xFFFFFFFF;

// =============================================================================
// LRU Replacer
// =============================================================================

/**
 * LRU (Least Recently Used) page replacement policy.
 *
 * Maintains a doubly-linked list of unpinned frame IDs. The tail of the
 * list is the least-recently-used frame — the eviction victim.
 *
 * Operations:
 *   - RecordAccess(frame_id): Move/add frame to the front (most recent)
 *   - Remove(frame_id): Remove frame from the LRU list (pinned)
 *   - Evict(): Remove and return the LRU frame (tail)
 *   - Size(): Number of evictable frames
 *
 * All operations are O(1) using a list + hash map combination.
 */
class LRUReplacer {
public:
    explicit LRUReplacer(uint32_t capacity);

    /**
     * Record that a frame was accessed (move to front / add to list).
     * Called when a page is unpinned.
     */
    void RecordAccess(FrameId frame_id);

    /**
     * Remove a frame from the replacer (it's been pinned or evicted).
     */
    void Remove(FrameId frame_id);

    /**
     * Evict the least-recently-used frame.
     * @return The evicted frame ID, or INVALID_FRAME_ID if none available
     */
    [[nodiscard]] FrameId Evict();

    /**
     * @return Number of frames currently eligible for eviction
     */
    [[nodiscard]] uint32_t Size() const;

private:
    /// Doubly-linked list: front = most recent, back = least recent (victim)
    std::list<FrameId> lru_list_;

    /// Map from frame_id → iterator into lru_list_ for O(1) access
    std::unordered_map<FrameId, std::list<FrameId>::iterator> frame_map_;

    uint32_t capacity_;
};

// =============================================================================
// Buffer Pool Manager
// =============================================================================

class BufferPoolManager {
public:
    /**
     * Construct a buffer pool with the specified number of frames.
     * @param pool_size Number of page frames in the buffer pool
     * @param disk_manager Pointer to the disk manager (caller owns lifetime)
     */
    BufferPoolManager(uint32_t pool_size, DiskManager* disk_manager);

    /// Non-copyable, non-movable
    BufferPoolManager(const BufferPoolManager&) = delete;
    BufferPoolManager& operator=(const BufferPoolManager&) = delete;

    ~BufferPoolManager();

    // =========================================================================
    // Page Access
    // =========================================================================

    /**
     * Fetch a page from the buffer pool. If the page is already in memory,
     * return it directly. Otherwise, read it from disk into a free frame.
     *
     * The returned page is pinned (pin_count incremented). Caller MUST
     * call UnpinPage when done.
     *
     * @param page_id The page to fetch
     * @return Pointer to the page, or nullptr if all frames are pinned
     */
    Page* FetchPage(PageId page_id);

    /**
     * Unpin a page, decrementing its reference count. When pin_count reaches 0,
     * the page becomes eligible for eviction.
     *
     * @param page_id The page to unpin
     * @param is_dirty Whether the caller modified the page
     * @return true if the page was found and unpinned, false otherwise
     */
    bool UnpinPage(PageId page_id, bool is_dirty);

    // =========================================================================
    // Page Creation
    // =========================================================================

    /**
     * Allocate a new page on disk and bring it into the buffer pool.
     * The new page is pinned with pin_count = 1.
     *
     * @return Pointer to the new page, or nullptr if all frames are pinned
     */
    Page* NewPage();

    /**
     * Delete a page from both the buffer pool and disk.
     * The page must not be pinned (pin_count must be 0).
     *
     * @param page_id The page to delete
     * @return true if successfully deleted, false if pinned or not found
     */
    bool DeletePage(PageId page_id);

    // =========================================================================
    // Flush Operations
    // =========================================================================

    /**
     * Write a specific dirty page to disk.
     * @param page_id The page to flush
     * @return true if the page was found and flushed, false otherwise
     */
    bool FlushPage(PageId page_id);

    /**
     * Write ALL dirty pages to disk. Used during checkpointing.
     */
    void FlushAllPages();

    // =========================================================================
    // Diagnostics
    // =========================================================================

    [[nodiscard]] uint32_t GetPoolSize() const { return pool_size_; }
    [[nodiscard]] uint32_t GetOccupiedFrameCount() const;
    [[nodiscard]] uint32_t GetEvictableFrameCount() const { return replacer_.Size(); }

    /// Buffer pool hit rate statistics
    struct Stats {
        uint64_t hits = 0;       ///< Page was already in buffer pool
        uint64_t misses = 0;     ///< Page had to be read from disk
        uint64_t evictions = 0;  ///< Pages evicted to make room
        uint64_t flushes = 0;    ///< Dirty pages written to disk

        [[nodiscard]] double HitRate() const {
            uint64_t total = hits + misses;
            return total > 0 ? static_cast<double>(hits) / total : 0.0;
        }
    };

    [[nodiscard]] Stats GetStats() const;
    void ResetStats();

private:
    // =========================================================================
    // Internal Helpers
    // =========================================================================

    /**
     * Find a free frame for a new page. Tries the free list first,
     * then evicts from LRU. Returns INVALID_FRAME_ID if all frames are pinned.
     */
    FrameId FindFreeFrame();

    /**
     * Evict a page from the given frame. Flushes to disk if dirty.
     */
    void EvictFrame(FrameId frame_id);

    // =========================================================================
    // Data Members
    // =========================================================================

    uint32_t pool_size_;                    ///< Total number of frames
    DiskManager* disk_manager_;             ///< Non-owning pointer to disk manager

    std::vector<Page> pages_;               ///< Array of page frames
    std::vector<FrameMetadata> metadata_;   ///< Per-frame metadata
    std::list<FrameId> free_list_;          ///< Frames not currently occupied

    /// Page table: page_id → frame_id
    std::unordered_map<PageId, FrameId> page_table_;

    LRUReplacer replacer_;                  ///< LRU eviction policy

    mutable std::mutex latch_;              ///< Protects all internal state
    Stats stats_;                           ///< Hit/miss statistics
};

}  // namespace courtdb
