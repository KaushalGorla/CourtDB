#pragma once
/**
 * =============================================================================
 * CourtDB - Log Manager (WAL)
 * =============================================================================
 *
 * Purpose:
 *   Manages the Write-Ahead Log file. Provides append-only log writing with
 *   group commit semantics and log file reading for recovery.
 *
 * Responsibilities:
 *   - Append log records to the log file
 *   - Assign monotonically increasing LSNs
 *   - Buffer log writes for performance (group commit)
 *   - Force-flush log to disk on demand (for WAL protocol)
 *   - Read log records sequentially (for recovery)
 *   - Provide an iterator over the log for redo/undo
 *
 * Time Complexity:
 *   - AppendLogRecord: O(1) amortized (buffered write)
 *   - Flush: O(buffer_size) (single sequential write)
 *   - ReadNext: O(1) (sequential read)
 *
 * Important Invariants:
 *   - LSNs are the byte offset of the record in the log file
 *   - Log records are never modified once written
 *   - The WAL protocol: log record flushed BEFORE dirty page written to disk
 *   - Group commit: multiple records batched into a single fsync
 *
 * Design Decisions:
 *   - Append-only file for maximum write throughput
 *   - In-memory buffer (log_buffer_) batches small writes
 *   - LSN = file offset for O(1) seeking to any record
 *   - Sequential scan for recovery (no random access needed)
 *
 * =============================================================================
 */

#include "recovery/log_record.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace courtdb {

// =============================================================================
// Log Manager
// =============================================================================

class LogManager {
public:
    /// Default log buffer size (64KB) — flushed when full or on explicit Flush()
    static constexpr uint32_t DEFAULT_LOG_BUFFER_SIZE = 64 * 1024;

    /**
     * Create or open a log manager for the given log file.
     * @param log_file_path Path to the WAL file
     */
    explicit LogManager(const std::string& log_file_path);

    /// Non-copyable
    LogManager(const LogManager&) = delete;
    LogManager& operator=(const LogManager&) = delete;

    ~LogManager();

    // =========================================================================
    // Write Operations
    // =========================================================================

    /**
     * Append a log record to the WAL. Assigns an LSN and buffers the write.
     * @param record The log record to append
     * @return The LSN assigned to this record
     */
    LSN AppendLogRecord(LogRecord& record);

    /**
     * Force all buffered log records to durable storage.
     * Must be called before a dirty page is written to the data file.
     */
    void Flush();

    /**
     * Flush the log up to (and including) the specified LSN.
     * Used to ensure WAL protocol before page writes.
     */
    void FlushUpTo(LSN lsn);

    // =========================================================================
    // Read Operations (for Recovery)
    // =========================================================================

    /**
     * Iterator for reading log records sequentially from the beginning.
     * Used during crash recovery.
     */
    class LogIterator {
    public:
        LogIterator() = default;

        /// @return true if there are more records to read
        [[nodiscard]] bool IsValid() const { return valid_; }

        /// @return The current log record
        [[nodiscard]] const LogRecord& GetRecord() const { return current_; }

        /// Advance to the next log record
        void Next();

    private:
        friend class LogManager;
        LogIterator(int fd, uint64_t file_size);

        int fd_ = -1;
        uint64_t file_size_ = 0;
        uint64_t current_offset_ = 0;
        LogRecord current_;
        bool valid_ = false;

        void ReadCurrentRecord();
    };

    /**
     * Begin reading log records from the start of the file.
     */
    [[nodiscard]] LogIterator Begin();

    /**
     * Begin reading log records from a specific LSN.
     */
    [[nodiscard]] LogIterator BeginAt(LSN start_lsn);

    // =========================================================================
    // Accessors
    // =========================================================================

    [[nodiscard]] LSN GetCurrentLSN() const { return next_lsn_; }
    [[nodiscard]] LSN GetFlushedLSN() const { return flushed_lsn_; }
    [[nodiscard]] uint64_t GetLogFileSize() const;
    [[nodiscard]] const std::string& GetFilePath() const { return log_file_path_; }

    /// Truncate the log file (used after a successful checkpoint)
    void Truncate();

private:
    // =========================================================================
    // Internal Helpers
    // =========================================================================

    void FlushBuffer();

    // =========================================================================
    // Data Members
    // =========================================================================

    std::string log_file_path_;
    int fd_ = -1;

    LSN next_lsn_ = 0;         ///< Next LSN to assign
    LSN flushed_lsn_ = 0;      ///< All records up to this LSN are durable

    std::vector<uint8_t> log_buffer_;  ///< Write buffer
    uint32_t buffer_offset_ = 0;       ///< Current write position in buffer

    mutable std::mutex latch_;  ///< Protects buffer and LSN state
};

}  // namespace courtdb
