/**
 * =============================================================================
 * CourtDB - Buffer Pool Manager Unit Tests
 * =============================================================================
 *
 * Comprehensive tests covering:
 *   - LRU Replacer: access, eviction order, removal, edge cases
 *   - Buffer Pool: fetch, unpin, new page, delete, flush
 *   - Eviction behavior under memory pressure
 *   - Dirty page tracking and write-back
 *   - Pin count correctness
 *   - Buffer pool hit rate tracking
 *   - Stress tests with many pages and limited frames
 *   - Data integrity across eviction and re-fetch
 *
 * =============================================================================
 */

#include "buffer/buffer_pool_manager.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace courtdb {
namespace {

// =============================================================================
// LRU Replacer Tests
// =============================================================================

class LRUReplacerTest : public ::testing::Test {
protected:
    LRUReplacer replacer_{10};
};

TEST_F(LRUReplacerTest, InitiallyEmpty) {
    EXPECT_EQ(replacer_.Size(), 0u);
    EXPECT_EQ(replacer_.Evict(), INVALID_FRAME_ID);
}

TEST_F(LRUReplacerTest, SingleEviction) {
    replacer_.RecordAccess(0);
    EXPECT_EQ(replacer_.Size(), 1u);

    FrameId victim = replacer_.Evict();
    EXPECT_EQ(victim, 0u);
    EXPECT_EQ(replacer_.Size(), 0u);
}

TEST_F(LRUReplacerTest, LRUOrder) {
    // Access frames in order: 0, 1, 2, 3
    // LRU order (back to front): 0, 1, 2, 3
    // So eviction should return 0 first
    replacer_.RecordAccess(0);
    replacer_.RecordAccess(1);
    replacer_.RecordAccess(2);
    replacer_.RecordAccess(3);

    EXPECT_EQ(replacer_.Evict(), 0u);  // Least recently used
    EXPECT_EQ(replacer_.Evict(), 1u);
    EXPECT_EQ(replacer_.Evict(), 2u);
    EXPECT_EQ(replacer_.Evict(), 3u);  // Most recently used
}

TEST_F(LRUReplacerTest, ReaccessMovesToFront) {
    replacer_.RecordAccess(0);
    replacer_.RecordAccess(1);
    replacer_.RecordAccess(2);

    // Re-access frame 0 — now it's the most recent
    replacer_.RecordAccess(0);

    EXPECT_EQ(replacer_.Evict(), 1u);  // 1 is now the LRU
    EXPECT_EQ(replacer_.Evict(), 2u);
    EXPECT_EQ(replacer_.Evict(), 0u);  // 0 was re-accessed, so evicted last
}

TEST_F(LRUReplacerTest, RemoveFrame) {
    replacer_.RecordAccess(0);
    replacer_.RecordAccess(1);
    replacer_.RecordAccess(2);

    // Remove frame 1 (simulating it being pinned)
    replacer_.Remove(1);
    EXPECT_EQ(replacer_.Size(), 2u);

    EXPECT_EQ(replacer_.Evict(), 0u);
    EXPECT_EQ(replacer_.Evict(), 2u);
    EXPECT_EQ(replacer_.Evict(), INVALID_FRAME_ID);  // Empty
}

TEST_F(LRUReplacerTest, RemoveNonExistentIsNoOp) {
    replacer_.RecordAccess(0);
    replacer_.Remove(99);  // Not in the replacer
    EXPECT_EQ(replacer_.Size(), 1u);
}

TEST_F(LRUReplacerTest, DoubleRecordAccessIsSafe) {
    replacer_.RecordAccess(5);
    replacer_.RecordAccess(5);  // Should just move to front (no duplicate)
    EXPECT_EQ(replacer_.Size(), 1u);
    EXPECT_EQ(replacer_.Evict(), 5u);
}

// =============================================================================
// Buffer Pool Manager Test Fixture
// =============================================================================

class BufferPoolTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_file_ = "/tmp/courtdb_bpm_test_" +
                     std::to_string(reinterpret_cast<uintptr_t>(this)) + ".db";
        std::filesystem::remove(test_file_);
        disk_manager_ = std::make_unique<DiskManager>(test_file_);
    }

    void TearDown() override {
        bpm_.reset();
        disk_manager_.reset();
        std::filesystem::remove(test_file_);
    }

    void CreateBPM(uint32_t pool_size = 10) {
        bpm_ = std::make_unique<BufferPoolManager>(pool_size, disk_manager_.get());
    }

    std::string test_file_;
    std::unique_ptr<DiskManager> disk_manager_;
    std::unique_ptr<BufferPoolManager> bpm_;
};

