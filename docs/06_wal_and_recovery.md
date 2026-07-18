# Subsystem 6: Write-Ahead Logging & Recovery

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                     Application Layer                            │
│  (HeapFile.InsertRecord, BPlusTree.Insert, etc.)                │
├─────────────────────────────────────────────────────────────────┤
│              ┌──────────────────────────┐                       │
│              │    Recovery Manager      │ ◄── THIS SUBSYSTEM    │
│              │  ┌────────────────────┐  │                       │
│              │  │    Log Manager     │  │                       │
│              │  │  ┌──────────────┐  │  │                       │
│              │  │  │ Log Records  │  │  │                       │
│              │  │  └──────────────┘  │  │                       │
│              │  └────────────────────┘  │                       │
│              └──────────────────────────┘                       │
├─────────────────────────────────────────────────────────────────┤
│                   Buffer Pool Manager                            │
├─────────────────────────────────────────────────────────────────┤
│         ┌──────────┐            ┌──────────┐                    │
│         │ Data File│            │ WAL File │                    │
│         │ (.db)    │            │ (.wal)   │                    │
│         └──────────┘            └──────────┘                    │
└─────────────────────────────────────────────────────────────────┘
```

## Write-Ahead Protocol

```
Normal Operation:
1. Application calls RecoveryManager::LogInsert(page_id, slot_id, data)
2. LogManager appends record to WAL buffer
3. Application modifies the page in the buffer pool
4. Page is marked dirty
5. Eventually: log flushed to disk (BEFORE dirty page is evicted)
6. Eventually: dirty page written to data file

Key Rule: LOG RECORD MUST BE DURABLE BEFORE DATA PAGE WRITE
```

## Log Record Format

```
┌────────────────────────────────────────────────────────────────┐
│ [4B total_size][8B LSN][1B type][4B page_id][2B slot][2B payload_len] │
│ [... payload bytes ...]                                         │
└────────────────────────────────────────────────────────────────┘
Header: 21 bytes fixed
Payload: variable (depends on record type)
```

### Record Types

| Type | Payload | Purpose |
|------|---------|---------|
| INSERT | record bytes | Redo an insert |
| DELETE | old record bytes | Redo a delete (stores data for potential undo) |
| UPDATE | [old_len][old][new_len][new] | Redo an update |
| NEW_PAGE | empty | Page allocation |
| CHECKPOINT_BEGIN | empty | Marks start of checkpoint |
| CHECKPOINT_END | dirty page table | Marks end of checkpoint |

## Log Sequence Numbers (LSN)

- LSN = byte offset of the record in the log file
- Monotonically increasing (never reused)
- Enables O(1) seeking to any record
- Used to determine if a page needs redo:
  - If page_LSN >= log_record_LSN → already applied (skip)
  - If page_LSN < log_record_LSN → needs redo

## Recovery Algorithm (ARIES-simplified, Redo-Only)

```
On Database Startup:
┌─────────────────────────────────────────┐
│ 1. Open WAL file                        │
│ 2. Scan from beginning (or checkpoint)  │
│ 3. For each data-modifying record:      │
│    a. Fetch affected page               │
│    b. If record not yet applied → redo  │
│    c. Mark page dirty                   │
│ 4. Flush all recovered pages            │
│ 5. Database is consistent               │
└─────────────────────────────────────────┘
```

### Why Redo-Only (No Undo)?

CourtDB is single-user with no transactions. Every write is implicitly committed immediately. There are no aborted transactions that need to be undone. This dramatically simplifies recovery.

## Checkpointing

```
Checkpoint Process:
1. Write CHECKPOINT_BEGIN to log
2. Flush ALL dirty pages from buffer pool to data file
3. Write CHECKPOINT_END to log (with dirty page table)
4. Flush the log

After Checkpoint:
- All data modifications are durable in the data file
- Log records before the checkpoint are no longer needed for recovery
- Log can be truncated to save space
```

## Log Manager Buffering

```
┌─────────────────────────────────────────────────────────────────┐
│                    Log Buffer (64KB)                             │
│  ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐ ┌─────────────────────┐  │
│  │Rec 1 │ │Rec 2 │ │Rec 3 │ │Rec 4 │ │   Free Space        │  │
│  └──────┘ └──────┘ └──────┘ └──────┘ └─────────────────────┘  │
│  ←─── buffer_offset_ ───→                                      │
└─────────────────────────────────────────────────────────────────┘
                    │
                    │ Flush (when full or explicit)
                    ▼
┌─────────────────────────────────────────────────────────────────┐
│                    WAL File (append-only)                        │
│  [Rec 1][Rec 2][Rec 3][Rec 4][Rec 5]...                        │
│  ←─── flushed_lsn_ ───→                                        │
└─────────────────────────────────────────────────────────────────┘
```

## Performance Characteristics

| Operation | Cost |
|-----------|------|
| Log append | O(1) amortized (buffered) |
| Log flush | 1 sequential write + fsync |
| Recovery | O(L) where L = log records since checkpoint |
| Checkpoint | O(D) dirty page writes + 1 log flush |

## Common Interview Questions

1. **Why Write-Ahead Logging?**  
   Ensures atomicity and durability. If we crash after logging but before writing the data page, we can redo from the log. If we wrote the data page without logging, a partial write could corrupt the database.

2. **Why is the log append-only?**  
   Sequential writes are 10-100x faster than random writes on disk. Append-only also simplifies concurrency (no read-write conflicts on the log).

3. **What's the relationship between LSN and pages?**  
   Each page has a pageLSN — the LSN of the most recent log record applied to it. During recovery, if pageLSN >= recordLSN, the change was already applied (skip). This makes recovery idempotent.

4. **How do checkpoints reduce recovery time?**  
   Without checkpoints, recovery must replay the entire log from the beginning. With checkpoints, only records after the last checkpoint need to be replayed. For 8M records, this reduces recovery from minutes to seconds.

5. **Why not use undo logging?**  
   Undo is needed for transaction abort in multi-user systems. CourtDB is single-user with no transactions — all writes commit immediately. Redo-only halves the log volume and simplifies recovery.

6. **How would you implement group commit?**  
   Buffer multiple log records, then flush them all with a single fsync. This amortizes the expensive fsync across many operations. The trade-off: slightly higher latency per record, but much higher throughput.

7. **What happens if the log file itself is corrupted?**  
   A torn write to the log can be detected by the total_size field (if it doesn't match actual bytes, the record is incomplete). Recovery stops at the last complete record.

## Files

```
include/recovery/log_record.h        - Log record types and serialization
include/recovery/log_manager.h       - WAL manager with buffered append
include/recovery/recovery_manager.h  - Crash recovery coordinator
src/recovery/log_record.cpp          - Log record factory + serialize/deserialize
src/recovery/log_manager.cpp         - Buffered WAL I/O implementation
src/recovery/recovery_manager.cpp    - Redo recovery + checkpointing
tests/recovery/recovery_test.cpp     - 18 comprehensive tests
```

## Test Summary

| Category | Tests | Status |
|----------|-------|--------|
| Log record serialization | 5 | ✓ |
| Log manager operations | 6 | ✓ |
| Recovery (redo/checkpoint) | 7 | ✓ |
| **Total** | **18** | **All pass** |
