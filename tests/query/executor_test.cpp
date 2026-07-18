/**
 * =============================================================================
 * CourtDB - Query Executor Unit Tests
 * =============================================================================
 *
 * Tests covering:
 *   - Table scan (full scan, empty table)
 *   - Index scan (point lookup, range scan)
 *   - Filter (various predicates)
 *   - Projection (field selection)
 *   - Sort (ORDER BY ascending/descending)
 *   - Limit (LIMIT N)
 *   - Aggregation (GROUP BY with COUNT, SUM, AVG)
 *   - TopN (ORDER BY + LIMIT optimization)
 *   - Composed operator trees (multi-operator pipelines)
 *   - NBA-specific analytical queries
 *
 * =============================================================================
 */

#include "query/executor.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

namespace courtdb {
namespace {

// =============================================================================
// Helpers
// =============================================================================

NBARecord MakeEvent(int id, const std::string& team, const std::string& player,
                    uint8_t event_type, uint8_t points, uint16_t season = 2023) {
    NBARecord r;
    r.game_id = "002210" + std::to_string(1000 + id);
    r.season = season;
    r.quarter = static_cast<uint8_t>((id % 4) + 1);
    r.clock = std::to_string(11 - (id % 12)) + ":00";
    r.team = team;
    r.player = player;
    r.event_type = event_type;
    r.description = player + " " + (points > 0 ? "scores" : "event");
    r.points = points;
    r.shot_distance = static_cast<uint16_t>(id % 30);
    r.home_score = static_cast<uint16_t>(50 + id);
    r.away_score = static_cast<uint16_t>(48 + id);
    return r;
}

// =============================================================================
// Test Fixture
// =============================================================================

class ExecutorTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_file_ = "/tmp/courtdb_exec_test_" +
                     std::to_string(reinterpret_cast<uintptr_t>(this)) + ".db";
        std::filesystem::remove(test_file_);
        disk_manager_ = std::make_unique<DiskManager>(test_file_);
        bpm_ = std::make_unique<BufferPoolManager>(100, disk_manager_.get());
        heap_ = std::make_unique<HeapFile>(bpm_.get());
    }

    void TearDown() override {
        index_.reset();
        heap_.reset();
        bpm_.reset();
        disk_manager_.reset();
        std::filesystem::remove(test_file_);
    }

    /// Load N test records into the heap
    void LoadTestData(int n) {
        std::vector<std::string> teams = {"LAL", "GSW", "BOS", "MIA", "DEN"};
        std::vector<std::string> players = {
            "LeBron James", "Stephen Curry", "Jayson Tatum",
            "Jimmy Butler", "Nikola Jokic"
        };

        for (int i = 0; i < n; ++i) {
            auto record = MakeEvent(
                i,
                teams[i % teams.size()],
                players[i % players.size()],
                static_cast<uint8_t>((i % 3) + 1),  // MadeShot, MissedShot, FreeThrow
                static_cast<uint8_t>(i % 4)          // 0, 1, 2, 3 points
            );
            auto bytes = RecordSerializer::Serialize(record);
            heap_->InsertRecord(bytes.data(), static_cast<uint16_t>(bytes.size()));
        }
    }

    /// Create and populate a B+ tree index on the `season` field
    void BuildSeasonIndex() {
        index_ = std::make_unique<BPlusTree>(bpm_.get());

        auto it = heap_->Begin();
        while (it.IsValid()) {
            auto [data, length] = it.GetRecord();
            auto record = RecordSerializer::Deserialize(data, length);
            RID rid = it.GetRID();
            index_->Insert(record.season, rid);
            it.Next();
        }
    }

    std::string test_file_;
    std::unique_ptr<DiskManager> disk_manager_;
    std::unique_ptr<BufferPoolManager> bpm_;
    std::unique_ptr<HeapFile> heap_;
    std::unique_ptr<BPlusTree> index_;
};

// =============================================================================
// Table Scan
// =============================================================================

TEST_F(ExecutorTest, TableScanEmpty) {
    TableScanExecutor scan(heap_.get(), bpm_.get());
    scan.Init();

    Tuple tuple;
    EXPECT_FALSE(scan.Next(&tuple));
}

TEST_F(ExecutorTest, TableScanAll) {
    LoadTestData(50);

    TableScanExecutor scan(heap_.get(), bpm_.get());
    scan.Init();

    int count = 0;
    Tuple tuple;
    while (scan.Next(&tuple)) {
        count++;
        EXPECT_EQ(tuple.season, 2023);
    }
    EXPECT_EQ(count, 50);
}