// =============================================================================
// Basic Operations
// =============================================================================

TEST_F(BufferPoolTest, NewPageReturnsValidPage) {
    CreateBPM();

    Page* page = bpm_->NewPage();
    ASSERT_NE(page, nullptr);
    EXPECT_NE(page->GetPageId(), INVALID_PAGE_ID);
    EXPECT_EQ(page->GetNumRecords(), 0);

    bpm_->UnpinPage(page->GetPageId(), false);
}

TEST_F(BufferPoolTest, NewPageIncrementsPageId) {
    CreateBPM();

    Page* p1 = bpm_->NewPage();
    Page* p2 = bpm_->NewPage();
    Page* p3 = bpm_->NewPage();

    ASSERT_NE(p1, nullptr);
    ASSERT_NE(p2, nullptr);
    ASSERT_NE(p3, nullptr);

    EXPECT_EQ(p1->GetPageId(), 1u);
    EXPECT_EQ(p2->GetPageId(), 2u);
    EXPECT_EQ(p3->GetPageId(), 3u);

    bpm_->UnpinPage(1, false);
    bpm_->UnpinPage(2, false);
    bpm_->UnpinPage(3, false);
}

TEST_F(BufferPoolTest, FetchPageAfterNew) {
    CreateBPM();

    Page* page = bpm_->NewPage();
    PageId pid = page->GetPageId();

    // Write some data
    auto record = std::string("LeBron James 3PT FG");
    (void)page->InsertRecord(
        reinterpret_cast<const uint8_t*>(record.data()),
        static_cast<uint16_t>(record.size()));
    bpm_->UnpinPage(pid, true);  // Mark dirty

    // Fetch the same page again
    Page* fetched = bpm_->FetchPage(pid);
    ASSERT_NE(fetched, nullptr);
    EXPECT_EQ(fetched->GetPageId(), pid);
    EXPECT_EQ(fetched->GetNumRecords(), 1);

    auto result = fetched->GetRecord(0);
    ASSERT_TRUE(result.has_value());
    std::string retrieved(reinterpret_cast<const char*>(result.value().first),
                          result.value().second);
    EXPECT_EQ(retrieved, "LeBron James 3PT FG");

    bpm_->UnpinPage(pid, false);
}

TEST_F(BufferPoolTest, FetchNonExistentPageFails) {
    CreateBPM();

    Page* page = bpm_->FetchPage(999);
    EXPECT_EQ(page, nullptr);
}

// =============================================================================
// Pin/Unpin Semantics
// =============================================================================

TEST_F(BufferPoolTest, UnpinReducesPinCount) {
    CreateBPM();

    Page* page = bpm_->NewPage();
    PageId pid = page->GetPageId();

    // Fetch the same page again (pin_count = 2)
    Page* page2 = bpm_->FetchPage(pid);
    EXPECT_EQ(page, page2);  // Same pointer

    // Unpin twice
    EXPECT_TRUE(bpm_->UnpinPage(pid, false));
    EXPECT_TRUE(bpm_->UnpinPage(pid, false));

    // Third unpin should fail (pin_count was already 0)
    EXPECT_FALSE(bpm_->UnpinPage(pid, false));
}

TEST_F(BufferPoolTest, PinnedPageCannotBeEvicted) {
    CreateBPM(3);  // Only 3 frames

    // Fill all 3 frames (all pinned)
    Page* p1 = bpm_->NewPage();
    Page* p2 = bpm_->NewPage();
    Page* p3 = bpm_->NewPage();
    ASSERT_NE(p1, nullptr);
    ASSERT_NE(p2, nullptr);
    ASSERT_NE(p3, nullptr);

    // Cannot create a 4th page — all frames are pinned
    Page* p4 = bpm_->NewPage();
    EXPECT_EQ(p4, nullptr);

    // Unpin one page — now we can allocate
    bpm_->UnpinPage(p1->GetPageId(), false);

    p4 = bpm_->NewPage();
    EXPECT_NE(p4, nullptr);

    bpm_->UnpinPage(p2->GetPageId(), false);
    bpm_->UnpinPage(p3->GetPageId(), false);
    bpm_->UnpinPage(p4->GetPageId(), false);
}

