/**
 * =============================================================================
 * CourtDB - Buffer Pool Manager Implementation
 * =============================================================================
 *
 * The buffer pool is the heart of CourtDB's memory management. It ensures:
 *   1. Hot pages stay in memory (LRU keeps frequently-accessed pages)
 *   2. Dirty pages are persisted before eviction (no data loss)
 *   3. Pin counting prevents eviction of in-use pages (safety)
 *   4. Fixed memory footprint (pool_size * PAGE_SIZE bytes)
 *
 * Threading Model:
 *   A single coarse-grained mutex protects all operations. This is simple
 *   and correct. For higher concurrency, partition-level latching or
 *   lock-free structures could be used (future optimization).
 *
 * =============================================================================
 */

#include "buffer/buffer_pool_manager.h"

#include <cassert>
#include <cstring>

namespace courtdb {

// =============================================================================
// LRU Replacer Implementation
// =============================================================================

LRUReplacer::LRUReplacer(uint32_t capacity) : capacity_(capacity) {
    frame_map_.reserve(capacity);
}

void LRUReplacer::RecordAccess(FrameId frame_id) {
    auto it = frame_map_.find(frame_id);
    if (it != frame_map_.end()) {
        // Already in the list — move to front (most recently used)
        lru_list_.splice(lru_list_.begin(), lru_list_, it->second);
    } else {
        // Not in the list — add to front
        lru_list_.push_front(frame_id);
        frame_map_[frame_id] = lru_list_.begin();
    }
}

void LRUReplacer::Remove(FrameId frame_id) {
    auto it = frame_map_.find(frame_id);
    if (it != frame_map_.end()) {
        lru_list_.erase(it->second);
        frame_map_.erase(it);
    }
}

FrameId LRUReplacer::Evict() {
    if (lru_list_.empty()) {
        return INVALID_FRAME_ID;
    }

    // Evict from back (least recently used)
    FrameId victim = lru_list_.back();
    lru_list_.pop_back();
    frame_map_.erase(victim);
    return victim;
}

uint32_t LRUReplacer::Size() const {
    return static_cast<uint32_t>(lru_list_.size());
}

// =============================================================================
// Buffer Pool Manager Implementation
// =============================================================================

BufferPoolManager::BufferPoolManager(uint32_t pool_size, DiskManager* disk_manager)
    : pool_size_(pool_size),
      disk_manager_(disk_manager),
      pages_(pool_size),
      metadata_(pool_size),
      replacer_(pool_size) {

    assert(pool_size > 0);
    assert(disk_manager != nullptr);

    // Initialize free list with all frame IDs
    for (uint32_t i = 0; i < pool_size; ++i) {
        free_list_.push_back(i);
    }

    page_table_.reserve(pool_size);
}

BufferPoolManager::~BufferPoolManager() {
    // Flush all dirty pages before destruction
    FlushAllPages();
}

// =============================================================================
// Page Access
// =============================================================================

Page* BufferPoolManager::FetchPage(PageId page_id) {
    std::lock_guard<std::mutex> lock(latch_);

    // Case 1: Page is already in the buffer pool
    auto it = page_table_.find(page_id);
    if (it != page_table_.end()) {
        FrameId frame_id = it->second;
        auto& meta = metadata_[frame_id];

        // Pin the page
        meta.pin_count++;

        // Remove from replacer (no longer evictable while pinned)
        if (meta.pin_count == 1) {
            replacer_.Remove(frame_id);
        }

        stats_.hits++;
        return &pages_[frame_id];
    }

    // Case 2: Page is not in memory — need to load from disk
    stats_.misses++;

    FrameId frame_id = FindFreeFrame();
    if (frame_id == INVALID_FRAME_ID) {
        return nullptr;  // All frames are pinned, cannot evict
    }

    // Read the page from disk into this frame
    auto& page = pages_[frame_id];
    if (!disk_manager_->ReadPage(page_id, page.GetMutableData())) {
        // Read failed — put frame back on free list
        free_list_.push_back(frame_id);
        return nullptr;
    }

    // Deserialize the page header and slot directory from the raw buffer
    page.DeserializeFromBuffer();

    // Update metadata
    auto& meta = metadata_[frame_id];
    meta.page_id = page_id;
    meta.pin_count = 1;
    meta.is_dirty = false;

    // Update page table
    page_table_[page_id] = frame_id;

    return &page;
}

bool BufferPoolManager::UnpinPage(PageId page_id, bool is_dirty) {
    std::lock_guard<std::mutex> lock(latch_);

    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return false;  // Page not in buffer pool
    }

    FrameId frame_id = it->second;
    auto& meta = metadata_[frame_id];

    if (meta.pin_count == 0) {
        return false;  // Already fully unpinned (bug in caller)
    }

    meta.pin_count--;

    // Dirty flag is sticky — once dirty, stays dirty until flushed
    if (is_dirty) {
        meta.is_dirty = true;
    }