TEST_F(ExecutorTest, TableScanVerifyData) {
    LoadTestData(5);

    TableScanExecutor scan(heap_.get(), bpm_.get());
    scan.Init();

    Tuple tuple;
    ASSERT_TRUE(scan.Next(&tuple));
    EXPECT_EQ(tuple.team, "LAL");
    EXPECT_EQ(tuple.player, "LeBron James");
}

// =============================================================================
// Index Scan
// =============================================================================

TEST_F(ExecutorTest, IndexScanPointLookup) {
    // Load records with different seasons
    for (int i = 0; i < 20; ++i) {
        auto record = MakeEvent(i, "LAL", "LeBron", 1, 2,
                                static_cast<uint16_t>(2020 + (i % 4)));
        auto bytes = RecordSerializer::Serialize(record);
        heap_->InsertRecord(bytes.data(), static_cast<uint16_t>(bytes.size()));
    }

    BuildSeasonIndex();

    // Look up season 2022
    IndexScanExecutor scan(index_.get(), heap_.get(), bpm_.get(),
                           static_cast<KeyType>(2022));
    scan.Init();

    int count = 0;
    Tuple tuple;
    while (scan.Next(&tuple)) {
        EXPECT_EQ(tuple.season, 2022);
        count++;
    }
    EXPECT_EQ(count, 5);  // 20 records / 4 seasons = 5 per season
}

TEST_F(ExecutorTest, IndexScanRange) {
    for (int i = 0; i < 30; ++i) {
        auto record = MakeEvent(i, "GSW", "Curry", 1, 3,
                                static_cast<uint16_t>(2018 + i % 6));
        auto bytes = RecordSerializer::Serialize(record);
        heap_->InsertRecord(bytes.data(), static_cast<uint16_t>(bytes.size()));
    }

    BuildSeasonIndex();

    // Range scan: seasons 2020-2022
    IndexScanExecutor scan(index_.get(), heap_.get(), bpm_.get(),
                           static_cast<KeyType>(2020), static_cast<KeyType>(2022));
    scan.Init();

    int count = 0;
    Tuple tuple;
    while (scan.Next(&tuple)) {
        EXPECT_GE(tuple.season, 2020);
        EXPECT_LE(tuple.season, 2022);
        count++;
    }
    EXPECT_EQ(count, 15);  // 30 records / 6 seasons * 3 seasons = 15
}

// =============================================================================
// Filter
// =============================================================================

TEST_F(ExecutorTest, FilterByTeam) {
    LoadTestData(50);

    auto scan = std::make_shared<TableScanExecutor>(heap_.get(), bpm_.get());
    auto filter = std::make_shared<FilterExecutor>(scan,
        [](const Tuple& t) { return t.team == "LAL"; });

    filter->Init();

    int count = 0;
    Tuple tuple;
    while (filter->Next(&tuple)) {
        EXPECT_EQ(tuple.team, "LAL");
        count++;
    }
    EXPECT_EQ(count, 10);  // 50 records / 5 teams = 10
}

TEST_F(ExecutorTest, FilterByPoints) {
    LoadTestData(100);

    auto scan = std::make_shared<TableScanExecutor>(heap_.get(), bpm_.get());
    auto filter = std::make_shared<FilterExecutor>(scan,
        [](const Tuple& t) { return t.points == 3; });

    filter->Init();

    int count = 0;
    Tuple tuple;
    while (filter->Next(&tuple)) {
        EXPECT_EQ(tuple.points, 3);
        count++;
    }
    EXPECT_EQ(count, 25);  // 100 records / 4 point values = 25
}

TEST_F(ExecutorTest, FilterNoMatches) {
    LoadTestData(20);

    auto scan = std::make_shared<TableScanExecutor>(heap_.get(), bpm_.get());
    auto filter = std::make_shared<FilterExecutor>(scan,
        [](const Tuple& t) { return t.team == "NYK"; });  // Not in test data

    filter->Init();

    Tuple tuple;
    EXPECT_FALSE(filter->Next(&tuple));
}

TEST_F(ExecutorTest, FilterCompound) {
    LoadTestData(100);

    auto scan = std::make_shared<TableScanExecutor>(heap_.get(), bpm_.get());
    // Team == "GSW" AND points >= 2
    auto filter = std::make_shared<FilterExecutor>(scan,
        [](const Tuple& t) { return t.team == "GSW" && t.points >= 2; });

    filter->Init();

    int count = 0;
    Tuple tuple;
    while (filter->Next(&tuple)) {
        EXPECT_EQ(tuple.team, "GSW");
        EXPECT_GE(tuple.points, 2);
        count++;
    }
    EXPECT_GT(count, 0);
}