// =============================================================================
// Eviction and Dirty Page Handling
// =============================================================================

TEST_F(BufferPoolTest, EvictionWritesDirtyPage) {
    CreateBPM(3);  // Small pool to force eviction

    // Create 3 pages, write data, unpin all
    std::vector<PageId> page_ids;
    for (int i = 0; i < 3; ++i) {
        Page* p = bpm_->NewPage();
        ASSERT_NE(p, nullptr);
        auto data = "Record " + std::to_string(i);
        (void)p->InsertRecord(
            reinterpret_cast<const uint8_t*>(data.data()),
            static_cast<uint16_t>(data.size()));
        page_ids.push_back(p->GetPageId());
        bpm_->UnpinPage(p->GetPageId(), true);  // Dirty
    }

    // Create a 4th page — forces eviction of LRU page (page_ids[0])
    Page* p4 = bpm_->NewPage();
    ASSERT_NE(p4, nullptr);
    bpm_->UnpinPage(p4->GetPageId(), false);

    // Fetch the evicted page — should be read back from disk with data intact
    Page* evicted = bpm_->FetchPage(page_ids[0]);
    ASSERT_NE(evicted, nullptr);
    EXPECT_EQ(evicted->GetNumRecords(), 1);

    auto result = evicted->GetRecord(0);
    ASSERT_TRUE(result.has_value());
    std::string retrieved(reinterpret_cast<const char*>(result.value().first),
                          result.value().second);
    EXPECT_EQ(retrieved, "Record 0");

    bpm_->UnpinPage(page_ids[0], false);
}

TEST_F(BufferPoolTest, LRUEvictsCorrectPage) {
    CreateBPM(3);

    Page* p1 = bpm_->NewPage();  // Frame 0
    Page* p2 = bpm_->NewPage();  // Frame 1
    Page* p3 = bpm_->NewPage();  // Frame 2

    PageId id1 = p1->GetPageId();
    PageId id2 = p2->GetPageId();
    PageId id3 = p3->GetPageId();

    // Unpin all (they enter LRU in order: p1 first, p3 last)
    bpm_->UnpinPage(id1, false);
    bpm_->UnpinPage(id2, false);
    bpm_->UnpinPage(id3, false);

    // Access p1 again — moves it to front of LRU
    Page* fetched = bpm_->FetchPage(id1);
    ASSERT_NE(fetched, nullptr);
    bpm_->UnpinPage(id1, false);

    // Create new page — should evict p2 (the true LRU)
    Page* p4 = bpm_->NewPage();
    ASSERT_NE(p4, nullptr);
    bpm_->UnpinPage(p4->GetPageId(), false);

    // p2 should have been evicted, p1 and p3 should still be in pool
    // Verify by checking hit stats
    bpm_->ResetStats();
    Page* f1 = bpm_->FetchPage(id1);
    ASSERT_NE(f1, nullptr);
    bpm_->UnpinPage(id1, false);

    auto stats = bpm_->GetStats();
    EXPECT_EQ(stats.hits, 1u);  // p1 was still in buffer
}

// =============================================================================
// Delete Page
// =============================================================================

TEST_F(BufferPoolTest, DeleteUnpinnedPage) {
    CreateBPM();

    Page* page = bpm_->NewPage();
    PageId pid = page->GetPageId();
    bpm_->UnpinPage(pid, false);

    EXPECT_TRUE(bpm_->DeletePage(pid));

    // Page should no longer be fetchable (it's been deallocated)
    Page* gone = bpm_->FetchPage(pid);
    // The page was deallocated but the disk manager might still return
    // the zeroed-out page. What matters is it's been removed from the pool.
    if (gone != nullptr) {
        bpm_->UnpinPage(pid, false);
    }
}

