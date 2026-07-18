/**
 * =============================================================================
 * CourtDB - Log Manager Implementation
 * =============================================================================
 *
 * The WAL (Write-Ahead Log) manager provides:
 *   - Buffered append-only writes to the log file
 *   - LSN assignment (LSN = byte offset in the file)
 *   - Group commit (multiple records per fsync)
 *   - Sequential read iterator for recovery
 *
 * File format:
 *   The log file is a sequence of serialized LogRecords, back to back.
 *   Each record starts with a 4-byte total_size field, enabling sequential parsing.
 *
 * =============================================================================
 */

#include "recovery/log_manager.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cassert>
#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace courtdb {

// =============================================================================
// Construction / Destruction
// =============================================================================

LogManager::LogManager(const std::string& log_file_path)
    : log_file_path_(log_file_path),
      log_buffer_(DEFAULT_LOG_BUFFER_SIZE) {

    fd_ = ::open(log_file_path_.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd_ < 0) {
        throw std::runtime_error("LogManager: failed to open log file '" +
                                 log_file_path_ + "': " + std::strerror(errno));
    }

    // Determine current file size (existing log content)
    struct stat st;
    if (::fstat(fd_, &st) == 0) {
        next_lsn_ = static_cast<LSN>(st.st_size);
        flushed_lsn_ = next_lsn_;
    }
}

LogManager::~LogManager() {
    if (fd_ >= 0) {
        // Flush remaining buffer
        if (buffer_offset_ > 0) {
            FlushBuffer();
        }
        ::close(fd_);
        fd_ = -1;
    }
}

// =============================================================================
// Write Operations
// =============================================================================

LSN LogManager::AppendLogRecord(LogRecord& record) {
    std::lock_guard<std::mutex> lock(latch_);

    // Serialize the record
    auto bytes = record.Serialize();
    uint32_t record_size = static_cast<uint32_t>(bytes.size());

    // Assign LSN (byte offset where this record will live)
    LSN assigned_lsn = next_lsn_;
    next_lsn_ += record_size;

    // Check if buffer can hold this record
    if (buffer_offset_ + record_size > log_buffer_.size()) {
        FlushBuffer();
    }

    // If record is larger than the entire buffer, write directly
    if (record_size > log_buffer_.size()) {
        ssize_t written = ::pwrite(fd_, bytes.data(), record_size,
                                   static_cast<off_t>(assigned_lsn));
        if (written != static_cast<ssize_t>(record_size)) {
            throw std::runtime_error("LogManager: failed to write large log record");
        }
        flushed_lsn_ = next_lsn_;
    } else {
        // Append to buffer
        std::memcpy(log_buffer_.data() + buffer_offset_, bytes.data(), record_size);
        buffer_offset_ += record_size;
    }

    return assigned_lsn;
}

void LogManager::Flush() {
    std::lock_guard<std::mutex> lock(latch_);
    if (buffer_offset_ > 0) {
        FlushBuffer();
    }
    ::fsync(fd_);
}

void LogManager::FlushUpTo(LSN lsn) {
    std::lock_guard<std::mutex> lock(latch_);
    if (lsn >= flushed_lsn_ && buffer_offset_ > 0) {
        FlushBuffer();
        ::fsync(fd_);
    }
}

// =============================================================================
// Read Operations
// =============================================================================

LogManager::LogIterator LogManager::Begin() {
    // Flush buffer first so all records are on disk
    Flush();
    return LogIterator(fd_, GetLogFileSize());
}

LogManager::LogIterator LogManager::BeginAt(LSN start_lsn) {
    Flush();
    LogIterator it(fd_, GetLogFileSize());
    it.current_offset_ = start_lsn;
    it.ReadCurrentRecord();
    return it;
}

// =============================================================================
// Accessors
// =============================================================================

uint64_t LogManager::GetLogFileSize() const {
    struct stat st;
    if (::fstat(fd_, &st) != 0) return 0;
    return static_cast<uint64_t>(st.st_size);
}

void LogManager::Truncate() {
    std::lock_guard<std::mutex> lock(latch_);
    buffer_offset_ = 0;
    ::ftruncate(fd_, 0);
    next_lsn_ = 0;
    flushed_lsn_ = 0;
}

// =============================================================================
// Private Helpers
// =============================================================================

void LogManager::FlushBuffer() {
    if (buffer_offset_ == 0) return;

    // Calculate the file offset for this write
    LSN write_offset = flushed_lsn_;
    ssize_t written = ::pwrite(fd_, log_buffer_.data(), buffer_offset_,
                               static_cast<off_t>(write_offset));

    if (written != static_cast<ssize_t>(buffer_offset_)) {
        throw std::runtime_error("LogManager: failed to flush log buffer");
    }

    flushed_lsn_ += buffer_offset_;
    buffer_offset_ = 0;
}

// =============================================================================
// LogIterator Implementation
// =============================================================================

LogManager::LogIterator::LogIterator(int fd, uint64_t file_size)
    : fd_(fd), file_size_(file_size), current_offset_(0) {
    if (file_size_ > 0) {
        ReadCurrentRecord();
    }
}

void LogManager::LogIterator::Next() {
    if (!valid_) return;

    current_offset_ += current_.GetTotalSize();
    if (current_offset_ >= file_size_) {
        valid_ = false;
        return;
    }

    ReadCurrentRecord();
}

void LogManager::LogIterator::ReadCurrentRecord() {
    if (current_offset_ >= file_size_) {
        valid_ = false;
        return;
    }

    // Read the total_size field first
    uint32_t total_size = 0;
    ssize_t read_bytes = ::pread(fd_, &total_size, 4,
                                 static_cast<off_t>(current_offset_));
    if (read_bytes != 4 || total_size == 0) {
        valid_ = false;
        return;
    }

    // Sanity check
    if (current_offset_ + total_size > file_size_) {
        valid_ = false;
        return;
    }

    // Read the full record
    std::vector<uint8_t> buf(total_size);
    read_bytes = ::pread(fd_, buf.data(), total_size,
                         static_cast<off_t>(current_offset_));
    if (read_bytes != static_cast<ssize_t>(total_size)) {
        valid_ = false;
        return;
    }

    current_ = LogRecord::Deserialize(buf.data(), total_size);
    valid_ = (current_.GetType() != LogRecordType::kInvalid);
}

}  // namespace courtdb
