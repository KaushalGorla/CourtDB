/**
 * =============================================================================
 * CourtDB - Disk Manager Unit Tests
 * =============================================================================
 *
 * Comprehensive tests for the disk manager covering:
 *   - File creation and opening
 *   - Page read/write round-trips
 *   - Page allocation (file extension)
 *   - Page deallocation and free list
 *   - Free list reuse
 *   - Multiple allocations and deallocations
 *   - Persistence across DiskManager instances
 *   - Error handling (invalid page IDs)
 *   - Stress test with many pages
 *
 * =============================================================================
 */

#include "disk/disk_manager.h"

#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

namespace courtdb {
namespace {

// =============================================================================
// Test Fixture
// =============================================================================

class DiskManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Use a unique file per test to avoid interference
        test_file_ = "/tmp/courtdb_test_" +
                     std::to_string(reinterpret_cast<uintptr_t>(this)) + ".db";
        // Clean up any leftover file
        std::filesystem::remove(test_file_);
    }

    void TearDown() override {
        dm_.reset();  // Close the file before removing
        std::filesystem::remove(test_file_);
    }

    std::string test_file_;
    std::unique_ptr<DiskManager> dm_;
};

// =============================================================================
// File Creation and Opening
// =============================================================================

TEST_F(DiskManagerTest, CreateNewFile) {
    dm_ = std::make_unique<DiskManager>(test_file_);

    EXPECT_EQ(dm_->GetPageCount(), 1u);  // Header page only
    EXPECT_EQ(dm_->GetFreePageCount(), 0u);
    EXPECT_TRUE(std::filesystem::exists(test_file_));
    EXPECT_EQ(dm_->GetFileSize(), PAGE_SIZE);  // One page (header)
}

TEST_F(DiskManagerTest, ReopenExistingFile) {
    {
        DiskManager dm1(test_file_);
        dm1.AllocatePage();  // Allocate page 1
        dm1.AllocatePage();  // Allocate page 2
    }  // dm1 destructor flushes and closes

    // Reopen
    dm_ = std::make_unique<DiskManager>(test_file_);
    EXPECT_EQ(dm_->GetPageCount(), 3u);  // Header + 2 data pages
}

TEST_F(DiskManagerTest, InvalidFileThrows) {
    // Create a file with garbage (wrong magic)
    {
        std::ofstream f(test_file_, std::ios::binary);
        uint8_t garbage[PAGE_SIZE] = {};
        garbage[0] = 0xFF;  // Wrong magic
        f.write(reinterpret_cast<char*>(garbage), PAGE_SIZE);
    }

    EXPECT_THROW(std::make_unique<DiskManager>(test_file_), std::runtime_error);
}

// =============================================================================
// Page Read/Write
// =============================================================================

TEST_F(DiskManagerTest, WriteAndReadPage) {
    dm_ = std::make_unique<DiskManager>(test_file_);

    PageId pid = dm_->AllocatePage();
    EXPECT_EQ(pid, 1u);  // First allocated page after header

    // Write a pattern
    uint8_t write_buf[PAGE_SIZE];
    std::memset(write_buf, 0xAB, PAGE_SIZE);
    write_buf[0] = 0x01;
    write_buf[PAGE_SIZE - 1] = 0x02;

    EXPECT_TRUE(dm_->WritePage(pid, write_buf));

    // Read it back
    uint8_t read_buf[PAGE_SIZE];
    std::memset(read_buf, 0, PAGE_SIZE);
    EXPECT_TRUE(dm_->ReadPage(pid, read_buf));

    EXPECT_EQ(std::memcmp(write_buf, read_buf, PAGE_SIZE), 0);
}

TEST_F(DiskManagerTest, ReadInvalidPageFails) {
    dm_ = std::make_unique<DiskManager>(test_file_);

    uint8_t buf[PAGE_SIZE];
    EXPECT_FALSE(dm_->ReadPage(999, buf));  // Page doesn't exist
}

TEST_F(DiskManagerTest, WriteInvalidPageFails) {
    dm_ = std::make_unique<DiskManager>(test_file_);

    uint8_t buf[PAGE_SIZE] = {};
    EXPECT_FALSE(dm_->WritePage(999, buf));  // Page doesn't exist
}