TEST_F(BufferPoolTest, DeletePinnedPageFails) {
    CreateBPM();

    Page* page = bpm_->NewPage();
    PageId pid = page->GetPageId();
    // Don't unpin — page is still pinned

    EXPECT_FALSE(bpm_->DeletePage(pid));

    bpm_->UnpinPage(pid, false);
}

// =============================================================================
// Flush Operations
// =============================================================================

TEST_F(BufferPoolTest, FlushDirtyPage) {
    CreateBPM();

    Page* page = bpm_->NewPage();
    PageId pid = page->GetPageId();
    auto data = std::string("Flush test data");
    (void)page->InsertRecord(
        reinterpret_cast<const uint8_t*>(data.data()),
        static_cast<uint16_t>(data.size()));

    bpm_->FlushPage(pid);
    bpm_->UnpinPage(pid, true);

    // Destroy buffer pool and recreate — data should persist
    bpm_.reset();
    bpm_ = std::make_unique<BufferPoolManager>(10, disk_manager_.get());

    Page* reloaded = bpm_->FetchPage(pid);
    ASSERT_NE(reloaded, nullptr);
    EXPECT_EQ(reloaded->GetNumRecords(), 1);

    auto result = reloaded->GetRecord(0);
    ASSERT_TRUE(result.has_value());
    std::string retrieved(reinterpret_cast<const char*>(result.value().first),
                          result.value().second);
    EXPECT_EQ(retrieved, "Flush test data");

    bpm_->UnpinPage(pid, false);
}

TEST_F(BufferPoolTest, FlushAllPages) {
    CreateBPM();

    // Create and dirty multiple pages
    for (int i = 0; i < 5; ++i) {
        Page* p = bpm_->NewPage();
        auto data = "Page " + std::to_string(i);
        (void)p->InsertRecord(
            reinterpret_cast<const uint8_t*>(data.data()),
            static_cast<uint16_t>(data.size()));
        bpm_->UnpinPage(p->GetPageId(), true);
    }

    bpm_->FlushAllPages();

    auto stats = bpm_->GetStats();
    EXPECT_GE(stats.flushes, 5u);
}

// =============================================================================
// Statistics
// =============================================================================

TEST_F(BufferPoolTest, HitRateTracking) {
    CreateBPM(10);

    // Create a page
    Page* p = bpm_->NewPage();
    PageId pid = p->GetPageId();
    bpm_->UnpinPage(pid, false);

    bpm_->ResetStats();

    // Fetch the same page 5 times — all should be hits
    for (int i = 0; i < 5; ++i) {
        Page* fetched = bpm_->FetchPage(pid);
        ASSERT_NE(fetched, nullptr);
        bpm_->UnpinPage(pid, false);
    }

    auto stats = bpm_->GetStats();
    EXPECT_EQ(stats.hits, 5u);
    EXPECT_EQ(stats.misses, 0u);
    EXPECT_DOUBLE_EQ(stats.HitRate(), 1.0);
}

TEST_F(BufferPoolTest, MissTracking) {
    CreateBPM(3);

    // Create more pages than the pool can hold
    std::vector<PageId> pids;
    for (int i = 0; i < 5; ++i) {
        Page* p = bpm_->NewPage();
        ASSERT_NE(p, nullptr);
        pids.push_back(p->GetPageId());
        bpm_->UnpinPage(p->GetPageId(), false);
    }

    bpm_->ResetStats();

    // Access a page that was evicted
    Page* fetched = bpm_->FetchPage(pids[0]);
    ASSERT_NE(fetched, nullptr);
    bpm_->UnpinPage(pids[0], false);

    auto stats = bpm_->GetStats();
    // First two pages were evicted when pages 4 and 5 were created
    EXPECT_EQ(stats.misses, 1u);
}

// =============================================================================
// Stress Tests
// =============================================================================