// =============================================================================
// Projection
// =============================================================================

TEST_F(ExecutorTest, ProjectionSelectFields) {
    LoadTestData(10);

    auto scan = std::make_shared<TableScanExecutor>(heap_.get(), bpm_.get());
    // Project: keep only team, player, points (clear other fields)
    auto proj = std::make_shared<ProjectionExecutor>(scan,
        [](const Tuple& t) -> Tuple {
            Tuple result;
            result.team = t.team;
            result.player = t.player;
            result.points = t.points;
            return result;
        });

    proj->Init();

    Tuple tuple;
    ASSERT_TRUE(proj->Next(&tuple));
    EXPECT_EQ(tuple.team, "LAL");
    EXPECT_EQ(tuple.player, "LeBron James");
    // Projected-out fields should be empty/zero
    EXPECT_EQ(tuple.game_id, "");
    EXPECT_EQ(tuple.season, 0);
}

// =============================================================================
// Sort (ORDER BY)
// =============================================================================

TEST_F(ExecutorTest, SortByPointsAscending) {
    LoadTestData(30);

    auto scan = std::make_shared<TableScanExecutor>(heap_.get(), bpm_.get());
    auto sort = std::make_shared<SortExecutor>(scan,
        [](const Tuple& a, const Tuple& b) { return a.points < b.points; });

    sort->Init();

    Tuple prev;
    prev.points = 0;
    int count = 0;
    Tuple tuple;
    while (sort->Next(&tuple)) {
        EXPECT_GE(tuple.points, prev.points);
        prev = tuple;
        count++;
    }
    EXPECT_EQ(count, 30);
}

TEST_F(ExecutorTest, SortByPlayerDescending) {
    LoadTestData(20);

    auto scan = std::make_shared<TableScanExecutor>(heap_.get(), bpm_.get());
    auto sort = std::make_shared<SortExecutor>(scan,
        [](const Tuple& a, const Tuple& b) { return a.player > b.player; });

    sort->Init();

    Tuple prev;
    prev.player = "\xFF";  // Large string
    int count = 0;
    Tuple tuple;
    while (sort->Next(&tuple)) {
        EXPECT_LE(tuple.player, prev.player);
        prev = tuple;
        count++;
    }
    EXPECT_EQ(count, 20);
}

// =============================================================================
// Limit
// =============================================================================

TEST_F(ExecutorTest, LimitBasic) {
    LoadTestData(50);

    auto scan = std::make_shared<TableScanExecutor>(heap_.get(), bpm_.get());
    auto limit = std::make_shared<LimitExecutor>(scan, 10);

    limit->Init();

    int count = 0;
    Tuple tuple;
    while (limit->Next(&tuple)) {
        count++;
    }
    EXPECT_EQ(count, 10);
}

TEST_F(ExecutorTest, LimitLargerThanInput) {
    LoadTestData(5);

    auto scan = std::make_shared<TableScanExecutor>(heap_.get(), bpm_.get());
    auto limit = std::make_shared<LimitExecutor>(scan, 100);

    limit->Init();

    int count = 0;
    Tuple tuple;
    while (limit->Next(&tuple)) {
        count++;
    }
    EXPECT_EQ(count, 5);  // Only 5 records exist
}

TEST_F(ExecutorTest, LimitZero) {
    LoadTestData(10);

    auto scan = std::make_shared<TableScanExecutor>(heap_.get(), bpm_.get());
    auto limit = std::make_shared<LimitExecutor>(scan, 0);

    limit->Init();

    Tuple tuple;
    EXPECT_FALSE(limit->Next(&tuple));
}

// =============================================================================
// Aggregation
// =============================================================================

TEST_F(ExecutorTest, AggCountAll) {
    LoadTestData(50);

    auto scan = std::make_shared<TableScanExecutor>(heap_.get(), bpm_.get());

    // COUNT(*) over all records (single group)
    AggregationExecutor agg(scan,
        [](const Tuple&) -> std::string { return "__all__"; },
        {{AggType::kCount, [](const Tuple&) { return 1.0; }, "count"}});

    agg.Init();

    auto& results = agg.GetResults();
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].group_key, "__all__");
    EXPECT_DOUBLE_EQ(results[0].values[0], 50.0);
}

