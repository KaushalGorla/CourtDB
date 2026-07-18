#pragma once
/**
 * =============================================================================
 * CourtDB - Page Layout
 * =============================================================================
 *
 * Purpose:
 *   Defines the fundamental unit of storage in CourtDB. Every piece of data
 *   (records, index nodes, metadata) lives inside a fixed-size Page.
 *
 * Responsibilities:
 *   - Provide a 4KB in-memory representation of a disk block
 *   - Manage a slotted page layout for variable-length records
 *   - Track free space, slot directory, and record offsets
 *   - Support record insertion, deletion, and access by slot number
 *
 * Time Complexity:
 *   - Insert record: O(1) amortized (append to free space)
 *   - Access by slot: O(1) (direct offset lookup)
 *   - Delete record: O(1) (mark slot as deleted, compact lazily)
 *
 * Important Invariants:
 *   - Page size is always exactly PAGE_SIZE (4096 bytes)
 *   - Slot directory grows forward from the header
 *   - Record data grows backward from the end of the page
 *   - free_space_offset always points to the boundary between used and free space
 *   - A slot with offset=0 and length=0 indicates a deleted/empty slot
 *
 * Design Decisions:
 *   - 4KB pages match OS page size for optimal I/O alignment
 *   - Slotted layout avoids external fragmentation for variable-length records
 *   - Page ID is stored in-page for self-identification after disk reads
 *   - Compaction is deferred to amortize cost across multiple deletes
 *
 * =============================================================================
 */

#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

namespace courtdb {

// =============================================================================
// Constants
// =============================================================================

/// Fixed page size in bytes. Matches typical OS page and filesystem block size.
static constexpr uint32_t PAGE_SIZE = 4096;

/// Invalid page identifier sentinel.
static constexpr uint32_t INVALID_PAGE_ID = 0xFFFFFFFF;

/// Type alias for page identifiers.
using PageId = uint32_t;

// =============================================================================
// Slot Directory Entry
// =============================================================================

/**
 * Each slot in the directory stores the offset and length of a record
 * within the page's data region. A slot with offset=0 and length=0
 * represents a deleted or unoccupied slot.
 */
struct SlotEntry {
    uint16_t offset = 0;  ///< Byte offset from start of page to record data
    uint16_t length = 0;  ///< Length of the record in bytes

    [[nodiscard]] bool IsEmpty() const { return offset == 0 && length == 0; }
};

// =============================================================================
// Page Header
// =============================================================================

/**
 * Fixed-size header at the beginning of every page.
 * Total size: 20 bytes (leaves 4076 bytes for slots + data).
 */
struct PageHeader {
    PageId page_id = INVALID_PAGE_ID;     ///< Self-identifying page number
    uint16_t num_slots = 0;                ///< Number of slots in the directory
    uint16_t free_space_offset = 0;        ///< End of slot directory (start of free space)
    uint16_t free_space_end = PAGE_SIZE;   ///< Start of record data region (grows backward)
    uint16_t num_records = 0;              ///< Number of live (non-deleted) records
    uint32_t next_page_id = INVALID_PAGE_ID; ///< Link to next page (for heap file chains)
    uint16_t flags = 0;                    ///< Page type flags (data, index, overflow, etc.)
};

// =============================================================================
// Page Type Flags
// =============================================================================

enum class PageType : uint16_t {
    kData = 0x0001,       ///< Regular data/heap page
    kIndex = 0x0002,      ///< B+ tree index page
    kOverflow = 0x0004,   ///< Overflow page for large records
    kMetadata = 0x0008,   ///< Database metadata/catalog page
    kFreeList = 0x0010,   ///< Free page list tracking page
};

// =============================================================================
// Page Class
// =============================================================================

/**
 * Page is the fundamental storage unit in CourtDB.
 *
 * Memory layout (4096 bytes total):
 *   [PageHeader][Slot 0][Slot 1]...[Slot N][--- free space ---][Record N]...[Record 0]
 *
 * The slot directory grows forward (low addresses), while record data grows
 * backward (high addresses). When they meet, the page is full.
 */
class Page {
public:
    // =========================================================================
    // Construction
    // =========================================================================