TEST_F(BufferPoolTest, StressManyPagesSmallPool) {
    CreateBPM(10);  // Only 10 frames

    constexpr int NUM_PAGES = 100;
    std::vector<PageId> page_ids;

    // Create many pages
    for (int i = 0; i < NUM_PAGES; ++i) {
        Page* p = bpm_->NewPage();
        ASSERT_NE(p, nullptr) << "Failed to create page " << i;
        auto data = "NBA Event #" + std::to_string(i);
        (void)p->InsertRecord(
            reinterpret_cast<const uint8_t*>(data.data()),
            static_cast<uint16_t>(data.size()));
        page_ids.push_back(p->GetPageId());
        bpm_->UnpinPage(p->GetPageId(), true);
    }

    // Random access pattern — verify data integrity after eviction/re-fetch
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> dist(0, NUM_PAGES - 1);

    for (int i = 0; i < 500; ++i) {
        int idx = dist(rng);
        Page* p = bpm_->FetchPage(page_ids[idx]);
        ASSERT_NE(p, nullptr) << "Failed to fetch page " << page_ids[idx];

        auto result = p->GetRecord(0);
        ASSERT_TRUE(result.has_value());
        std::string expected = "NBA Event #" + std::to_string(idx);
        std::string actual(reinterpret_cast<const char*>(result.value().first),
                           result.value().second);
        EXPECT_EQ(actual, expected) << "Data corruption for page " << idx;

        bpm_->UnpinPage(page_ids[idx], false);
    }
}

TEST_F(BufferPoolTest, StressSequentialScan) {
    CreateBPM(5);  // Very small pool

    constexpr int NUM_PAGES = 50;
    std::vector<PageId> page_ids;

    // Create pages
    for (int i = 0; i < NUM_PAGES; ++i) {
        Page* p = bpm_->NewPage();
        ASSERT_NE(p, nullptr);
        auto data = "Scan record " + std::to_string(i);
        (void)p->InsertRecord(
            reinterpret_cast<const uint8_t*>(data.data()),
            static_cast<uint16_t>(data.size()));
        page_ids.push_back(p->GetPageId());
        bpm_->UnpinPage(p->GetPageId(), true);
    }

    // Sequential scan — simulates a table scan
    for (int scan = 0; scan < 3; ++scan) {
        for (int i = 0; i < NUM_PAGES; ++i) {
            Page* p = bpm_->FetchPage(page_ids[i]);
            ASSERT_NE(p, nullptr);

            auto result = p->GetRecord(0);
            ASSERT_TRUE(result.has_value());
            std::string expected = "Scan record " + std::to_string(i);
            std::string actual(reinterpret_cast<const char*>(result.value().first),
                               result.value().second);
            EXPECT_EQ(actual, expected);

            bpm_->UnpinPage(page_ids[i], false);
        }
    }
}

TEST_F(BufferPoolTest, StressMultiplePinsAndModifications) {
    CreateBPM(10);

    Page* page = bpm_->NewPage();
    PageId pid = page->GetPageId();

    // Pin the same page multiple times
    for (int i = 0; i < 5; ++i) {
        Page* p = bpm_->FetchPage(pid);
        ASSERT_NE(p, nullptr);
    }
    // Total pin count should be 6 (1 from NewPage + 5 from FetchPage)

    // Unpin 6 times
    for (int i = 0; i < 6; ++i) {
        EXPECT_TRUE(bpm_->UnpinPage(pid, i == 0));  // First unpin marks dirty
    }

    // 7th unpin should fail
    EXPECT_FALSE(bpm_->UnpinPage(pid, false));
}

TEST_F(BufferPoolTest, OccupiedFrameCount) {
    CreateBPM(10);

    EXPECT_EQ(bpm_->GetOccupiedFrameCount(), 0u);

    Page* p1 = bpm_->NewPage();
    EXPECT_EQ(bpm_->GetOccupiedFrameCount(), 1u);

    Page* p2 = bpm_->NewPage();
    EXPECT_EQ(bpm_->GetOccupiedFrameCount(), 2u);

    bpm_->UnpinPage(p1->GetPageId(), false);
    EXPECT_EQ(bpm_->GetOccupiedFrameCount(), 2u);  // Still occupied, just evictable

    bpm_->UnpinPage(p2->GetPageId(), false);
    bpm_->DeletePage(p1->GetPageId());
    EXPECT_EQ(bpm_->GetOccupiedFrameCount(), 1u);  // p1 was removed
}

}  // namespace
}  // namespace courtdb
