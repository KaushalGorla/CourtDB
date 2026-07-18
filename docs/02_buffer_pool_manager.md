# Subsystem 2: Buffer Pool Manager

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│  Query Executor  │  B+ Tree Index  │  Heap File  │  Recovery    │
├─────────────────────────────────────────────────────────────────┤
│                   ┌────────────────────────┐                    │
│                   │  Buffer Pool Manager   │ ◄── THIS SUBSYSTEM │
│                   │  ┌──────────────────┐  │                    │
│                   │  │   LRU Replacer   │  │                    │
│                   │  └──────────────────┘  │                    │
│                   │  ┌──────────────────┐  │                    │
│                   │  │   Page Table     │  │                    │
│                   │  └──────────────────┘  │                    │
│                   │  ┌──────────────────┐  │                    │
│                   │  │   Frame Array    │  │                    │
│                   │  └──────────────────┘  │                    │
│                   └────────────────────────┘                    │
├─────────────────────────────────────────────────────────────────┤
│                       Disk Manager                              │
└─────────────────────────────────────────────────────────────────┘
```

## Components

### LRU Replacer

```
Most Recent ←──── list ────→ Least Recent (Victim)
  [Frame 3] ←→ [Frame 7] ←→ [Frame 1] ←→ [Frame 5]
                                              ↑
                                        Next eviction
```

- **Data structure**: `std::list<FrameId>` + `std::unordered_map<FrameId, iterator>`
- **RecordAccess**: Splice to front — O(1)
- **Remove**: Erase from map and list — O(1)  
- **Evict**: Pop from back — O(1)
- Only contains frames with `pin_count == 0`

### Page Table

- `std::unordered_map<PageId, FrameId>` — maps logical page IDs to physical frame slots
- O(1) average lookup
- Entries added on page load, removed on eviction/delete

### Frame Array

- Fixed-size `std::vector<Page>` — the actual memory holding page data
- Accompanied by `std::vector<FrameMetadata>` — per-frame pin count, dirty bit, page_id
- Size determined at construction, never changes

## Operation Flow

### FetchPage(page_id)

```
1. Check page_table_ for page_id
   ├── HIT: increment pin_count, remove from LRU, return page pointer
   └── MISS:
       2. Find free frame (free_list_ or LRU eviction)
       3. If victim is dirty → serialize + write to disk
       4. pread(page_id) into the frame
       5. DeserializeFromBuffer()
       6. Update page_table_, metadata_
       7. Return page pointer (pin_count = 1)
```

### UnpinPage(page_id, is_dirty)

```
1. Look up frame in page_table_
2. Decrement pin_count
3. Set dirty flag (sticky: once dirty, stays dirty until flush)
4. If pin_count == 0 → add to LRU replacer
```

### Eviction

```
1. Call replacer_.Evict() → get victim frame_id
2. If victim is dirty:
   a. SerializeToBuffer() (writes header+slots into raw data_[])
   b. WritePage() to disk
3. Remove from page_table_
4. Clear frame metadata
5. Frame is now available for new page
```

## Key Design Decisions

| Decision | Rationale |
|----------|-----------|
| Coarse-grained mutex | Simple, correct. Fine-grained locking is premature for an embedded DB |
| Pin counting (not binary) | Supports concurrent readers/writers of the same page |
| Sticky dirty flag | A page stays dirty until explicitly flushed, even if subsequent accesses are read-only |
| Free list + LRU | Free list avoids eviction overhead when pool isn't full yet |
| Stats tracking | Essential for tuning pool size and diagnosing performance issues |

## Performance Considerations

1. **Hash map for page table**: O(1) average lookup, critical for fetch-heavy workloads
2. **List + map for LRU**: True O(1) for all operations (no priority queue overhead)
3. **Frame reuse**: Pages are not allocated/deallocated — frames are pre-allocated and reused
4. **Batch flush**: `FlushAllPages()` does a single fsync after all writes
5. **Eviction cost**: Only paid on cache miss with full pool; dirty pages are the expensive case

## Common Interview Questions

1. **Why LRU over Clock/LRU-K/2Q?**  
   LRU is the simplest correct choice. For analytical workloads with sequential scans, LRU can thrash, but it's the baseline. Clock approximation saves memory for large pools. LRU-K (frequency-based) is better for mixed workloads.

2. **What happens if all pages are pinned?**  
   `FetchPage` and `NewPage` return nullptr. The caller must handle this (e.g., retry after another thread unpins). In production, this indicates the pool is too small.

3. **Why serialize before eviction?**  
   The Page class maintains both a raw byte buffer (`data_[]`) and deserialized in-memory structures (`header_`, `slots_`). Inserts modify the in-memory structures and write record data directly to `data_[]`, but the header/slots in `data_[]` may be stale. `SerializeToBuffer()` makes `data_[]` consistent before disk write.

4. **How would you make this thread-safe with better concurrency?**  
   - Replace coarse mutex with per-frame latches
   - Use a concurrent hash map for page_table_
   - Partition the LRU list (e.g., one per hash bucket)
   - Use optimistic latching for read-heavy workloads

5. **How does the buffer pool interact with WAL?**  
   Before a dirty page is flushed to disk, the corresponding log records must be flushed first (Write-Ahead Logging protocol). This ensures crash recovery can redo any operation whose page write was lost.

## Files

```
include/buffer/buffer_pool_manager.h       - BPM + LRU Replacer declarations
src/buffer/buffer_pool_manager.cpp         - Full implementation
tests/buffer/buffer_pool_manager_test.cpp  - 25 unit tests (7 LRU + 18 BPM)
```

## Test Summary

| Category | Tests | Status |
|----------|-------|--------|
| LRU Replacer | 7 | ✓ |
| Basic operations | 4 | ✓ |
| Pin/Unpin | 2 | ✓ |
| Eviction & dirty pages | 3 | ✓ |
| Delete page | 2 | ✓ |
| Flush | 2 | ✓ |
| Statistics | 2 | ✓ |
| Stress tests | 4 | ✓ |
| **Total** | **25** | **All pass** |