    // When pin_count reaches 0, page becomes eligible for eviction
    if (meta.pin_count == 0) {
        replacer_.RecordAccess(frame_id);
    }

    return true;
}

// =============================================================================
// Page Creation
// =============================================================================

Page* BufferPoolManager::NewPage() {
    std::lock_guard<std::mutex> lock(latch_);

    FrameId frame_id = FindFreeFrame();
    if (frame_id == INVALID_FRAME_ID) {
        return nullptr;  // All frames pinned
    }

    // Allocate a new page on disk
    PageId new_page_id = disk_manager_->AllocatePage();

    // Initialize the page in the frame as a fresh empty page
    auto& page = pages_[frame_id];
    // Use placement-style reset: zero the buffer and reinitialize the Page state
    std::memset(page.GetMutableData(), 0, PAGE_SIZE);
    // Reconstruct the page with proper initial state (not from zeroed buffer)
    page = Page(new_page_id);

    // Update metadata
    auto& meta = metadata_[frame_id];
    meta.page_id = new_page_id;
    meta.pin_count = 1;
    meta.is_dirty = true;  // New page needs to be written

    // Update page table
    page_table_[new_page_id] = frame_id;

    return &page;
}

bool BufferPoolManager::DeletePage(PageId page_id) {
    std::lock_guard<std::mutex> lock(latch_);

    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        // Page not in buffer pool — deallocate on disk directly
        return disk_manager_->DeallocatePage(page_id);
    }

    FrameId frame_id = it->second;
    auto& meta = metadata_[frame_id];

    if (meta.pin_count > 0) {
        return false;  // Cannot delete a pinned page
    }

    // Remove from replacer and page table
    replacer_.Remove(frame_id);
    page_table_.erase(it);

    // Reset frame metadata
    meta.page_id = INVALID_PAGE_ID;
    meta.pin_count = 0;
    meta.is_dirty = false;

    // Return frame to free list
    free_list_.push_back(frame_id);

    // Deallocate on disk
    return disk_manager_->DeallocatePage(page_id);
}

// =============================================================================
// Flush Operations
// =============================================================================

bool BufferPoolManager::FlushPage(PageId page_id) {
    std::lock_guard<std::mutex> lock(latch_);

    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return false;
    }

    FrameId frame_id = it->second;
    auto& page = pages_[frame_id];
    auto& meta = metadata_[frame_id];

    // Serialize in-memory state to the raw buffer before writing
    page.SerializeToBuffer();
    disk_manager_->WritePage(page_id, page.GetData());
    meta.is_dirty = false;
    stats_.flushes++;

    return true;
}

void BufferPoolManager::FlushAllPages() {
    std::lock_guard<std::mutex> lock(latch_);

    for (FrameId i = 0; i < pool_size_; ++i) {
        if (metadata_[i].IsOccupied() && metadata_[i].is_dirty) {
            auto& page = pages_[i];
            page.SerializeToBuffer();
            disk_manager_->WritePage(metadata_[i].page_id, page.GetData());
            metadata_[i].is_dirty = false;
            stats_.flushes++;
        }
    }

    disk_manager_->Flush();
}

// =============================================================================
// Diagnostics
// =============================================================================

uint32_t BufferPoolManager::GetOccupiedFrameCount() const {
    std::lock_guard<std::mutex> lock(latch_);
    return static_cast<uint32_t>(page_table_.size());
}

BufferPoolManager::Stats BufferPoolManager::GetStats() const {
    std::lock_guard<std::mutex> lock(latch_);
    return stats_;
}

void BufferPoolManager::ResetStats() {
    std::lock_guard<std::mutex> lock(latch_);
    stats_ = Stats{};
}

// =============================================================================
// Private Helpers
// =============================================================================

FrameId BufferPoolManager::FindFreeFrame() {
    // Try the free list first (no eviction needed)
    if (!free_list_.empty()) {
        FrameId frame_id = free_list_.front();
        free_list_.pop_front();
        return frame_id;
    }

    // No free frames — must evict
    FrameId victim_frame = replacer_.Evict();
    if (victim_frame == INVALID_FRAME_ID) {
        return INVALID_FRAME_ID;  // All frames are pinned
    }

    EvictFrame(victim_frame);
    stats_.evictions++;
    return victim_frame;
}

void BufferPoolManager::EvictFrame(FrameId frame_id) {
    auto& meta = metadata_[frame_id];
    assert(meta.pin_count == 0);

    // Flush to disk if dirty
    if (meta.is_dirty) {
        auto& page = pages_[frame_id];
        page.SerializeToBuffer();
        disk_manager_->WritePage(meta.page_id, page.GetData());
        meta.is_dirty = false;
        stats_.flushes++;
    }

    // Remove from page table
    page_table_.erase(meta.page_id);

    // Clear metadata
    meta.page_id = INVALID_PAGE_ID;
    meta.pin_count = 0;
    meta.is_dirty = false;
}

}  // namespace courtdb
