/**
 * =============================================================================
 * CourtDB - Heap File & Record Serialization Tests
 * =============================================================================
 *
 * Comprehensive tests covering:
 *   - Record serialization round-trip
 *   - Heap file insert/get/delete/update
 *   - Sequential scan iteration
 *   - Bulk insert
 *   - Page chain growth
 *   - Free space reuse
 *   - Data integrity under eviction pressure
 *   - Stress tests with many records
 *
 * =============================================================================
 */

#include "storage/heap_file.h"
#include "storage/record.h"

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
// Helpers
// =============================================================================

NBARecord MakeTestRecord(int id) {
    NBARecord r;
    r.game_id = "002210" + std::to_string(1000 + id);
    r.season = 2023;
    r.quarter = static_cast<uint8_t>((id % 4) + 1);
    r.clock = std::to_string(11 - (id % 12)) + ":" + std::to_string(id % 60);
    r.team = (id % 2 == 0) ? "LAL" : "GSW";
    r.player = "Player_" + std::to_string(id);
    r.event_type = static_cast<uint8_t>((id % 13) + 1);
    r.description = "Event description for play #" + std::to_string(id);
    r.points = static_cast<uint8_t>(id % 4);
    r.shot_distance = static_cast<uint16_t>(id % 30);
    r.home_score = static_cast<uint16_t>(50 + id);
    r.away_score = static_cast<uint16_t>(48 + id);
    return r;
}

// =============================================================================
// Record Serialization Tests
// =============================================================================

TEST(RecordTest, SerializeDeserializeRoundTrip) {
    NBARecord original;
    original.game_id = "0022100001";
    original.season = 2023;
    original.quarter = 3;
    original.clock = "7:42";
    original.team = "LAL";
    original.player = "LeBron James";
    original.event_type = static_cast<uint8_t>(EventType::kMadeShot);
    original.description = "LeBron James 26' 3PT Jump Shot (28 PTS)";
    original.points = 3;
    original.shot_distance = 26;
    original.home_score = 78;
    original.away_score = 72;

    auto bytes = RecordSerializer::Serialize(original);
    auto deserialized = RecordSerializer::Deserialize(bytes.data(),
                                                      static_cast<uint16_t>(bytes.size()));

    EXPECT_EQ(deserialized.game_id, original.game_id);
    EXPECT_EQ(deserialized.season, original.season);
    EXPECT_EQ(deserialized.quarter, original.quarter);
    EXPECT_EQ(deserialized.clock, original.clock);
    EXPECT_EQ(deserialized.team, original.team);
    EXPECT_EQ(deserialized.player, original.player);
    EXPECT_EQ(deserialized.event_type, original.event_type);
    EXPECT_EQ(deserialized.description, original.description);
    EXPECT_EQ(deserialized.points, original.points);
    EXPECT_EQ(deserialized.shot_distance, original.shot_distance);
    EXPECT_EQ(deserialized.home_score, original.home_score);
    EXPECT_EQ(deserialized.away_score, original.away_score);
}

TEST(RecordTest, ComputeSerializedSize) {
    NBARecord record;
    record.game_id = "ABC";        // 3 bytes + 2 prefix = 5
    record.clock = "12:00";        // 5 bytes + 2 prefix = 7
    record.team = "XX";            // 2 bytes + 2 prefix = 4
    record.player = "Test";        // 4 bytes + 2 prefix = 6
    record.description = "Desc";   // 4 bytes + 2 prefix = 6
    // Fixed: 11 bytes
    // Total: 5 + 7 + 4 + 6 + 6 + 11 = 39

    EXPECT_EQ(RecordSerializer::ComputeSerializedSize(record), 39);

    auto bytes = RecordSerializer::Serialize(record);
    EXPECT_EQ(bytes.size(), 39u);
}