TEST_F(ExecutorTest, AggSumPoints) {
    LoadTestData(100);

    auto scan = std::make_shared<TableScanExecutor>(heap_.get(), bpm_.get());

    // SUM(points) over all records
    AggregationExecutor agg(scan,
        [](const Tuple&) -> std::string { return "__all__"; },
        {{AggType::kSum,
          [](const Tuple& t) { return static_cast<double>(t.points); },
          "total_points"}});

    agg.Init();

    auto& results = agg.GetResults();
    ASSERT_EQ(results.size(), 1u);
    // Points cycle: 0,1,2,3,0,1,2,3... → sum = 25 * (0+1+2+3) = 150
    EXPECT_DOUBLE_EQ(results[0].values[0], 150.0);
}

TEST_F(ExecutorTest, AggAvgByTeam) {
    LoadTestData(100);

    auto scan = std::make_shared<TableScanExecutor>(heap_.get(), bpm_.get());

    // AVG(points) GROUP BY team
    AggregationExecutor agg(scan,
        [](const Tuple& t) -> std::string { return t.team; },
        {{AggType::kAvg,
          [](const Tuple& t) { return static_cast<double>(t.points); },
          "avg_points"},
         {AggType::kCount,
          [](const Tuple&) { return 1.0; },
          "count"}});

    agg.Init();

    auto& results = agg.GetResults();
    EXPECT_EQ(results.size(), 5u);  // 5 teams

    // Each team has 20 records with points cycling 0,1,2,3
    for (const auto& result : results) {
        EXPECT_EQ(result.values[1], 20.0);  // COUNT = 20 per team
    }
}

TEST_F(ExecutorTest, AggGroupByQuarter) {
    LoadTestData(40);

    auto scan = std::make_shared<TableScanExecutor>(heap_.get(), bpm_.get());

    // COUNT(*) GROUP BY quarter
    AggregationExecutor agg(scan,
        [](const Tuple& t) -> std::string { return std::to_string(t.quarter); },
        {{AggType::kCount, [](const Tuple&) { return 1.0; }, "count"}});

    agg.Init();

    auto& results = agg.GetResults();
    EXPECT_EQ(results.size(), 4u);  // 4 quarters

    double total = 0;
    for (const auto& result : results) {
        total += result.values[0];
    }
    EXPECT_DOUBLE_EQ(total, 40.0);
}

TEST_F(ExecutorTest, AggMinMax) {
    LoadTestData(50);

    auto scan = std::make_shared<TableScanExecutor>(heap_.get(), bpm_.get());

    AggregationExecutor agg(scan,
        [](const Tuple&) -> std::string { return "__all__"; },
        {{AggType::kMin,
          [](const Tuple& t) { return static_cast<double>(t.shot_distance); },
          "min_dist"},
         {AggType::kMax,
          [](const Tuple& t) { return static_cast<double>(t.shot_distance); },
          "max_dist"}});

    agg.Init();

    auto& results = agg.GetResults();
    ASSERT_EQ(results.size(), 1u);
    EXPECT_DOUBLE_EQ(results[0].values[0], 0.0);   // min(i%30) for i=0..49
    EXPECT_DOUBLE_EQ(results[0].values[1], 29.0);  // max(i%30) for i=0..49
}

// =============================================================================
// Top-N (ORDER BY + LIMIT)
// =============================================================================

TEST_F(ExecutorTest, TopNByPoints) {
    LoadTestData(50);

    auto scan = std::make_shared<TableScanExecutor>(heap_.get(), bpm_.get());
    TopNExecutor topn(scan,
        [](const Tuple& a, const Tuple& b) { return a.points > b.points; },
        5);

    topn.Init();

    int count = 0;
    Tuple tuple;
    uint8_t prev_points = 255;
    while (topn.Next(&tuple)) {
        EXPECT_LE(tuple.points, prev_points);
        prev_points = tuple.points;
        count++;
    }
    EXPECT_EQ(count, 5);
}

TEST_F(ExecutorTest, TopNByShotDistance) {
    LoadTestData(100);

    auto scan = std::make_shared<TableScanExecutor>(heap_.get(), bpm_.get());
    TopNExecutor topn(scan,
        [](const Tuple& a, const Tuple& b) {
            return a.shot_distance > b.shot_distance;
        },
        3);

    topn.Init();

    Tuple tuple;
    ASSERT_TRUE(topn.Next(&tuple));
    EXPECT_EQ(tuple.shot_distance, 29);  // Max distance
}

// =============================================================================
// Composed Query Plans
// =============================================================================

