/**
 * =============================================================================
 * CourtDB - Page Unit Tests
 * =============================================================================
 *
 * Comprehensive tests for the slotted page implementation covering:
 *   - Basic insert/get/delete operations
 *   - Variable-length records
 *   - Page full detection
 *   - Slot reuse after deletion
 *   - Compaction correctness
 *   - Serialization/deserialization round-trip
 *   - Edge cases (empty records, maximum-size records)
 *   - Stress testing with many small records
 *
 * =============================================================================
 */

#include "storage/page.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <numeric>
#include <random>
#include <vector>

namespace courtdb {
namespace {

// =============================================================================
// Helper Functions
// =============================================================================

/// Create a test record filled with a repeating byte pattern
std::vector<uint8_t> MakeRecord(uint16_t size, uint8_t fill = 0xAB) {
    std::vector<uint8_t> record(size, fill);
    return record;
}

/// Create a record containing a recognizable string
std::vector<uint8_t> MakeStringRecord(const std::string& s) {
    return std::vector<uint8_t>(s.begin(), s.end());
}

// =============================================================================
// Basic Operations
// =============================================================================

TEST(PageTest, ConstructorInitializesEmpty) {
    Page page(42);
    EXPECT_EQ(page.GetPageId(), 42u);
    EXPECT_EQ(page.GetNumRecords(), 0);
    EXPECT_EQ(page.GetNumSlots(), 0);
    EXPECT_EQ(page.GetNextPageId(), INVALID_PAGE_ID);
}

TEST(PageTest, InsertSingleRecord) {
    Page page(1);
    auto record = MakeRecord(100);

    auto slot = page.InsertRecord(record.data(), record.size());
    ASSERT_TRUE(slot.has_value());
    EXPECT_EQ(slot.value(), 0);
    EXPECT_EQ(page.GetNumRecords(), 1);
    EXPECT_EQ(page.GetNumSlots(), 1);
}

TEST(PageTest, InsertAndRetrieveRecord) {
    Page page(1);
    auto record = MakeStringRecord("LeBron James 3-pointer");

    auto slot = page.InsertRecord(record.data(), record.size());
    ASSERT_TRUE(slot.has_value());

    auto result = page.GetRecord(slot.value());
    ASSERT_TRUE(result.has_value());

    auto [data, length] = result.value();
    EXPECT_EQ(length, record.size());
    EXPECT_EQ(std::memcmp(data, record.data(), length), 0);
}

TEST(PageTest, InsertMultipleRecords) {
    Page page(1);

    std::vector<std::string> events = {
        "Stephen Curry 3PT FG",
        "Kevin Durant Dunk",
        "Giannis Free Throw Made",
        "Luka Doncic Assist"
    };

    for (size_t i = 0; i < events.size(); ++i) {
        auto record = MakeStringRecord(events[i]);
        auto slot = page.InsertRecord(record.data(), record.size());
        ASSERT_TRUE(slot.has_value());
        EXPECT_EQ(slot.value(), static_cast<uint16_t>(i));
    }

    EXPECT_EQ(page.GetNumRecords(), 4);

    // Verify all records
    for (size_t i = 0; i < events.size(); ++i) {
        auto result = page.GetRecord(static_cast<uint16_t>(i));
        ASSERT_TRUE(result.has_value());
        auto [data, length] = result.value();
        std::string retrieved(reinterpret_cast<const char*>(data), length);
        EXPECT_EQ(retrieved, events[i]);
    }
}

// =============================================================================
// Deletion
// =============================================================================

TEST(PageTest, DeleteRecord) {
    Page page(1);
    auto record = MakeRecord(50);

    auto slot = page.InsertRecord(record.data(), record.size());
    ASSERT_TRUE(slot.has_value());
    EXPECT_EQ(page.GetNumRecords(), 1);

    bool deleted = page.DeleteRecord(slot.value());
    EXPECT_TRUE(deleted);
    EXPECT_EQ(page.GetNumRecords(), 0);

    // Slot should now be empty
    auto result = page.GetRecord(slot.value());
    EXPECT_FALSE(result.has_value());
}

TEST(PageTest, DeleteNonExistentSlot) {
    Page page(1);
    EXPECT_FALSE(page.DeleteRecord(0));   // No slots exist
    EXPECT_FALSE(page.DeleteRecord(99));  // Way out of range
}

TEST(PageTest, DoubleDeleteFails) {
    Page page(1);
    auto record = MakeRecord(30);
    auto slot = page.InsertRecord(record.data(), record.size());
    ASSERT_TRUE(slot.has_value());

    EXPECT_TRUE(page.DeleteRecord(slot.value()));
    EXPECT_FALSE(page.DeleteRecord(slot.value()));  // Already deleted
}

TEST(PageTest, SlotReuseAfterDeletion) {
    Page page(1);

    auto r1 = MakeStringRecord("First");
    auto r2 = MakeStringRecord("Second");
    auto r3 = MakeStringRecord("Third");

    auto s1 = page.InsertRecord(r1.data(), r1.size());
    auto s2 = page.InsertRecord(r2.data(), r2.size());

    ASSERT_TRUE(s1.has_value());
    ASSERT_TRUE(s2.has_value());

    // Delete first record
    page.DeleteRecord(s1.value());

    // Insert third record — should reuse slot 0
    auto s3 = page.InsertRecord(r3.data(), r3.size());
    ASSERT_TRUE(s3.has_value());
    EXPECT_EQ(s3.value(), 0);  // Reused slot 0

    // Verify the new record at slot 0
    auto result = page.GetRecord(0);
    ASSERT_TRUE(result.has_value());
    auto [data, length] = result.value();
    std::string retrieved(reinterpret_cast<const char*>(data), length);
    EXPECT_EQ(retrieved, "Third");
}

// =============================================================================
// Page Full Behavior
// =============================================================================

TEST(PageTest, PageFull) {
    Page page(1);

    // Fill the page with records until it's full
    auto record = MakeRecord(200);
    int count = 0;
    while (true) {
        auto slot = page.InsertRecord(record.data(), record.size());
        if (!slot.has_value()) break;
        ++count;
    }

    EXPECT_GT(count, 0);
    EXPECT_EQ(page.GetNumRecords(), count);

    // One more insert should fail
    auto result = page.InsertRecord(record.data(), record.size());
    EXPECT_FALSE(result.has_value());
}

TEST(PageTest, HasSpaceForAccuracy) {
    Page page(1);

    // Fresh page should have space for a reasonable record
    EXPECT_TRUE(page.HasSpaceFor(100));
    EXPECT_TRUE(page.HasSpaceFor(1000));
    EXPECT_TRUE(page.HasSpaceFor(3000));

    // Can't fit a record larger than the usable page space
    EXPECT_FALSE(page.HasSpaceFor(PAGE_SIZE));
}

// =============================================================================
// Update
// =============================================================================

TEST(PageTest, UpdateSmallerRecord) {
    Page page(1);
    auto r1 = MakeStringRecord("Original long record data");
    auto slot = page.InsertRecord(r1.data(), r1.size());
    ASSERT_TRUE(slot.has_value());

    auto r2 = MakeStringRecord("Short");
    bool updated = page.UpdateRecord(slot.value(), r2.data(), r2.size());
    EXPECT_TRUE(updated);

    auto result = page.GetRecord(slot.value());
    ASSERT_TRUE(result.has_value());
    auto [data, length] = result.value();
    std::string retrieved(reinterpret_cast<const char*>(data), length);
    EXPECT_EQ(retrieved, "Short");
}

TEST(PageTest, UpdateLargerRecordWithSpace) {
    Page page(1);
    auto r1 = MakeStringRecord("Hi");
    auto slot = page.InsertRecord(r1.data(), r1.size());
    ASSERT_TRUE(slot.has_value());

    auto r2 = MakeStringRecord("This is a much longer replacement record");
    bool updated = page.UpdateRecord(slot.value(), r2.data(), r2.size());
    EXPECT_TRUE(updated);

    auto result = page.GetRecord(slot.value());
    ASSERT_TRUE(result.has_value());
    auto [data, length] = result.value();
    std::string retrieved(reinterpret_cast<const char*>(data), length);
    EXPECT_EQ(retrieved, "This is a much longer replacement record");
}

// =============================================================================
// Compaction
// =============================================================================

TEST(PageTest, CompactionRecoversFreeSpace) {
    Page page(1);

    // Insert several records
    std::vector<uint16_t> slots;
    for (int i = 0; i < 10; ++i) {
        auto record = MakeRecord(100, static_cast<uint8_t>(i));
        auto slot = page.InsertRecord(record.data(), record.size());
        ASSERT_TRUE(slot.has_value());
        slots.push_back(slot.value());
    }

    uint16_t free_before = page.GetFreeSpace();

    // Delete every other record (creates fragmentation)
    for (int i = 0; i < 10; i += 2) {
        page.DeleteRecord(slots[i]);
    }

    // Compact should recover space from deleted records
    page.Compact();

    uint16_t free_after = page.GetFreeSpace();
    EXPECT_GT(free_after, free_before);

    // Remaining records should still be readable
    for (int i = 1; i < 10; i += 2) {
        auto result = page.GetRecord(slots[i]);
        ASSERT_TRUE(result.has_value());
        auto [data, length] = result.value();
        EXPECT_EQ(length, 100);
        EXPECT_EQ(data[0], static_cast<uint8_t>(i));
    }
}

// =============================================================================
// Serialization Round-Trip
// =============================================================================

TEST(PageTest, SerializeDeserializeRoundTrip) {
    Page page(7);
    page.SetNextPageId(42);
    page.SetFlags(static_cast<uint16_t>(PageType::kData));

    auto r1 = MakeStringRecord("Game: LAL vs GSW");
    auto r2 = MakeStringRecord("Player: LeBron James");
    auto r3 = MakeStringRecord("Event: 3PT FG Made");

    (void)page.InsertRecord(r1.data(), r1.size());
    (void)page.InsertRecord(r2.data(), r2.size());
    (void)page.InsertRecord(r3.data(), r3.size());

    // Serialize to buffer
    page.SerializeToBuffer();

    // Create a new page and load from the same buffer
    Page page2;
    std::memcpy(page2.GetMutableData(), page.GetData(), PAGE_SIZE);
    page2.DeserializeFromBuffer();

    // Verify metadata
    EXPECT_EQ(page2.GetPageId(), 7u);
    EXPECT_EQ(page2.GetNextPageId(), 42u);
    EXPECT_EQ(page2.GetFlags(), static_cast<uint16_t>(PageType::kData));
    EXPECT_EQ(page2.GetNumRecords(), 3);
    EXPECT_EQ(page2.GetNumSlots(), 3);

    // Verify records
    auto result = page2.GetRecord(2);
    ASSERT_TRUE(result.has_value());
    auto [data, length] = result.value();
    std::string retrieved(reinterpret_cast<const char*>(data), length);
    EXPECT_EQ(retrieved, "Event: 3PT FG Made");
}

// =============================================================================
// Edge Cases
// =============================================================================

TEST(PageTest, NullDataInsertFails) {
    Page page(1);
    auto slot = page.InsertRecord(nullptr, 10);
    EXPECT_FALSE(slot.has_value());
}

TEST(PageTest, ZeroLengthInsertFails) {
    Page page(1);
    uint8_t dummy = 0;
    auto slot = page.InsertRecord(&dummy, 0);
    EXPECT_FALSE(slot.has_value());
}

TEST(PageTest, SingleByteRecord) {
    Page page(1);
    uint8_t byte = 0x42;
    auto slot = page.InsertRecord(&byte, 1);
    ASSERT_TRUE(slot.has_value());

    auto result = page.GetRecord(slot.value());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().second, 1);
    EXPECT_EQ(result.value().first[0], 0x42);
}

TEST(PageTest, MaximumSizeRecord) {
    Page page(1);
    // Maximum record: PAGE_SIZE - header - one slot entry
    // Header=20, slot=4, so max data = 4096-20-4 = 4072 bytes
    uint16_t max_record_size = PAGE_SIZE - 20 - 4;
    auto record = MakeRecord(max_record_size);

    auto slot = page.InsertRecord(record.data(), record.size());
    ASSERT_TRUE(slot.has_value());

    auto result = page.GetRecord(slot.value());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().second, max_record_size);
}