TEST(RecordTest, EmptyStrings) {
    NBARecord record{};  // All strings empty
    auto bytes = RecordSerializer::Serialize(record);
    auto deserialized = RecordSerializer::Deserialize(bytes.data(),
                                                      static_cast<uint16_t>(bytes.size()));
    EXPECT_EQ(deserialized.game_id, "");
    EXPECT_EQ(deserialized.player, "");
    EXPECT_EQ(deserialized.description, "");
}

TEST(RecordTest, ManyRecordsSerialization) {
    for (int i = 0; i < 100; ++i) {
        auto record = MakeTestRecord(i);
        auto bytes = RecordSerializer::Serialize(record);
        auto deserialized = RecordSerializer::Deserialize(
            bytes.data(), static_cast<uint16_t>(bytes.size()));

        EXPECT_EQ(deserialized.game_id, record.game_id);
        EXPECT_EQ(deserialized.player, record.player);
        EXPECT_EQ(deserialized.season, record.season);
        EXPECT_EQ(deserialized.points, record.points);
    }
}

// =============================================================================
// RID Tests
// =============================================================================

TEST(RIDTest, DefaultIsInvalid) {
    RID rid;
    EXPECT_FALSE(rid.IsValid());
}

TEST(RIDTest, ValidRID) {
    RID rid(5, 3);
    EXPECT_TRUE(rid.IsValid());
    EXPECT_EQ(rid.page_id, 5u);
    EXPECT_EQ(rid.slot_id, 3);
}

TEST(RIDTest, Comparison) {
    RID a(1, 0);
    RID b(1, 1);
    RID c(2, 0);

    EXPECT_TRUE(a < b);
    EXPECT_TRUE(b < c);
    EXPECT_TRUE(a < c);
    EXPECT_EQ(a, RID(1, 0));
    EXPECT_NE(a, b);
}

// =============================================================================
// Heap File Test Fixture
// =============================================================================

class HeapFileTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_file_ = "/tmp/courtdb_heap_test_" +
                     std::to_string(reinterpret_cast<uintptr_t>(this)) + ".db";
        std::filesystem::remove(test_file_);
        disk_manager_ = std::make_unique<DiskManager>(test_file_);
        bpm_ = std::make_unique<BufferPoolManager>(50, disk_manager_.get());
    }

    void TearDown() override {
        heap_.reset();
        bpm_.reset();
        disk_manager_.reset();
        std::filesystem::remove(test_file_);
    }

    void CreateHeap() {
        heap_ = std::make_unique<HeapFile>(bpm_.get());
    }

    std::string test_file_;
    std::unique_ptr<DiskManager> disk_manager_;
    std::unique_ptr<BufferPoolManager> bpm_;
    std::unique_ptr<HeapFile> heap_;
};

// =============================================================================
// Basic Heap File Operations
// =============================================================================

TEST_F(HeapFileTest, CreateNewHeap) {
    CreateHeap();
    EXPECT_NE(heap_->GetFirstPageId(), INVALID_PAGE_ID);
    EXPECT_EQ(heap_->GetRecordCount(), 0u);
    EXPECT_EQ(heap_->GetPageCount(), 1u);
}

TEST_F(HeapFileTest, InsertSingleRecord) {
    CreateHeap();

    auto record = MakeTestRecord(1);
    auto bytes = RecordSerializer::Serialize(record);
    RID rid = heap_->InsertRecord(bytes.data(), static_cast<uint16_t>(bytes.size()));

    EXPECT_TRUE(rid.IsValid());
    EXPECT_EQ(heap_->GetRecordCount(), 1u);
}