    Page();
    explicit Page(PageId page_id);

    /// Non-copyable (pages are large; use move or pointers)
    Page(const Page&) = delete;
    Page& operator=(const Page&) = delete;

    /// Movable
    Page(Page&& other) noexcept;
    Page& operator=(Page&& other) noexcept;

    ~Page() = default;

    // =========================================================================
    // Record Operations
    // =========================================================================

    /**
     * Insert a record into the page.
     * @param data Pointer to record bytes
     * @param length Number of bytes to insert
     * @return Slot number if successful, std::nullopt if page is full
     */
    [[nodiscard]] std::optional<uint16_t> InsertRecord(const uint8_t* data, uint16_t length);

    /**
     * Delete the record at the given slot.
     * @param slot_id Slot number to delete
     * @return true if the slot was valid and deleted, false otherwise
     */
    bool DeleteRecord(uint16_t slot_id);

    /**
     * Retrieve the record at the given slot.
     * @param slot_id Slot number to read
     * @return Pointer to the record data and its length, or nullopt if slot is empty/invalid
     */
    [[nodiscard]] std::optional<std::pair<const uint8_t*, uint16_t>> GetRecord(uint16_t slot_id) const;

    /**
     * Update the record at the given slot. If the new record is larger than
     * the old one and doesn't fit, returns false (caller must delete + re-insert).
     * @param slot_id Slot number to update
     * @param data New record data
     * @param length New record length
     * @return true if updated in-place, false if insufficient space
     */
    bool UpdateRecord(uint16_t slot_id, const uint8_t* data, uint16_t length);

    // =========================================================================
    // Space Management
    // =========================================================================

    /**
     * Compact the page by eliminating gaps left by deleted records.
     * Moves all live records to be contiguous at the end of the page.
     * Slot offsets are updated accordingly.
     */
    void Compact();

    /**
     * @return Number of free bytes available for a new record (including slot overhead)
     */
    [[nodiscard]] uint16_t GetFreeSpace() const;

    /**
     * Check whether a record of the given size can be inserted.
     * Accounts for both the record data and a new slot entry.
     */
    [[nodiscard]] bool HasSpaceFor(uint16_t record_length) const;

    // =========================================================================
    // Accessors
    // =========================================================================

    [[nodiscard]] PageId GetPageId() const { return header_.page_id; }
    void SetPageId(PageId id) { header_.page_id = id; }

    [[nodiscard]] uint16_t GetNumRecords() const { return header_.num_records; }
    [[nodiscard]] uint16_t GetNumSlots() const { return header_.num_slots; }

    [[nodiscard]] PageId GetNextPageId() const { return header_.next_page_id; }
    void SetNextPageId(PageId id) { header_.next_page_id = id; }

    [[nodiscard]] uint16_t GetFlags() const { return header_.flags; }
    void SetFlags(uint16_t flags) { header_.flags = flags; }

    /// Direct access to the raw page data (for disk I/O)
    [[nodiscard]] const uint8_t* GetData() const { return data_; }
    [[nodiscard]] uint8_t* GetMutableData() { return data_; }

    /// Serialize the in-memory header and slot directory into the raw data buffer
    void SerializeToBuffer();

    /// Deserialize the raw data buffer into in-memory header and slot directory
    void DeserializeFromBuffer();

private:
    // =========================================================================
    // Internal Helpers
    // =========================================================================

    /// Compute the byte offset where the slot directory ends
    [[nodiscard]] uint16_t SlotDirectoryEnd() const;

    /// Find a reusable empty slot in the directory, or return nullopt
    [[nodiscard]] std::optional<uint16_t> FindEmptySlot() const;

    // =========================================================================
    // Data Members
    // =========================================================================

    alignas(64) uint8_t data_[PAGE_SIZE];  ///< Raw page bytes (cache-line aligned)
    PageHeader header_;                     ///< Deserialized header
    std::vector<SlotEntry> slots_;          ///< Deserialized slot directory
};

}  // namespace courtdb