// =============================================================================
// Page Allocation
// =============================================================================

TEST_F(DiskManagerTest, AllocateMultiplePages) {
    dm_ = std::make_unique<DiskManager>(test_file_);

    PageId p1 = dm_->AllocatePage();
    PageId p2 = dm_->AllocatePage();
    PageId p3 = dm_->AllocatePage();

    EXPECT_EQ(p1, 1u);
    EXPECT_EQ(p2, 2u);
    EXPECT_EQ(p3, 3u);
    EXPECT_EQ(dm_->GetPageCount(), 4u);
    EXPECT_EQ(dm_->GetFileSize(), 4 * PAGE_SIZE);
}

TEST_F(DiskManagerTest, AllocatedPageIsZeroed) {
    dm_ = std::make_unique<DiskManager>(test_file_);

    PageId pid = dm_->AllocatePage();

    uint8_t buf[PAGE_SIZE];
    dm_->ReadPage(pid, buf);

    // Freshly allocated page should be all zeros
    uint8_t zeroes[PAGE_SIZE] = {};
    EXPECT_EQ(std::memcmp(buf, zeroes, PAGE_SIZE), 0);
}

// =============================================================================
// Page Deallocation and Free List
// =============================================================================

TEST_F(DiskManagerTest, DeallocatePage) {
    dm_ = std::make_unique<DiskManager>(test_file_);

    PageId p1 = dm_->AllocatePage();
    EXPECT_EQ(dm_->GetFreePageCount(), 0u);

    EXPECT_TRUE(dm_->DeallocatePage(p1));
    EXPECT_EQ(dm_->GetFreePageCount(), 1u);
}

TEST_F(DiskManagerTest, DeallocateHeaderPageFails) {
    dm_ = std::make_unique<DiskManager>(test_file_);
    EXPECT_FALSE(dm_->DeallocatePage(0));  // Cannot deallocate header
}

TEST_F(DiskManagerTest, DeallocateInvalidPageFails) {
    dm_ = std::make_unique<DiskManager>(test_file_);
    EXPECT_FALSE(dm_->DeallocatePage(999));  // Doesn't exist
}

TEST_F(DiskManagerTest, FreeListReuse) {
    dm_ = std::make_unique<DiskManager>(test_file_);

    /*PageId p1 =*/ dm_->AllocatePage();  // Page 1
    PageId p2 = dm_->AllocatePage();     // Page 2
    /*PageId p3 =*/ dm_->AllocatePage(); // Page 3

    // Deallocate page 2 (middle of the file)
    dm_->DeallocatePage(p2);
    EXPECT_EQ(dm_->GetFreePageCount(), 1u);

    // Next allocation should reuse page 2
    PageId p4 = dm_->AllocatePage();
    EXPECT_EQ(p4, p2);  // Reused!
    EXPECT_EQ(dm_->GetFreePageCount(), 0u);
    EXPECT_EQ(dm_->GetPageCount(), 4u);  // File didn't grow
}

TEST_F(DiskManagerTest, FreeListMultipleReuse) {
    dm_ = std::make_unique<DiskManager>(test_file_);

    // Allocate 5 pages
    std::vector<PageId> pages;
    for (int i = 0; i < 5; ++i) {
        pages.push_back(dm_->AllocatePage());
    }

    // Deallocate pages 2, 4, 1 (in that order — builds a LIFO chain)
    dm_->DeallocatePage(pages[1]);  // page 2
    dm_->DeallocatePage(pages[3]);  // page 4
    dm_->DeallocatePage(pages[0]);  // page 1
    EXPECT_EQ(dm_->GetFreePageCount(), 3u);

    // Allocations should come from free list (LIFO order)
    PageId r1 = dm_->AllocatePage();
    PageId r2 = dm_->AllocatePage();
    PageId r3 = dm_->AllocatePage();

    EXPECT_EQ(r1, pages[0]);  // Last deallocated = first reallocated
    EXPECT_EQ(r2, pages[3]);
    EXPECT_EQ(r3, pages[1]);
    EXPECT_EQ(dm_->GetFreePageCount(), 0u);
}

