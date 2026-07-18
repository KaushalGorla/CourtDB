# Subsystem 5: Query Executor

## Architecture (Volcano Iterator Model)

```
┌─────────────────────────────────────────────────────────────────┐
│                         Query Plan Tree                          │
│                                                                  │
│              ┌──────────┐                                       │
│              │  Limit   │  ← LIMIT 10                           │
│              └────┬─────┘                                       │
│                   │                                              │
│              ┌────┴─────┐                                       │
│              │   Sort   │  ← ORDER BY points DESC               │
│              └────┬─────┘                                       │
│                   │                                              │
│              ┌────┴─────┐                                       │
│              │  Filter  │  ← WHERE team = 'LAL'                 │
│              └────┬─────┘                                       │
│                   │                                              │
│              ┌────┴─────┐                                       │
│              │TableScan │  ← FROM events (heap file)            │
│              └──────────┘                                       │
│                                                                  │
│  Data flows UP via Next() calls (pull-based)                    │
└─────────────────────────────────────────────────────────────────┘
```

## Operators

### TableScanExecutor
- Wraps a HeapFileIterator for sequential access
- Deserializes each record into a Tuple (NBARecord)
- O(N) — must visit every record

### IndexScanExecutor
- Uses B+ tree for point lookups or range scans
- Fetches matching RIDs, then reads records from heap
- O(log N + K) — dramatically faster for selective queries

### FilterExecutor
- Evaluates a predicate on each input tuple
- Passes through only matching tuples
- Supports arbitrary predicates via `std::function<bool(const Tuple&)>`
- Implements predicate pushdown when composed below sort/limit

### ProjectionExecutor
- Transforms tuples (select fields, compute derived values)
- O(1) per tuple — no state

### SortExecutor (Blocking)
- Materializes all input on Init()
- Sorts using std::sort with a custom comparator
- Emits tuples in sorted order via Next()
- O(N log N) — blocking operator

### LimitExecutor
- Counts emitted tuples, stops at the limit
- O(1) per Next() call — passes through from child

### AggregationExecutor (Blocking)
- Materializes all input, groups by key
- Computes COUNT, SUM, AVG, MIN, MAX per group
- Hash-based grouping using unordered_map
- O(N) materialization + O(G) result emission

### TopNExecutor (Optimized)
- Combines ORDER BY + LIMIT into a single operator
- Uses std::partial_sort for O(N log K) instead of O(N log N)
- Significant win when K << N (e.g., top 10 of 8M records)

## Operator Composition

Operators are composed using shared_ptr child references:

```cpp
// SELECT player, points FROM events WHERE team='GSW' ORDER BY points DESC LIMIT 5

auto scan   = make_shared<TableScanExecutor>(heap, bpm);
auto filter = make_shared<FilterExecutor>(scan,
    [](const Tuple& t) { return t.team == "GSW"; });
auto sort   = make_shared<SortExecutor>(filter,
    [](const Tuple& a, const Tuple& b) { return a.points > b.points; });
auto limit  = make_shared<LimitExecutor>(sort, 5);

limit->Init();
Tuple tuple;
while (limit->Next(&tuple)) {
    // Process result
}
```

## NBA Analytics Examples

| Query | Operators |
|-------|-----------|
| All 3-pointers by Curry | Filter(player="Curry" AND points=3) → TableScan |
| Top 10 longest shots | TopN(shot_distance DESC, 10) → TableScan |
| Points per team | Aggregation(GROUP BY team, SUM(points)) → TableScan |
| Q4 scoring leaders | Filter(quarter=4) → Aggregation(GROUP BY player, SUM) |
| Season scoring trends | Filter(event_type=MadeShot) → Aggregation(GROUP BY season) |

## Performance Characteristics

| Operator | Time | Space | Blocking? |
|----------|------|-------|-----------|
| TableScan | O(N) | O(1) | No |
| IndexScan | O(log N + K) | O(K) | Partial |
| Filter | O(input) | O(1) | No |
| Projection | O(input) | O(1) | No |
| Sort | O(N log N) | O(N) | Yes |
| Limit | O(limit) | O(1) | No |
| Aggregation | O(N) | O(G) | Yes |
| TopN | O(N log K) | O(N) | Yes |

## Design Trade-offs

1. **Volcano vs Vectorized**: Volcano is simpler but has per-tuple overhead. Vectorized (batch) execution would be faster for analytical queries. Could be a future optimization.

2. **Materialization**: Sort and Aggregation materialize all input. For 8M records, this means ~680MB in memory. Production systems use external sorting.

3. **Predicate Pushdown**: By placing Filter below Sort, we reduce the sort input. The optimizer would choose this automatically in a full system.

4. **Index Selection**: IndexScan vs TableScan depends on selectivity. <15% selectivity → use index; >15% → full scan is faster.

## Common Interview Questions

1. **Why Volcano model over materialization?**  
   Pipelining avoids materializing intermediate results. A Filter→Limit query can stop after K matches without scanning the entire table.

2. **When is Sort better than TopN?**  
   When you need ALL results sorted (no LIMIT), or when K is close to N. partial_sort only wins when K << N.

3. **How would you implement external sort for 8M records?**  
   Split input into sorted runs that fit in memory, write runs to disk, then merge runs using a min-heap (k-way merge). I/O optimal: O(N/B * log_{M/B}(N/B)) page accesses.

4. **How does predicate pushdown work?**  
   Move filter predicates below expensive operators (joins, sorts). If we filter before sorting, we sort fewer tuples. In CourtDB, placing Filter below Sort is a manual optimization.

5. **How would you implement a hash join?**  
   Build phase: hash the smaller table into an in-memory hash table. Probe phase: for each tuple in the larger table, probe the hash table. O(N+M) time, O(min(N,M)) space.

## Files

```
include/query/executor.h       - All operator declarations (7 executors)
src/query/executor.cpp         - Full implementation (~260 lines)
tests/query/executor_test.cpp  - 27 comprehensive tests
```

## Test Summary

| Category | Tests | Status |
|----------|-------|--------|
| Table scan | 3 | ✓ |
| Index scan | 2 | ✓ |
| Filter | 4 | ✓ |
| Projection | 1 | ✓ |
| Sort | 2 | ✓ |
| Limit | 3 | ✓ |
| Aggregation | 5 | ✓ |
| TopN | 2 | ✓ |
| Composed plans | 2 | ✓ |
| NBA queries | 2 | ✓ |
| Stress | 1 | ✓ |
| **Total** | **27** | **All pass** |