// =============================================================================
// Stress Test
// =============================================================================

TEST(PageTest, StressManySmallRecords) {
    Page page(1);
    std::vector<std::pair<uint16_t, std::vector<uint8_t>>> inserted;

    // Insert as many 10-byte records as possible
    int count = 0;
    while (true) {
        auto record = MakeRecord(10, static_cast<uint8_t>(count & 0xFF));
        auto slot = page.InsertRecord(record.data(), record.size());
        if (!slot.has_value()) break;
        inserted.emplace_back(slot.value(), record);
        ++count;
    }

    // Should fit many small records
    EXPECT_GT(count, 200);

    // Verify all records
    for (const auto& [slot_id, expected] : inserted) {
        auto result = page.GetRecord(slot_id);
        ASSERT_TRUE(result.has_value());
        auto [data, length] = result.value();
        EXPECT_EQ(length, 10);
        EXPECT_EQ(std::memcmp(data, expected.data(), length), 0);
    }
}

TEST(PageTest, RandomizedInsertDeleteStress) {
    Page page(1);
    std::mt19937 rng(12345);  // Deterministic seed for reproducibility
    std::uniform_int_distribution<uint16_t> size_dist(5, 200);

    std::vector<uint16_t> live_slots;

    for (int iteration = 0; iteration < 1000; ++iteration) {
        if (live_slots.empty() || (rng() % 3 != 0 && page.HasSpaceFor(200))) {
            // Insert
            uint16_t size = size_dist(rng);
            auto record = MakeRecord(size, static_cast<uint8_t>(iteration & 0xFF));
            auto slot = page.InsertRecord(record.data(), record.size());
            if (slot.has_value()) {
                live_slots.push_back(slot.value());
            }
        } else {
            // Delete random slot
            std::uniform_int_distribution<size_t> idx_dist(0, live_slots.size() - 1);
            size_t idx = idx_dist(rng);
            page.DeleteRecord(live_slots[idx]);
            live_slots.erase(live_slots.begin() + idx);
        }
    }

    // After all operations, verify num_records matches live slots
    EXPECT_EQ(page.GetNumRecords(), live_slots.size());
}

TEST(PageTest, MoveSemantics) {
    Page page(99);
    auto record = MakeStringRecord("Movable data");
    (void)page.InsertRecord(record.data(), record.size());

    // Move construct
    Page page2(std::move(page));
    EXPECT_EQ(page2.GetPageId(), 99u);
    EXPECT_EQ(page2.GetNumRecords(), 1);

    auto result = page2.GetRecord(0);
    ASSERT_TRUE(result.has_value());
    auto [data, length] = result.value();
    std::string retrieved(reinterpret_cast<const char*>(data), length);
    EXPECT_EQ(retrieved, "Movable data");
}

}  // namespace
}  // namespace courtdb