TEST_F(ExecutorTest, FilterThenSort) {
    LoadTestData(100);

    // SELECT * FROM events WHERE team='LAL' ORDER BY points DESC
    auto scan = std::make_shared<TableScanExecutor>(heap_.get(), bpm_.get());
    auto filter = std::make_shared<FilterExecutor>(scan,
        [](const Tuple& t) { return t.team == "LAL"; });
    auto sort = std::make_shared<SortExecutor>(filter,
        [](const Tuple& a, const Tuple& b) { return a.points > b.points; });

    sort->Init();

    int count = 0;
    uint8_t prev_points = 255;
    Tuple tuple;
    while (sort->Next(&tuple)) {
        EXPECT_EQ(tuple.team, "LAL");
        EXPECT_LE(tuple.points, prev_points);
        prev_points = tuple.points;
        count++;
    }
    EXPECT_EQ(count, 20);  // 100/5 teams = 20 LAL records
}

TEST_F(ExecutorTest, FilterSortLimit) {
    LoadTestData(200);

    // SELECT * FROM events WHERE points >= 2 ORDER BY shot_distance DESC LIMIT 10
    auto scan = std::make_shared<TableScanExecutor>(heap_.get(), bpm_.get());
    auto filter = std::make_shared<FilterExecutor>(scan,
        [](const Tuple& t) { return t.points >= 2; });
    auto sort = std::make_shared<SortExecutor>(filter,
        [](const Tuple& a, const Tuple& b) {
            return a.shot_distance > b.shot_distance;
        });
    auto limit = std::make_shared<LimitExecutor>(sort, 10);

    limit->Init();

    int count = 0;
    uint16_t prev_dist = 65535;
    Tuple tuple;
    while (limit->Next(&tuple)) {
        EXPECT_GE(tuple.points, 2);
        EXPECT_LE(tuple.shot_distance, prev_dist);
        prev_dist = tuple.shot_distance;
        count++;
    }
    EXPECT_EQ(count, 10);
}

TEST_F(ExecutorTest, NBAQuery_TopScorers) {
    // Simulate: SELECT player, SUM(points) FROM events GROUP BY player
    LoadTestData(100);

    auto scan = std::make_shared<TableScanExecutor>(heap_.get(), bpm_.get());

    AggregationExecutor agg(scan,
        [](const Tuple& t) -> std::string { return t.player; },
        {{AggType::kSum,
          [](const Tuple& t) { return static_cast<double>(t.points); },
          "total_points"},
         {AggType::kCount,
          [](const Tuple&) { return 1.0; },
          "games"}});

    agg.Init();

    auto& results = agg.GetResults();
    EXPECT_EQ(results.size(), 5u);  // 5 players

    double grand_total = 0;
    for (const auto& r : results) {
        grand_total += r.values[0];
    }
    EXPECT_DOUBLE_EQ(grand_total, 150.0);  // Total points across all records
}

TEST_F(ExecutorTest, NBAQuery_ThreePointers) {
    LoadTestData(200);

    // SELECT player, COUNT(*) FROM events WHERE points=3 GROUP BY player
    auto scan = std::make_shared<TableScanExecutor>(heap_.get(), bpm_.get());
    auto filter = std::make_shared<FilterExecutor>(scan,
        [](const Tuple& t) { return t.points == 3; });

    AggregationExecutor agg(filter,
        [](const Tuple& t) -> std::string { return t.player; },
        {{AggType::kCount, [](const Tuple&) { return 1.0; }, "three_pt_count"}});

    agg.Init();

    auto& results = agg.GetResults();
    EXPECT_GT(results.size(), 0u);

    double total_threes = 0;
    for (const auto& r : results) {
        total_threes += r.values[0];
    }
    EXPECT_EQ(total_threes, 50.0);  // 200 records / 4 point values = 50 with points=3
}

// =============================================================================
// Stress
// =============================================================================

TEST_F(ExecutorTest, StressFullPipeline) {
    LoadTestData(1000);

    // Complex query: Filter → Sort → Limit
    auto scan = std::make_shared<TableScanExecutor>(heap_.get(), bpm_.get());
    auto filter = std::make_shared<FilterExecutor>(scan,
        [](const Tuple& t) { return t.quarter <= 2; });
    auto sort = std::make_shared<SortExecutor>(filter,
        [](const Tuple& a, const Tuple& b) {
            return a.home_score > b.home_score;
        });
    auto limit = std::make_shared<LimitExecutor>(sort, 50);

    limit->Init();

    int count = 0;
    uint16_t prev_score = 65535;
    Tuple tuple;
    while (limit->Next(&tuple)) {
        EXPECT_LE(tuple.quarter, 2);
        EXPECT_LE(tuple.home_score, prev_score);
        prev_score = tuple.home_score;
        count++;
    }
    EXPECT_EQ(count, 50);
}

}  // namespace
}  // namespace courtdb