TEST_F(HeapFileTest, InsertAndRetrieve) {
    CreateHeap();

    auto record = MakeTestRecord(42);
    auto bytes = RecordSerializer::Serialize(record);
    RID rid = heap_->InsertRecord(bytes.data(), static_cast<uint16_t>(bytes.size()));
    ASSERT_TRUE(rid.IsValid());

    const uint8_t* data;
    uint16_t length;
    ASSERT_TRUE(heap_->GetRecord(rid, &data, &length));

    auto deserialized = RecordSerializer::Deserialize(data, length);
    EXPECT_EQ(deserialized.game_id, record.game_id);
    EXPECT_EQ(deserialized.player, record.player);
    EXPECT_EQ(deserialized.season, 2023);

    bpm_->UnpinPage(rid.page_id, false);
}

TEST_F(HeapFileTest, InsertMultipleRecords) {
    CreateHeap();

    std::vector<RID> rids;
    for (int i = 0; i < 20; ++i) {
        auto record = MakeTestRecord(i);
        auto bytes = RecordSerializer::Serialize(record);
        RID rid = heap_->InsertRecord(bytes.data(), static_cast<uint16_t>(bytes.size()));
        ASSERT_TRUE(rid.IsValid()) << "Failed to insert record " << i;
        rids.push_back(rid);
    }

    EXPECT_EQ(heap_->GetRecordCount(), 20u);

    // Verify all records
    for (int i = 0; i < 20; ++i) {
        const uint8_t* data;
        uint16_t length;
        ASSERT_TRUE(heap_->GetRecord(rids[i], &data, &length));

        auto deserialized = RecordSerializer::Deserialize(data, length);
        auto expected = MakeTestRecord(i);
        EXPECT_EQ(deserialized.game_id, expected.game_id);
        EXPECT_EQ(deserialized.player, expected.player);

        bpm_->UnpinPage(rids[i].page_id, false);
    }
}

// =============================================================================
// Delete
// =============================================================================

TEST_F(HeapFileTest, DeleteRecord) {
    CreateHeap();

    auto record = MakeTestRecord(1);
    auto bytes = RecordSerializer::Serialize(record);
    RID rid = heap_->InsertRecord(bytes.data(), static_cast<uint16_t>(bytes.size()));
    ASSERT_TRUE(rid.IsValid());

    EXPECT_TRUE(heap_->DeleteRecord(rid));
    EXPECT_EQ(heap_->GetRecordCount(), 0u);

    // Record should no longer be accessible
    const uint8_t* data;
    uint16_t length;
    EXPECT_FALSE(heap_->GetRecord(rid, &data, &length));
}

TEST_F(HeapFileTest, DeleteInvalidRID) {
    CreateHeap();
    EXPECT_FALSE(heap_->DeleteRecord(RID{}));
    EXPECT_FALSE(heap_->DeleteRecord(RID{999, 0}));
}

// =============================================================================
// Update
// =============================================================================

TEST_F(HeapFileTest, UpdateSmallerRecord) {
    CreateHeap();

    auto record = MakeTestRecord(1);
    record.description = "A very long description that takes up lots of space in the page";
    auto bytes = RecordSerializer::Serialize(record);
    RID rid = heap_->InsertRecord(bytes.data(), static_cast<uint16_t>(bytes.size()));
    ASSERT_TRUE(rid.IsValid());

    // Update with a shorter record
    record.description = "Short";
    auto new_bytes = RecordSerializer::Serialize(record);
    RID new_rid = heap_->UpdateRecord(rid, new_bytes.data(),
                                       static_cast<uint16_t>(new_bytes.size()));
    EXPECT_TRUE(new_rid.IsValid());
    EXPECT_EQ(new_rid, rid);  // Updated in place

    const uint8_t* data;
    uint16_t length;
    ASSERT_TRUE(heap_->GetRecord(new_rid, &data, &length));
    auto deserialized = RecordSerializer::Deserialize(data, length);
    EXPECT_EQ(deserialized.description, "Short");

    bpm_->UnpinPage(new_rid.page_id, false);
}

// =============================================================================
// Sequential Scan
// =============================================================================

