#pragma once
/**
 * =============================================================================
 * CourtDB - Disk Manager
 * =============================================================================
 *
 * Purpose:
 *   Provides page-level read/write access to the database file on disk.
 *   This is the single point of contact between CourtDB and the filesystem.
 *
 * Responsibilities:
 *   - Open/create database files
 *   - Read pages from disk into memory buffers
 *   - Write pages from memory buffers to disk
 *   - Allocate new pages (extend the file)
 *   - Deallocate pages (maintain a free list)
 *   - Track total page count and file size
 *   - Flush writes to durable storage
 *
 * Time Complexity:
 *   - ReadPage: O(1) (single positioned read)
 *   - WritePage: O(1) (single positioned write)
 *   - AllocatePage: O(1) amortized (free list pop or file extend)
 *   - DeallocatePage: O(1) (free list push)
 *
 * Important Invariants:
 *   - Page 0 is always the header/metadata page
 *   - File size is always a multiple of PAGE_SIZE
 *   - All I/O occurs at page-aligned offsets
 *   - Thread safety is NOT guaranteed (caller must synchronize)
 *
 * Design Decisions:
 *   - Uses POSIX pread/pwrite for positioned I/O (no seeking required)
 *   - Header page stores page count and free list head
 *   - Free pages form a singly-linked list via their first 4 bytes
 *   - fsync is explicit (caller decides when durability is needed)
 *
 * =============================================================================
 */

#include "storage/page.h"

#include <cstdint>
#include <string>
#include <mutex>

namespace courtdb {

// =============================================================================
// Database File Header (stored in page 0)
// =============================================================================

/**
 * The first page of the database file stores global metadata.
 * This is serialized directly into page 0.
 */
struct DatabaseHeader {
    static constexpr uint32_t MAGIC = 0x434F5552;  // "COUR" in ASCII
    static constexpr uint32_t VERSION = 1;

    uint32_t magic = MAGIC;            ///< Magic number for file identification
    uint32_t version = VERSION;        ///< Schema version
    uint32_t page_count = 1;           ///< Total number of pages (including header)
    PageId free_list_head = INVALID_PAGE_ID;  ///< First page in the free list
    uint32_t free_page_count = 0;      ///< Number of pages in the free list
};

// =============================================================================
// Disk Manager
// =============================================================================

class DiskManager {
public:
    /**
     * Construct a DiskManager for the given file path.
     * Creates the file if it doesn't exist; opens it if it does.
     * @param db_file_path Path to the database file
     * @throws std::runtime_error if file cannot be opened/created
     */
    explicit DiskManager(const std::string& db_file_path);

    /// Non-copyable, non-movable (owns a file descriptor)
    DiskManager(const DiskManager&) = delete;
    DiskManager& operator=(const DiskManager&) = delete;
    DiskManager(DiskManager&&) = delete;
    DiskManager& operator=(DiskManager&&) = delete;

    ~DiskManager();

    // =========================================================================
    // Page I/O
    // =========================================================================

    /**
     * Read a page from disk into the provided buffer.
     * @param page_id The page to read
     * @param buffer Output buffer (must be at least PAGE_SIZE bytes)
     * @return true on success, false on error (e.g., invalid page_id)
     */
    bool ReadPage(PageId page_id, uint8_t* buffer);

    /**
     * Write a buffer to disk at the specified page location.
     * @param page_id The page to write
     * @param buffer Input buffer (must be at least PAGE_SIZE bytes)
     * @return true on success, false on error
     */
    bool WritePage(PageId page_id, const uint8_t* buffer);

    // =========================================================================
    // Page Allocation
    // =========================================================================

    /**
     * Allocate a new page. Reuses a free-list page if available,
     * otherwise extends the file.
     * @return The PageId of the newly allocated page
     */
    PageId AllocatePage();

    /**
     * Return a page to the free list for future reuse.
     * @param page_id The page to deallocate
     * @return true on success, false if page_id is invalid
     */
    bool DeallocatePage(PageId page_id);

    // =========================================================================
    // Durability
    // =========================================================================

    /**
     * Force all pending writes to durable storage.
     * Calls fsync on the underlying file descriptor.
     */
    void Flush();

    // =========================================================================
    // Accessors
    // =========================================================================

    [[nodiscard]] uint32_t GetPageCount() const { return header_.page_count; }
    [[nodiscard]] uint32_t GetFreePageCount() const { return header_.free_page_count; }
    [[nodiscard]] const std::string& GetFilePath() const { return file_path_; }
    [[nodiscard]] uint64_t GetFileSize() const;

private:
    // =========================================================================
    // Internal Helpers
    // =========================================================================

    /// Compute the byte offset for a given page ID
    [[nodiscard]] uint64_t PageOffset(PageId page_id) const {
        return static_cast<uint64_t>(page_id) * PAGE_SIZE;
    }

    /// Read the database header from page 0
    void ReadHeader();

    /// Write the database header to page 0
    void WriteHeader();

    /// Initialize a brand-new database file
    void InitializeNewFile();

    // =========================================================================
    // Data Members
    // =========================================================================

    std::string file_path_;         ///< Path to the database file
    int fd_ = -1;                   ///< POSIX file descriptor
    DatabaseHeader header_;         ///< Cached copy of the file header
    uint32_t num_writes_ = 0;      ///< Write counter (for diagnostics)
    uint32_t num_reads_ = 0;       ///< Read counter (for diagnostics)
};

}  // namespace courtdb