// =============================================================================
// Persistence
// =============================================================================

TEST_F(DiskManagerTest, DataPersistsAcrossReopen) {
    PageId pid;
    {
        DiskManager dm(test_file_);
        pid = dm.AllocatePage();

        uint8_t buf[PAGE_SIZE];
        std::memset(buf, 0xCD, PAGE_SIZE);
        dm.WritePage(pid, buf);
    }

    // Reopen and verify data
    dm_ = std::make_unique<DiskManager>(test_file_);
    uint8_t read_buf[PAGE_SIZE];
    EXPECT_TRUE(dm_->ReadPage(pid, read_buf));

    for (uint32_t i = 0; i < PAGE_SIZE; ++i) {
        EXPECT_EQ(read_buf[i], 0xCD) << "Mismatch at byte " << i;
    }
}

TEST_F(DiskManagerTest, FreeListPersistsAcrossReopen) {
    PageId freed_page;
    {
        DiskManager dm(test_file_);
        dm.AllocatePage();          // Page 1
        freed_page = dm.AllocatePage();  // Page 2
        dm.AllocatePage();          // Page 3

        dm.DeallocatePage(freed_page);
    }

    // Reopen — free list should be intact
    dm_ = std::make_unique<DiskManager>(test_file_);
    EXPECT_EQ(dm_->GetFreePageCount(), 1u);

    // Allocate should reuse the freed page
    PageId reused = dm_->AllocatePage();
    EXPECT_EQ(reused, freed_page);
}

// =============================================================================
// Flush
// =============================================================================

TEST_F(DiskManagerTest, FlushDoesNotCrash) {
    dm_ = std::make_unique<DiskManager>(test_file_);
    dm_->AllocatePage();
    dm_->Flush();  // Should not throw or crash
}

// =============================================================================
// Stress Test
// =============================================================================

TEST_F(DiskManagerTest, StressAllocateMany) {
    dm_ = std::make_unique<DiskManager>(test_file_);

    constexpr int NUM_PAGES = 1000;
    std::vector<PageId> pages;
    pages.reserve(NUM_PAGES);

    // Allocate many pages
    for (int i = 0; i < NUM_PAGES; ++i) {
        PageId pid = dm_->AllocatePage();
        pages.push_back(pid);
        EXPECT_EQ(pid, static_cast<PageId>(i + 1));
    }

    EXPECT_EQ(dm_->GetPageCount(), NUM_PAGES + 1);

    // Write unique data to each page
    for (int i = 0; i < NUM_PAGES; ++i) {
        uint8_t buf[PAGE_SIZE];
        std::memset(buf, static_cast<uint8_t>(i & 0xFF), PAGE_SIZE);
        EXPECT_TRUE(dm_->WritePage(pages[i], buf));
    }

    // Verify all pages
    for (int i = 0; i < NUM_PAGES; ++i) {
        uint8_t buf[PAGE_SIZE];
        EXPECT_TRUE(dm_->ReadPage(pages[i], buf));
        EXPECT_EQ(buf[0], static_cast<uint8_t>(i & 0xFF));
        EXPECT_EQ(buf[PAGE_SIZE - 1], static_cast<uint8_t>(i & 0xFF));
    }
}

TEST_F(DiskManagerTest, StressAllocateDeallocateCycles) {
    dm_ = std::make_unique<DiskManager>(test_file_);

    // Allocate and deallocate in cycles
    for (int cycle = 0; cycle < 10; ++cycle) {
        std::vector<PageId> batch;
        for (int i = 0; i < 50; ++i) {
            batch.push_back(dm_->AllocatePage());
        }
        // Deallocate half
        for (int i = 0; i < 25; ++i) {
            dm_->DeallocatePage(batch[i]);
        }
    }

    // The file should not have grown unboundedly due to reuse
    // 10 cycles * 50 pages = 500 allocated, 250 freed
    // Net: 250 live pages + some reuse from earlier cycles
    EXPECT_LE(dm_->GetPageCount(), 300u + 1u);  // Conservative upper bound with reuse
}

}  // namespace
}  // namespace courtdb