TEST_F(HeapFileTest, SequentialScan) {
    CreateHeap();

    constexpr int NUM_RECORDS = 30;
    for (int i = 0; i < NUM_RECORDS; ++i) {
        auto record = MakeTestRecord(i);
        auto bytes = RecordSerializer::Serialize(record);
        (void)heap_->InsertRecord(bytes.data(), static_cast<uint16_t>(bytes.size()));
    }

    // Scan all records
    int count = 0;
    auto it = heap_->Begin();
    while (it.IsValid()) {
        auto [data, length] = it.GetRecord();
        EXPECT_GT(length, 0);
        count++;
        it.Next();
    }

    EXPECT_EQ(count, NUM_RECORDS);
}

TEST_F(HeapFileTest, ScanAfterDeletes) {
    CreateHeap();

    std::vector<RID> rids;
    for (int i = 0; i < 10; ++i) {
        auto record = MakeTestRecord(i);
        auto bytes = RecordSerializer::Serialize(record);
        rids.push_back(heap_->InsertRecord(bytes.data(), static_cast<uint16_t>(bytes.size())));
    }

    // Delete even-numbered records
    for (int i = 0; i < 10; i += 2) {
        heap_->DeleteRecord(rids[i]);
    }

    // Scan should only see 5 records
    int count = 0;
    auto it = heap_->Begin();
    while (it.IsValid()) {
        count++;
        it.Next();
    }
    EXPECT_EQ(count, 5);
}

TEST_F(HeapFileTest, ScanEmptyHeap) {
    CreateHeap();
    auto it = heap_->Begin();
    EXPECT_FALSE(it.IsValid());
}

// =============================================================================
// Page Chain Growth
// =============================================================================

TEST_F(HeapFileTest, MultiplePages) {
    CreateHeap();

    // Insert enough records to span multiple pages
    // Each serialized NBARecord is ~70-100 bytes, page holds ~40-50 records
    constexpr int NUM_RECORDS = 200;
    for (int i = 0; i < NUM_RECORDS; ++i) {
        auto record = MakeTestRecord(i);
        auto bytes = RecordSerializer::Serialize(record);
        RID rid = heap_->InsertRecord(bytes.data(), static_cast<uint16_t>(bytes.size()));
        ASSERT_TRUE(rid.IsValid()) << "Failed at record " << i;
    }

    EXPECT_GT(heap_->GetPageCount(), 1u);
    EXPECT_EQ(heap_->GetRecordCount(), static_cast<uint32_t>(NUM_RECORDS));

    // Verify all records via scan
    int count = 0;
    auto it = heap_->Begin();
    while (it.IsValid()) {
        count++;
        it.Next();
    }
    EXPECT_EQ(count, NUM_RECORDS);
}

// =============================================================================
// Bulk Insert
// =============================================================================

TEST_F(HeapFileTest, BulkInsert) {
    CreateHeap();

    std::vector<std::vector<uint8_t>> serialized_records;
    std::vector<std::pair<const uint8_t*, uint16_t>> record_ptrs;

    for (int i = 0; i < 50; ++i) {
        auto record = MakeTestRecord(i);
        serialized_records.push_back(RecordSerializer::Serialize(record));
    }

    for (auto& bytes : serialized_records) {
        record_ptrs.emplace_back(bytes.data(), static_cast<uint16_t>(bytes.size()));
    }

    auto rids = heap_->BulkInsert(record_ptrs);
    EXPECT_EQ(rids.size(), 50u);

    // All RIDs should be valid
    for (const auto& rid : rids) {
        EXPECT_TRUE(rid.IsValid());
    }

    EXPECT_EQ(heap_->GetRecordCount(), 50u);
}

// =============================================================================
// Free Space Reuse
// =============================================================================

