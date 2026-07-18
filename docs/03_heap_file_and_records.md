# Subsystem 3: Heap File & Record Serialization

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│  Query Executor  │  B+ Tree Index  │  Recovery                  │
├─────────────────────────────────────────────────────────────────┤
│              ┌──────────────────────────┐                       │
│              │       Heap File          │ ◄── THIS SUBSYSTEM    │
│              │  ┌────────────────────┐  │                       │
│              │  │ Record Serializer  │  │                       │
│              │  └────────────────────┘  │                       │
│              │  ┌────────────────────┐  │                       │
│              │  │ Page Chain Manager │  │                       │
│              │  └────────────────────┘  │                       │
│              │  ┌────────────────────┐  │                       │
│              │  │ Free Space Dir     │  │                       │
│              │  └────────────────────┘  │                       │
│              └──────────────────────────┘                       │
├─────────────────────────────────────────────────────────────────┤
│                   Buffer Pool Manager                            │
├─────────────────────────────────────────────────────────────────┤
│                     Disk Manager                                 │
└─────────────────────────────────────────────────────────────────┘
```

## Record Serialization Format

```
NBA Play-by-Play Record (binary format):
┌──────────────────────────────────────────────────────┐
│ [2B len][game_id bytes]                              │
│ [2B season]                                          │
│ [1B quarter]                                         │
│ [2B len][clock bytes]                                │
│ [2B len][team bytes]                                 │
│ [2B len][player bytes]                               │
│ [1B event_type]                                      │
│ [2B len][description bytes]                          │
│ [1B points]                                          │
│ [2B shot_distance]                                   │
│ [2B home_score]                                      │
│ [2B away_score]                                      │
└──────────────────────────────────────────────────────┘
Fixed overhead: 21 bytes (length prefixes + fixed fields)
Typical total: 60-120 bytes per record
```

## Heap File Page Chain

```
first_page_id_                                    last_page_id_
     │                                                  │
     ▼                                                  ▼
┌─────────┐     ┌─────────┐     ┌─────────┐     ┌─────────┐
│  Page 1 │────▶│  Page 3 │────▶│  Page 7 │────▶│ Page 12 │──▶ NULL
│ 45 recs │     │ 42 recs │     │ 48 recs │     │ 30 recs │
│ 200B free│     │ 500B free│    │  50B free│     │2000B free│
└─────────┘     └─────────┘     └─────────┘     └─────────┘
                      ▲                                 ▲
                      │                                 │
              pages_with_space_ = [3, 12]
```

## Record ID (RID)

A record is uniquely addressed by `(page_id, slot_id)`:
- **page_id**: Which page in the chain contains this record
- **slot_id**: Which slot within that page's directory

RIDs are stable across compaction (slot directory preserves slot indices).
RIDs may change on update if the new record doesn't fit in the same page.

## Key Operations

### Insert
1. Check `pages_with_space_` for a page with enough room
2. If found → fetch page, insert, done
3. If not → allocate new page, link into chain, insert

### Delete
1. Fetch page by `rid.page_id`
2. Mark slot as deleted (tombstone)
3. Add page to `pages_with_space_` (it now has room)

### Sequential Scan (Iterator)
1. Start at `first_page_id_`, slot 0
2. Skip empty/deleted slots
3. When page exhausted, follow `next_page_id` link
4. Stop when `next_page_id == INVALID_PAGE_ID` and no more slots

### Bulk Insert
- Batches inserts to minimize pin/unpin overhead
- Keeps current page pinned while inserting multiple records
- Only unpins when the page is full or all records are inserted

## Performance Considerations

1. **Free space directory** avoids O(N) page scans on every insert
2. **Bulk insert** amortizes pin/unpin cost (important for 8M+ records)
3. **Zero-copy reads**: `GetRecord` returns a pointer into the pinned page buffer
4. **Page chain locality**: New pages are appended sequentially on disk
5. **Slot reuse**: Deleted slots are recycled without compaction

## Capacity Estimates (NBA Dataset)

| Metric | Value |
|--------|-------|
| Avg record size | ~85 bytes |
| Records per page | ~45 |
| Pages for 8M records | ~178,000 |
| Disk space (data only) | ~700 MB |
| Scan throughput (cached) | ~39M records/sec |

## Common Interview Questions

1. **Why a heap file instead of a sorted file?**  
   Heap files support O(1) inserts (append). For analytical queries, a full scan is often needed anyway. Sorted access is provided by B+ tree indexes.

2. **How does the iterator handle concurrent modifications?**  
   This is a single-threaded embedded DB. The iterator pins pages as it reads them. In a concurrent system, you'd need snapshot isolation or MVCC.

3. **What about records larger than a page?**  
   Records > ~4KB would need overflow pages. For NBA data, the largest records are ~200 bytes, well within a single page.

4. **Why length-prefixed strings instead of null-terminated?**  
   Length-prefix allows O(1) size knowledge, supports binary data, and enables skipping fields without scanning for null bytes.

5. **How would you implement a clustered index?**  
   Replace the heap file with a B+ tree where leaf nodes store full records (not just pointers). Inserts require maintaining sort order.

6. **What's the trade-off of the free space directory?**  
   It's an in-memory approximation that may be stale (page could fill up between checks). But it avoids the alternative: scanning every page's header on every insert. For 178K pages, that's a significant saving.

## Files

```
include/storage/record.h         - NBARecord schema + serializer declaration
include/storage/heap_file.h      - HeapFile + iterator declaration
src/storage/record.cpp           - Binary serialization implementation
src/storage/heap_file.cpp        - Heap file + page chain management
tests/storage/heap_file_test.cpp - 22 tests (4 record + 3 RID + 15 heap)
```

## Test Summary

| Category | Tests | Status |
|----------|-------|--------|
| Record serialization | 4 | ✓ |
| RID operations | 3 | ✓ |
| Basic heap operations | 7 | ✓ |
| Sequential scan | 3 | ✓ |
| Bulk insert | 1 | ✓ |
| Free space reuse | 1 | ✓ |
| Stress tests | 2 | ✓ |
| **Total** | **22** | **All pass** |
