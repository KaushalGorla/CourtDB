#pragma once
/**
 * =============================================================================
 * CourtDB - Heap File (Table Storage)
 * =============================================================================
 *
 * Purpose:
 *   Organizes records into a table using a linked list of data pages.
 *   This is the primary storage structure for NBA play-by-play data.
 *   Records are stored unordered (heap) and identified by RID.
 *
 * Responsibilities:
 *   - Insert records into the table (find page with space, or allocate new)
 *   - Delete records by RID
 *   - Retrieve records by RID
 *   - Update records by RID
 *   - Provide a sequential scan iterator over all records
 *   - Maintain a linked list of data pages for the table
 *   - Track a "directory" of pages with free space for fast insertion
 *
 * Time Complexity:
 *   - Insert: O(1) amortized (free space tracking)
 *   - Delete by RID: O(1) (direct page + slot access)
 *   - Get by RID: O(1) (direct page + slot access)
 *   - Sequential scan: O(N) where N is total records
 *
 * Important Invariants:
 *   - Every record has a unique, stable RID (page_id, slot_id)
 *   - Pages are linked via next_page_id forming a singly-linked list
 *   - The first page ID of the heap is stored in a header/catalog
 *   - Deleted records leave tombstones (reclaimed on compaction)
 *
 * Design Decisions:
 *   - Linked page list is simple and supports unlimited table growth
 *   - Free space directory avoids scanning all pages on insert
 *   - Sequential scan follows the page chain (good for analytics)
 *   - No ordering guarantee — indexes provide ordered access
 *
 * =============================================================================
 */

#include "buffer/buffer_pool_manager.h"
#include "storage/page.h"
#include "storage/record.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace courtdb {

// =============================================================================
// Heap File Iterator
// =============================================================================

/**
 * Iterator for sequential scan over all records in a heap file.
 * Follows the page chain from first to last, visiting every live slot.
 *
 * Usage:
 *   auto it = heap.Begin();
 *   while (it.IsValid()) {
 *       auto [data, len] = it.GetRecord();
 *       // process record...
 *       it.Next();
 *   }
 */
class HeapFileIterator {
public:
    HeapFileIterator() = default;

    /// @return true if the iterator points to a valid record
    [[nodiscard]] bool IsValid() const { return valid_; }

    /// @return The RID of the current record
    [[nodiscard]] RID GetRID() const { return current_rid_; }

    /// @return Pointer to current record data and its length
    [[nodiscard]] std::pair<const uint8_t*, uint16_t> GetRecord() const;

    /// Advance to the next record
    void Next();

private:
    friend class HeapFile;

    HeapFileIterator(BufferPoolManager* bpm, PageId first_page_id);

    /// Advance to the next non-empty slot, possibly moving to next page
    void AdvanceToNextRecord();

    BufferPoolManager* bpm_ = nullptr;
    PageId current_page_id_ = INVALID_PAGE_ID;
    uint16_t current_slot_ = 0;
    Page* current_page_ = nullptr;
    RID current_rid_;
    bool valid_ = false;
};

// =============================================================================
// Heap File
// =============================================================================

class HeapFile {
public:
    /**
     * Create or open a heap file.
     * @param bpm Buffer pool manager for page access
     * @param first_page_id The first page of this heap (INVALID_PAGE_ID to create new)
     */
    HeapFile(BufferPoolManager* bpm, PageId first_page_id = INVALID_PAGE_ID);

    // =========================================================================
    // Record Operations
    // =========================================================================

    /**
     * Insert a record into the heap file.
     * Finds a page with sufficient free space, or allocates a new page.
     *
     * @param data Record bytes to insert
     * @param length Number of bytes
     * @return RID of the inserted record, or invalid RID on failure
     */
    RID InsertRecord(const uint8_t* data, uint16_t length);

    /**
     * Delete the record at the given RID.
     * @param rid Record to delete
     * @return true if successfully deleted, false if RID is invalid
     */
    bool DeleteRecord(const RID& rid);

    /**
     * Retrieve the record at the given RID.
     * @param rid Record to retrieve
     * @param out_data Output: pointer to record data (valid while page is pinned)
     * @param out_length Output: record length
     * @return true if record found, false otherwise
     */
    bool GetRecord(const RID& rid, const uint8_t** out_data, uint16_t* out_length);

    /**
     * Update the record at the given RID.
     * If the new record doesn't fit in the same page, deletes and re-inserts
     * (the RID may change in this case).
     *
     * @param rid Current RID of the record
     * @param data New record data
     * @param length New record length
     * @return The RID of the updated record (may differ from input if relocated)
     */
    RID UpdateRecord(const RID& rid, const uint8_t* data, uint16_t length);

    // =========================================================================
    // Iteration
    // =========================================================================

    /**
     * Begin a sequential scan from the first record.
     * The caller must call UnpinPage on pages as they advance through the scan.
     */
    [[nodiscard]] HeapFileIterator Begin();

    // =========================================================================
    // Bulk Operations
    // =========================================================================

    /**
     * Insert multiple records efficiently using batch semantics.
     * Reduces pin/unpin overhead by batching inserts to the same page.
     *
     * @param records Vector of (data, length) pairs
     * @return Vector of RIDs for all inserted records
     */
    std::vector<RID> BulkInsert(const std::vector<std::pair<const uint8_t*, uint16_t>>& records);

    // =========================================================================
    // Accessors
    // =========================================================================

    [[nodiscard]] PageId GetFirstPageId() const { return first_page_id_; }
    [[nodiscard]] uint32_t GetRecordCount() const { return record_count_; }
    [[nodiscard]] uint32_t GetPageCount() const { return page_count_; }

private:
    // =========================================================================
    // Internal Helpers
    // =========================================================================

    /// Allocate a new page and link it into the heap chain
    PageId AllocateNewPage();

    /// Find a page with enough free space for a record of the given size
    PageId FindPageWithSpace(uint16_t record_length);

    // =========================================================================
    // Data Members
    // =========================================================================

    BufferPoolManager* bpm_;           ///< Non-owning pointer to buffer pool
    PageId first_page_id_;             ///< Head of the page chain
    PageId last_page_id_;              ///< Tail of the page chain (for append)
    uint32_t record_count_ = 0;        ///< Total live records in the heap
    uint32_t page_count_ = 0;          ///< Number of pages in the heap

    /// Free space directory: pages known to have space available.
    /// Avoids scanning the entire chain on every insert.
    std::vector<PageId> pages_with_space_;
};

}  // namespace courtdb