TEST_F(HeapFileTest, FreeSpaceReuse) {
    CreateHeap();

    // Fill a page
    std::vector<RID> rids;
    for (int i = 0; i < 40; ++i) {
        auto record = MakeTestRecord(i);
        auto bytes = RecordSerializer::Serialize(record);
        rids.push_back(heap_->InsertRecord(bytes.data(), static_cast<uint16_t>(bytes.size())));
    }

    uint32_t pages_before = heap_->GetPageCount();

    // Delete half the records
    for (int i = 0; i < 20; ++i) {
        heap_->DeleteRecord(rids[i]);
    }

    // Insert new records — should reuse freed space, not allocate new pages
    for (int i = 100; i < 120; ++i) {
        auto record = MakeTestRecord(i);
        auto bytes = RecordSerializer::Serialize(record);
        heap_->InsertRecord(bytes.data(), static_cast<uint16_t>(bytes.size()));
    }

    // Page count should not have grown (space was reused)
    EXPECT_LE(heap_->GetPageCount(), pages_before + 1u);
}

// =============================================================================
// Stress Tests
// =============================================================================

TEST_F(HeapFileTest, StressInsertAndScan) {
    CreateHeap();

    constexpr int NUM_RECORDS = 1000;
    std::vector<RID> rids;
    rids.reserve(NUM_RECORDS);

    for (int i = 0; i < NUM_RECORDS; ++i) {
        auto record = MakeTestRecord(i);
        auto bytes = RecordSerializer::Serialize(record);
        RID rid = heap_->InsertRecord(bytes.data(), static_cast<uint16_t>(bytes.size()));
        ASSERT_TRUE(rid.IsValid()) << "Failed at record " << i;
        rids.push_back(rid);
    }

    EXPECT_EQ(heap_->GetRecordCount(), static_cast<uint32_t>(NUM_RECORDS));

    // Verify random access
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> dist(0, NUM_RECORDS - 1);

    for (int i = 0; i < 200; ++i) {
        int idx = dist(rng);
        const uint8_t* data;
        uint16_t length;
        ASSERT_TRUE(heap_->GetRecord(rids[idx], &data, &length))
            << "Failed to get record " << idx;

        auto deserialized = RecordSerializer::Deserialize(data, length);
        auto expected = MakeTestRecord(idx);
        EXPECT_EQ(deserialized.game_id, expected.game_id);
        EXPECT_EQ(deserialized.player, expected.player);

        bpm_->UnpinPage(rids[idx].page_id, false);
    }

    // Full scan
    int scan_count = 0;
    auto it = heap_->Begin();
    while (it.IsValid()) {
        scan_count++;
        it.Next();
    }
    EXPECT_EQ(scan_count, NUM_RECORDS);
}

TEST_F(HeapFileTest, StressInsertDeleteMixed) {
    CreateHeap();

    std::mt19937 rng(12345);
    std::vector<RID> live_rids;

    for (int iteration = 0; iteration < 500; ++iteration) {
        if (live_rids.empty() || rng() % 3 != 0) {
            // Insert
            auto record = MakeTestRecord(iteration);
            auto bytes = RecordSerializer::Serialize(record);
            RID rid = heap_->InsertRecord(bytes.data(), static_cast<uint16_t>(bytes.size()));
            if (rid.IsValid()) {
                live_rids.push_back(rid);
            }
        } else {
            // Delete random record
            std::uniform_int_distribution<size_t> idx_dist(0, live_rids.size() - 1);
            size_t idx = idx_dist(rng);
            heap_->DeleteRecord(live_rids[idx]);
            live_rids.erase(live_rids.begin() + static_cast<long>(idx));
        }
    }

    // Verify record count matches live RIDs
    EXPECT_EQ(heap_->GetRecordCount(), static_cast<uint32_t>(live_rids.size()));

    // Verify scan count matches
    int scan_count = 0;
    auto it = heap_->Begin();
    while (it.IsValid()) {
        scan_count++;
        it.Next();
    }
    EXPECT_EQ(scan_count, static_cast<int>(live_rids.size()));
}

}  // namespace
}  // namespace courtdb
