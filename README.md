# CourtDB

An embedded analytical database built from scratch in C++17, designed for NBA play-by-play analytics. No external storage engines — every layer from disk I/O to query execution is hand-built.

## Quick Start

```bash
# Build the library
make lib

# Build and run all tests (downloads GoogleTest automatically)
make test

# Build with optimizations
BUILD=release make lib
```

## Using CourtDB in Your Code

```cpp
#include "disk/disk_manager.h"
#include "buffer/buffer_pool_manager.h"
#include "storage/heap_file.h"
#include "storage/record.h"
#include "index/btree.h"
#include "query/executor.h"
#include "recovery/recovery_manager.h"

using namespace courtdb;

int main() {
    // 1. Initialize the storage stack
    DiskManager disk("nba.db");
    BufferPoolManager bpm(1024, &disk);  // 1024 pages = 4MB cache
    LogManager log("nba.wal");
    RecoveryManager recovery(&log, &bpm, &disk);

    // 2. Run crash recovery on startup
    recovery.Recover();

    // 3. Create a table (heap file)
    HeapFile events(&bpm);

    // 4. Insert NBA records
    NBARecord record;
    record.game_id = "0022300001";
    record.season = 2024;
    record.quarter = 1;
    record.clock = "11:42";
    record.team = "LAL";
    record.player = "LeBron James";
    record.event_type = static_cast<uint8_t>(EventType::kMadeShot);
    record.description = "LeBron James 26' 3PT Jump Shot";
    record.points = 3;
    record.shot_distance = 26;
    record.home_score = 3;
    record.away_score = 0;

    auto bytes = RecordSerializer::Serialize(record);
    RID rid = events.InsertRecord(bytes.data(), bytes.size());

    // 5. Build an index on season
    BPlusTree season_index(&bpm);
    season_index.Insert(record.season, rid);

    // 6. Query: Find all 2024 events
    auto results = season_index.Lookup(2024);

    // 7. Query: Full table scan with filter + aggregation
    auto scan = std::make_shared<TableScanExecutor>(&events, &bpm);
    auto filter = std::make_shared<FilterExecutor>(scan,
        [](const Tuple& t) { return t.team == "LAL"; });

    filter->Init();
    Tuple tuple;
    while (filter->Next(&tuple)) {
        // Process LAL events...
    }

    // 8. Checkpoint for durability
    recovery.Checkpoint();

    return 0;
}
```

## Compiling Your Program

```bash
# Build the library first
make lib

# Compile your program against it
g++ -std=c++17 -O2 -I include your_program.cpp -L build/lib -lcourtdb -o your_program
```

## Analytical Query Examples

### Top 10 scorers
```cpp
auto scan = std::make_shared<TableScanExecutor>(&events, &bpm);
AggregationExecutor agg(scan,
    [](const Tuple& t) { return t.player; },  // GROUP BY player
    {{AggType::kSum,
      [](const Tuple& t) { return (double)t.points; },
      "total_points"}});
agg.Init();
// results sorted by total_points...
```

### 3-pointers by team in Q4
```cpp
auto scan = std::make_shared<TableScanExecutor>(&events, &bpm);
auto filter = std::make_shared<FilterExecutor>(scan,
    [](const Tuple& t) { return t.points == 3 && t.quarter == 4; });
AggregationExecutor agg(filter,
    [](const Tuple& t) { return t.team; },
    {{AggType::kCount, [](const Tuple&) { return 1.0; }, "count"}});
agg.Init();
```

### Top 5 longest made shots
```cpp
auto scan = std::make_shared<TableScanExecutor>(&events, &bpm);
auto filter = std::make_shared<FilterExecutor>(scan,
    [](const Tuple& t) { return t.event_type == (uint8_t)EventType::kMadeShot; });
TopNExecutor topn(filter,
    [](const Tuple& a, const Tuple& b) {
        return a.shot_distance > b.shot_distance;
    }, 5);
topn.Init();
```

### Range scan with index (seasons 2020-2023)
```cpp
auto results = season_index.RangeScan(2020, 2023);
for (auto& [key, rid] : results) {
    const uint8_t* data; uint16_t len;
    events.GetRecord(rid, &data, &len);
    auto record = RecordSerializer::Deserialize(data, len);
    bpm.UnpinPage(rid.page_id, false);
}
```

## Architecture

```
┌──────────────────────────────────────────────────┐
│              Query Executor (Volcano)             │
│  TableScan │ IndexScan │ Filter │ Sort │ Agg     │
├──────────────────────────────────────────────────┤
│  Heap File (Table)     │   B+ Tree (Index)       │
├──────────────────────────────────────────────────┤
│              Buffer Pool Manager (LRU)            │
├──────────────────────────────────────────────────┤
│         Disk Manager         │    WAL Manager     │
├──────────────────────────────────────────────────┤
│              Operating System / Filesystem        │
└──────────────────────────────────────────────────┘
```

## Project Stats

- **~9,600 lines** of C++17
- **156 unit tests** (all passing)
- **0 external dependencies** (beyond STL + GoogleTest for tests)
- **6 subsystems**: Disk I/O, Buffer Pool, Heap Storage, B+ Tree, Query Executor, WAL/Recovery

## Running Benchmarks

```bash
BUILD=release make lib
g++ -std=c++17 -O2 -I include benchmarks/storage/page_benchmark.cpp src/storage/page.cpp -o build/page_bench
g++ -std=c++17 -O2 -I include benchmarks/disk/disk_benchmark.cpp src/disk/disk_manager.cpp src/storage/page.cpp -lstdc++fs -o build/disk_bench
./build/page_bench
./build/disk_bench
```

## Documentation

Detailed architecture docs for each subsystem are in the `docs/` folder:
- [01 - Disk & Page Layout](docs/01_disk_and_page_layout.md)
- [02 - Buffer Pool Manager](docs/02_buffer_pool_manager.md)
- [03 - Heap File & Records](docs/03_heap_file_and_records.md)
- [04 - B+ Tree Index](docs/04_bplus_tree_index.md)
- [05 - Query Executor](docs/05_query_executor.md)
- [06 - WAL & Recovery](docs/06_wal_and_recovery.md)
