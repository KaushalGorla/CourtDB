# Subsystem 4: B+ Tree Index

## Architecture

```
                        ┌─────────────────┐
                        │   Root (Internal)│
                        │  [50 | 100 | 200]│
                        └──┬────┬────┬────┬┘
                           │    │    │    │
              ┌────────────┘    │    │    └────────────┐
              ▼                 ▼    ▼                 ▼
    ┌──────────────┐  ┌──────────────┐  ┌──────────────┐
    │ Internal Node│  │ Internal Node│  │ Internal Node│
    │ [10|20|30|40]│  │ [60|70|80|90]│  │[150|160|170] │
    └──┬──┬──┬──┬──┘  └──┬──┬──┬──┬──┘  └──┬──┬──┬──┘
       │  │  │  │  │      │  │  │  │  │      │  │  │  │
       ▼  ▼  ▼  ▼  ▼      ▼  ▼  ▼  ▼  ▼      ▼  ▼  ▼  ▼
    ┌────┐┌────┐...    ┌────┐┌────┐...    ┌────┐┌────┐...
    │Leaf││Leaf│       │Leaf││Leaf│       │Leaf││Leaf│
    │1-9 ││10- │       │50- ││60- │       │100-││150-│
    └──┬─┘└──┬─┘       └──┬─┘└──┬─┘       └──┬─┘└──┬─┘
       └──►──┘             └──►──┘             └──►──┘
     (next_leaf links for range scans)
```

## Node Layout in Pages

### Internal Node (after page header byte 20)

```
Offset: 20
┌──────────────────────────────────────────────────────────┐
│ [1B type][4B parent][2B num_keys][4B next(unused)]       │  <- Header (11B)
│ [4B child_0][4B key_0][4B child_1][4B key_1]...[4B last]│  <- Data
└──────────────────────────────────────────────────────────┘
Entry size: 8 bytes (key + child), plus 4 bytes for extra child
Max keys: (4065 - 4) / 8 = 507
```

### Leaf Node (after page header byte 20)

```
Offset: 20
┌──────────────────────────────────────────────────────────┐
│ [1B type][4B parent][2B num_keys][4B next_leaf]          │  <- Header (11B)
│ [4B key_0][4B page_id_0][2B slot_0]                     │  <- Entry 0 (10B)
│ [4B key_1][4B page_id_1][2B slot_1]                     │  <- Entry 1
│ ...                                                      │
└──────────────────────────────────────────────────────────┘
Entry size: 10 bytes (4B key + 6B RID)
Max entries: 4065 / 10 = 406
```

## Key Operations

### Insert(key, RID)

```
1. If tree is empty → create leaf root, insert, done
2. FindLeafPage(key) → traverse from root using binary search
3. If leaf has space → insert in sorted position, done
4. Leaf is full → SPLIT:
   a. Create temporary array with all entries + new entry
   b. Split at midpoint: left keeps [0, mid), right gets [mid, end)
   c. Link right as next_leaf of left
   d. Push first key of right up to parent (InsertIntoParent)
   e. If parent full → recursively split parent
   f. If root splits → create new root (tree grows taller)
```

### Lookup(key)

```
1. FindLeafPage(key) → O(log N) traversal
2. Binary search within leaf for key
3. Collect all matching (key, RID) pairs
4. If duplicates span leaf boundary → follow next_leaf links
```

### RangeScan(low, high)

```
1. FindLeafPage(low) → find starting leaf
2. Binary search for first key >= low
3. Scan forward collecting (key, RID) pairs where key <= high
4. Follow next_leaf links until key > high or end of chain
```

### Delete(key, RID) — Lazy

```
1. FindLeafPage(key)
2. Binary search for (key, RID) match
3. Remove entry, shift remaining entries
4. No rebalancing (lazy delete — nodes can become underfull)
```

## Splitting Details

### Leaf Split

```
Before:                          After:
┌─────────────────────┐          ┌───────────┐  ┌───────────┐
│ [1,2,3,4,5,6,7,NEW] │   →     │ [1,2,3,4] │──│ [5,6,7,NEW]│
└─────────────────────┘          └───────────┘  └───────────┘
                                       push key=5 to parent
```

### Internal Split

```
Before:                              After:
┌───────────────────────────┐        ┌─────────┐     ┌─────────┐
│ [k0,k1,k2,k3,k4,k5,NEW] │  →    │ [k0,k1,k2]│   │[k4,k5,NEW]│
└───────────────────────────┘        └─────────┘     └─────────┘
                                         push k3 to parent
```

### Root Split

```
Before:              After:
┌──────────┐         ┌────────────┐  ← New root
│  Root    │    →    │ [split_key]│
│ (full)   │         └──┬──────┬──┘
└──────────┘            ▼      ▼
                    ┌──────┐ ┌──────┐
                    │ Left │ │ Right│
                    └──────┘ └──────┘
```

## Performance Characteristics

| Metric | Value |
|--------|-------|
| Leaf fanout | 406 entries |
| Internal fanout | 508 children |
| Height for 8M records | 2-3 levels |
| Point lookup I/O | 2-3 page reads |
| Range scan (K results) | log N + K/406 page reads |
| Insert (no split) | log N page reads + 1 write |
| Insert (with split) | log N reads + 3-5 writes |

## Common Interview Questions

1. **Why B+ tree over B-tree?**  
   B+ tree stores all data in leaves, enabling efficient range scans via leaf links. B-tree stores data in all nodes, making range scans require traversal of the tree structure.

2. **Why lazy deletion (no merge)?**  
   Merge/redistribution is complex, rarely triggers in practice, and the space is eventually reused. Many production systems (PostgreSQL, InnoDB) also use lazy deletion with periodic vacuuming.

3. **How do you handle duplicate keys?**  
   Multiple entries with the same key are stored in the same leaf (and overflow to the next leaf if needed). Lookup follows the leaf chain to find all duplicates.

4. **What's the I/O cost of a point lookup on 8M records?**  
   With fanout 406, height = ceil(log_406(8M)) = ceil(3.5) = 4 at most. But internal nodes are likely cached, so 1-2 actual disk reads in practice.

5. **How would you support variable-length keys (e.g., player names)?**  
   Options: (a) Use indirection — store key pointers in nodes, actual keys separately; (b) Use prefix compression; (c) Use a fixed-size hash of the key with collision handling.

6. **What about concurrency?**  
   The standard approach is latch crabbing: acquire parent latch, then child latch, release parent if child is safe (won't split). Or use optimistic approaches like B-link trees.

## Files

```
include/index/btree.h       - B+ tree, InternalNode, LeafNode declarations
src/index/btree.cpp         - Full implementation (~370 lines)
tests/index/btree_test.cpp  - 25 comprehensive tests
```

## Test Summary

| Category | Tests | Status |
|----------|-------|--------|
| Basic operations | 4 | ✓ |
| Duplicate keys | 1 | ✓ |
| Leaf splits | 2 | ✓ |
| Root/internal splits | 2 | ✓ |
| Range scans | 4 | ✓ |
| Deletion | 5 | ✓ |
| Node serialization | 3 | ✓ |
| Stress tests | 4 | ✓ |
| **Total** | **25** | **All pass** |
