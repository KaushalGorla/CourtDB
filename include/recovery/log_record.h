#pragma once
/**
 * =============================================================================
 * CourtDB - Log Record Definitions
 * =============================================================================
 *
 * Purpose:
 *   Defines the structure of Write-Ahead Log (WAL) records. Every modification
 *   to the database is first recorded as a log entry before the actual page
 *   is modified. This enables crash recovery via redo.
 *
 * Responsibilities:
 *   - Define log record types (Insert, Delete, Update, Checkpoint, etc.)
 *   - Provide serialization/deserialization of log records
 *   - Assign Log Sequence Numbers (LSNs) for ordering
 *   - Track transaction-level and page-level information
 *
 * Important Invariants:
 *   - LSNs are monotonically increasing
 *   - Every log record is self-describing (type + length prefix)
 *   - Log records are append-only (never modified in place)
 *   - A log record is written BEFORE the corresponding page modification
 *
 * Design Decisions:
 *   - Redo-only logging (no undo) — simpler, sufficient for embedded DB
 *   - Physical logging: stores the actual bytes written to pages
 *   - Fixed header + variable payload for each log record
 *   - LSN stored in each page header for recovery coordination
 *
 * =============================================================================
 */

#include "storage/page.h"
#include "storage/record.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace courtdb {

// =============================================================================
// Log Sequence Number
// =============================================================================

/// Monotonically increasing identifier for log records.
/// Also represents the byte offset of the record in the log file.
using LSN = uint64_t;

static constexpr LSN INVALID_LSN = 0;

// =============================================================================
// Log Record Types
// =============================================================================

enum class LogRecordType : uint8_t {
    kInvalid = 0,
    kInsert = 1,       ///< Record inserted into a page
    kDelete = 2,       ///< Record deleted from a page
    kUpdate = 3,       ///< Record updated in a page
    kNewPage = 4,      ///< New page allocated
    kCheckpointBegin = 5,  ///< Start of a checkpoint
    kCheckpointEnd = 6,    ///< End of a checkpoint (includes dirty page table)
};

// =============================================================================
// Log Record Header
// =============================================================================

/**
 * Fixed header at the start of every log record.
 * Total: 21 bytes
 *
 * Layout:
 *   [4B: total_size] [8B: lsn] [1B: type] [4B: page_id] [2B: slot_id] [2B: prev_lsn_offset]
 */
struct LogRecordHeader {
    uint32_t total_size = 0;       ///< Total size of this log record (header + payload)
    LSN lsn = INVALID_LSN;         ///< Log sequence number
    LogRecordType type = LogRecordType::kInvalid;
    PageId page_id = INVALID_PAGE_ID;  ///< Page affected by this operation
    uint16_t slot_id = 0;          ///< Slot affected (for insert/delete/update)
    uint16_t payload_size = 0;     ///< Size of the payload data

    static constexpr uint32_t HEADER_SIZE = 21;
};

// =============================================================================
// Log Record
// =============================================================================

/**
 * A complete log record with header and optional payload.
 *
 * For Insert: payload = new record bytes
 * For Delete: payload = old record bytes (for potential undo)
 * For Update: payload = [2B old_len][old_data][2B new_len][new_data]
 * For NewPage: payload = empty
 * For Checkpoint: payload = dirty page table
 */
class LogRecord {
public:
    LogRecord() = default;

    // =========================================================================
    // Factory Methods
    // =========================================================================

    /// Create an INSERT log record
    static LogRecord MakeInsert(LSN lsn, PageId page_id, uint16_t slot_id,
                                const uint8_t* data, uint16_t length);

    /// Create a DELETE log record
    static LogRecord MakeDelete(LSN lsn, PageId page_id, uint16_t slot_id,
                                const uint8_t* old_data, uint16_t old_length);

    /// Create an UPDATE log record
    static LogRecord MakeUpdate(LSN lsn, PageId page_id, uint16_t slot_id,
                                const uint8_t* old_data, uint16_t old_length,
                                const uint8_t* new_data, uint16_t new_length);

    /// Create a NEW PAGE log record
    static LogRecord MakeNewPage(LSN lsn, PageId page_id);

    /// Create a CHECKPOINT BEGIN log record
    static LogRecord MakeCheckpointBegin(LSN lsn);

    /// Create a CHECKPOINT END log record with dirty page table
    static LogRecord MakeCheckpointEnd(LSN lsn,
                                       const std::vector<std::pair<PageId, LSN>>& dirty_pages);

    // =========================================================================
    // Serialization
    // =========================================================================

    /// Serialize the log record into a byte buffer for writing to the log file
    [[nodiscard]] std::vector<uint8_t> Serialize() const;

    /// Deserialize a log record from a byte buffer
    static LogRecord Deserialize(const uint8_t* data, uint32_t length);

    // =========================================================================
    // Accessors
    // =========================================================================

    [[nodiscard]] const LogRecordHeader& GetHeader() const { return header_; }
    [[nodiscard]] LogRecordType GetType() const { return header_.type; }
    [[nodiscard]] LSN GetLSN() const { return header_.lsn; }
    [[nodiscard]] PageId GetPageId() const { return header_.page_id; }
    [[nodiscard]] uint16_t GetSlotId() const { return header_.slot_id; }
    [[nodiscard]] uint32_t GetTotalSize() const { return header_.total_size; }

    [[nodiscard]] const std::vector<uint8_t>& GetPayload() const { return payload_; }

    /// For INSERT records: get the inserted data
    [[nodiscard]] std::pair<const uint8_t*, uint16_t> GetInsertData() const;

    /// For DELETE records: get the deleted data
    [[nodiscard]] std::pair<const uint8_t*, uint16_t> GetDeleteData() const;

    /// For UPDATE records: get old and new data
    struct UpdateData {
        const uint8_t* old_data;
        uint16_t old_length;
        const uint8_t* new_data;
        uint16_t new_length;
    };
    [[nodiscard]] UpdateData GetUpdateData() const;

    /// For CHECKPOINT END: get the dirty page table
    [[nodiscard]] std::vector<std::pair<PageId, LSN>> GetDirtyPageTable() const;

private:
    LogRecordHeader header_;
    std::vector<uint8_t> payload_;
};

}  // namespace courtdb
