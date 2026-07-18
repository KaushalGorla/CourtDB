# Subsystem 1: Disk Manager & Page Layout

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                         CourtDB Stack                            │
├─────────────────────────────────────────────────────────────────┤
│  Query Executor  │  B+ Tree Index  │  Heap File  │  Recovery    │
├─────────────────────────────────────────────────────────────────┤
│                     Buffer Pool Manager                          │
├─────────────────────────────────────────────────────────────────┤
│                     ┌─────────────────┐                         │
│                     │  Disk Manager   │  ◄── THIS SUBSYSTEM     │
│                     └─────────────────┘                         │
├─────────────────────────────────────────────────────────────────┤
│                     ┌─────────────────┐                         │
│                     │  Page Layout    │  ◄── THIS SUBSYSTEM     │
│                     └─────────────────┘                         │
├─────────────────────────────────────────────────────────────────┤
│                       OS / Filesystem                            │
└─────────────────────────────────────────────────────────────────┘
```

## Page Layout (Slotted Page)

```
Byte offset: 0                                              4095
             ┌────────┬───────────────┬──────────┬──────────────┐
             │ Header │ Slot Directory│   Free   │ Record Data  │
             │ (20B)  │ (4B × N)      │  Space   │ (grows ←)    │
             └────────┴───────────────┴──────────┴──────────────┘
                       ──── grows → ───           ──── grows ← ──
```

### Why Slotted Pages?

1. **Variable-length records**: NBA events vary in size (player names, descriptions)
2. **O(1) record access**: Slot number → offset lookup is constant time
3. **No external fragmentation**: Records are packed at the end; compaction recovers internal gaps
4. **Stable record IDs**: (page_id, slot_id) never changes even after compaction

### Page Header (20 bytes)

| Field              | Type      | Description                        |
|--------------------|-----------|------------------------------------|
| page_id            | uint32_t  | Self-identifying page number       |
| num_slots          | uint16_t  | Total slots in directory           |
| free_space_offset  | uint16_t  | End of slot directory (free start) |
| free_space_end     | uint16_t  | Start of data region (free end)    |
| num_records        | uint16_t  | Number of live records             |
| next_page_id       | uint32_t  | Link to next page (heap chain)     |
| flags              | uint16_t  | Page type flags                    |

### Slot Entry (4 bytes)

| Field  | Type     | Description               |
|--------|----------|---------------------------|
| offset | uint16_t | Byte offset of record     |
| length | uint16_t | Length of record in bytes  |

## Disk Manager

### File Structure

```
┌──────────┬──────────┬──────────┬──────────┬─────┐
│  Page 0  │  Page 1  │  Page 2  │  Page 3  │ ... │
│ (Header) │  (Data)  │  (Data)  │  (Free)  │     │
└──────────┴──────────┴──────────┴──────────┴─────┘
     │                                  │
     └─ magic, page_count,             └─ next_free → INVALID
        free_list_head ────────────────────┘
```

### Free List

Deallocated pages form a singly-linked list:
- Header stores the head of the free list
- Each free page stores the next free page ID in its first 4 bytes
- Allocation pops from the head (LIFO)
- Deallocation pushes to the head

### I/O Strategy

- **pread/pwrite**: Positioned I/O avoids seek races in concurrent scenarios
- **Page-aligned**: All reads/writes are exactly 4096 bytes at aligned offsets
- **Explicit fsync**: Caller decides when to force durability (important for WAL)

## Performance Considerations

1. **Cache-line alignment**: Page data buffer is `alignas(64)` for optimal CPU cache usage
2. **Minimal allocations**: Slot directory uses `vector::reserve` patterns
3. **Zero-copy reads**: `GetRecord()` returns a pointer into the page buffer
4. **Deferred compaction**: Deletes are O(1) tombstones; compaction amortizes cost
5. **Sequential allocation**: New pages are appended sequentially for disk locality

## Common Interview Questions

1. **Why 4KB pages?** Matches OS page size and filesystem block size. Minimizes wasted I/O bandwidth. Enables memory-mapped I/O compatibility.

2. **Why slotted pages over fixed-size records?** NBA data has variable-length fields (player names, descriptions). Fixed-size would waste space or require overflow pages for everything.

3. **How does the free list work?** LIFO linked list through page content. O(1) push/pop. No separate data structure needed — the free pages themselves store the chain.

4. **Why not use mmap?** Direct I/O with pread/pwrite gives explicit control over what's in memory (required for buffer pool). mmap delegates eviction to the OS, which doesn't understand database access patterns.

5. **What happens when a record is too large for a page?** Handled by overflow pages (to be implemented). The max record per page is ~4072 bytes.

6. **How does compaction work without invalidating record IDs?** Slot IDs are stable — compaction moves record data but updates the offset in the slot directory. External references (page_id, slot_id) remain valid.

## Files

```
include/storage/page.h           - Page class declaration
include/disk/disk_manager.h      - DiskManager class declaration  
src/storage/page.cpp             - Slotted page implementation
src/disk/disk_manager.cpp        - Disk I/O implementation
tests/storage/page_test.cpp      - 21 unit tests for page layout
tests/disk/disk_manager_test.cpp - 18 unit tests for disk manager
```
