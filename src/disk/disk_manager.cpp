/**
 * =============================================================================
 * CourtDB - Disk Manager Implementation
 * =============================================================================
 *
 * Implements page-level I/O using POSIX file operations.
 *
 * Key implementation details:
 *   - Uses pread/pwrite for atomic positioned I/O (no lseek races)
 *   - Header page (page 0) is read into memory on open, written on mutation
 *   - Free list is maintained as a linked list through deallocated pages
 *   - File is extended in full PAGE_SIZE increments
 *
 * =============================================================================
 */

#include "disk/disk_manager.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace courtdb {

// =============================================================================
// Construction / Destruction
// =============================================================================

DiskManager::DiskManager(const std::string& db_file_path)
    : file_path_(db_file_path) {

    // Try to open existing file first
    fd_ = ::open(file_path_.c_str(), O_RDWR, 0644);

    if (fd_ < 0) {
        if (errno == ENOENT) {
            // File doesn't exist — create it
            fd_ = ::open(file_path_.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
            if (fd_ < 0) {
                throw std::runtime_error(
                    "DiskManager: failed to create file '" + file_path_ +
                    "': " + std::strerror(errno));
            }
            InitializeNewFile();
        } else {
            throw std::runtime_error(
                "DiskManager: failed to open file '" + file_path_ +
                "': " + std::strerror(errno));
        }
    } else {
        // Existing file — read the header
        ReadHeader();

        // Validate magic number
        if (header_.magic != DatabaseHeader::MAGIC) {
            ::close(fd_);
            fd_ = -1;
            throw std::runtime_error(
                "DiskManager: invalid database file (bad magic number)");
        }
    }
}

DiskManager::~DiskManager() {
    if (fd_ >= 0) {
        // Flush header before closing
        WriteHeader();
        ::fsync(fd_);
        ::close(fd_);
        fd_ = -1;
    }
}

// =============================================================================
// Page I/O
// =============================================================================

bool DiskManager::ReadPage(PageId page_id, uint8_t* buffer) {
    if (page_id >= header_.page_count) {
        return false;  // Page doesn't exist
    }

    uint64_t offset = PageOffset(page_id);
    ssize_t bytes_read = ::pread(fd_, buffer, PAGE_SIZE, static_cast<off_t>(offset));

    if (bytes_read != PAGE_SIZE) {
        return false;
    }

    ++num_reads_;
    return true;
}

bool DiskManager::WritePage(PageId page_id, const uint8_t* buffer) {
    if (page_id >= header_.page_count) {
        return false;  // Page doesn't exist
    }

    uint64_t offset = PageOffset(page_id);
    ssize_t bytes_written = ::pwrite(fd_, buffer, PAGE_SIZE, static_cast<off_t>(offset));

    if (bytes_written != PAGE_SIZE) {
        return false;
    }

    ++num_writes_;
    return true;
}

// =============================================================================
// Page Allocation
// =============================================================================

PageId DiskManager::AllocatePage() {
    PageId new_page_id;

    if (header_.free_list_head != INVALID_PAGE_ID) {
        // Reuse a page from the free list
        new_page_id = header_.free_list_head;

        // Read the free page to get the next pointer in the chain
        uint8_t buffer[PAGE_SIZE];
        if (ReadPage(new_page_id, buffer)) {
            // First 4 bytes of a free page store the next free page ID
            PageId next_free;
            std::memcpy(&next_free, buffer, sizeof(PageId));
            header_.free_list_head = next_free;
            header_.free_page_count--;
        }

        // Zero out the reused page
        std::memset(buffer, 0, PAGE_SIZE);
        WritePage(new_page_id, buffer);
    } else {
        // Extend the file by one page
        new_page_id = header_.page_count;
        header_.page_count++;

        // Extend the file
        uint8_t buffer[PAGE_SIZE];
        std::memset(buffer, 0, PAGE_SIZE);
        uint64_t offset = PageOffset(new_page_id);
        ssize_t written = ::pwrite(fd_, buffer, PAGE_SIZE, static_cast<off_t>(offset));

        if (written != PAGE_SIZE) {
            // Rollback
            header_.page_count--;
            throw std::runtime_error("DiskManager: failed to extend file");
        }
    }

    // Persist the updated header
    WriteHeader();
    return new_page_id;
}

bool DiskManager::DeallocatePage(PageId page_id) {
    if (page_id == 0 || page_id >= header_.page_count) {
        return false;  // Cannot deallocate header page or non-existent page
    }

    // Write the current free list head into the first 4 bytes of this page
    uint8_t buffer[PAGE_SIZE];
    std::memset(buffer, 0, PAGE_SIZE);
    std::memcpy(buffer, &header_.free_list_head, sizeof(PageId));

    if (!WritePage(page_id, buffer)) {
        return false;
    }

    // Update free list head to point to this page
    header_.free_list_head = page_id;
    header_.free_page_count++;

    WriteHeader();
    return true;
}

// =============================================================================
// Durability
// =============================================================================

void DiskManager::Flush() {
    if (fd_ >= 0) {
        ::fsync(fd_);
    }
}

// =============================================================================
// Accessors
// =============================================================================

uint64_t DiskManager::GetFileSize() const {
    struct stat st;
    if (::fstat(fd_, &st) != 0) {
        return 0;
    }
    return static_cast<uint64_t>(st.st_size);
}

// =============================================================================
// Private Helpers
// =============================================================================

void DiskManager::ReadHeader() {
    uint8_t buffer[PAGE_SIZE];
    ssize_t bytes_read = ::pread(fd_, buffer, PAGE_SIZE, 0);

    if (bytes_read != PAGE_SIZE) {
        throw std::runtime_error("DiskManager: failed to read header page");
    }

    // Deserialize header from buffer
    uint16_t pos = 0;
    std::memcpy(&header_.magic, buffer + pos, sizeof(header_.magic));
    pos += sizeof(header_.magic);

    std::memcpy(&header_.version, buffer + pos, sizeof(header_.version));
    pos += sizeof(header_.version);

    std::memcpy(&header_.page_count, buffer + pos, sizeof(header_.page_count));
    pos += sizeof(header_.page_count);

    std::memcpy(&header_.free_list_head, buffer + pos, sizeof(header_.free_list_head));
    pos += sizeof(header_.free_list_head);

    std::memcpy(&header_.free_page_count, buffer + pos, sizeof(header_.free_page_count));
}

void DiskManager::WriteHeader() {
    uint8_t buffer[PAGE_SIZE];
    std::memset(buffer, 0, PAGE_SIZE);

    // Serialize header into buffer
    uint16_t pos = 0;
    std::memcpy(buffer + pos, &header_.magic, sizeof(header_.magic));
    pos += sizeof(header_.magic);

    std::memcpy(buffer + pos, &header_.version, sizeof(header_.version));
    pos += sizeof(header_.version);

    std::memcpy(buffer + pos, &header_.page_count, sizeof(header_.page_count));
    pos += sizeof(header_.page_count);

    std::memcpy(buffer + pos, &header_.free_list_head, sizeof(header_.free_list_head));
    pos += sizeof(header_.free_list_head);

    std::memcpy(buffer + pos, &header_.free_page_count, sizeof(header_.free_page_count));

    // Write to page 0
    ssize_t written = ::pwrite(fd_, buffer, PAGE_SIZE, 0);
    if (written != PAGE_SIZE) {
        throw std::runtime_error("DiskManager: failed to write header page");
    }
}

void DiskManager::InitializeNewFile() {
    // Set up default header
    header_ = DatabaseHeader{};

    // Write the header page (page 0)
    WriteHeader();
}

}  // namespace courtdb
