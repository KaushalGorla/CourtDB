#pragma once
/**
 * =============================================================================
 * CourtDB - B+ Tree Index
 * =============================================================================
 *
 * Purpose:
 *   Provides ordered index access over heap file records. Supports point
 *   lookups, range scans, insertions, and deletions. Keys are variable-length
 *   byte sequences with a comparator; values are RIDs pointing to heap records.
 *
 * Responsibilities:
 *   - Maintain a balanced B+ tree over a set of (key, RID) pairs
 *   - Support duplicate keys (multiple RIDs per key value)
 *   - Provide O(log N) point lookup
 *   - Provide O(log N + K) range scans (K = result count)
 *   - Handle leaf/internal node splits and parent propagation
 *   - Handle root splits (tree height growth)
 *   - Store nodes in pages managed by the buffer pool
 *
 * Time Complexity:
 *   - Insert: O(log N) amortized (split cascades are rare)
 *   - Lookup: O(log N)
 *   - Range scan: O(log N + K) where K = number of results
 *   - Delete: O(log N) (lazy delete, no merge/redistribution)
 *
 * Important Invariants:
 *   - All data (key, RID pairs) lives in leaf nodes
 *   - Internal nodes contain separator keys and child page pointers
 *   - Leaf nodes are linked via next_leaf_id for efficient range scans
 *   - Keys within a node are always sorted (binary search enabled)
 *   - The tree is always balanced (all leaves at same depth)
 *   - A node is at least half-full after a split
 *
 * Design Decisions:
 *   - Keys are stored as fixed-size integers (uint32_t) for the NBA use case
 *     (season, score, distance). String keys would require a different comparator.
 *   - Leaf nodes store (key, RID) pairs; internal nodes store (key, child_page_id)
 *   - Duplicate keys: all duplicates go to the same leaf or overflow to siblings
 *   - Lazy deletion: marks entries as deleted without rebalancing
 *   - Page-level storage: each node fits in exactly one page
 *
 * =============================================================================
 */

#include "buffer/buffer_pool_manager.h"
#include "storage/record.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace courtdb {

// =============================================================================
// B+ Tree Configuration
// =============================================================================

/// Key type for the index. Using uint32_t for numeric NBA fields.
using KeyType = uint32_t;

/// Comparison function type
using KeyComparator = std::function<int(KeyType, KeyType)>;

/// Default comparator for unsigned integers
inline int DefaultKeyComparator(KeyType a, KeyType b) {
    if (a < b) return -1;
    if (a > b) return 1;
    return 0;
}

// =============================================================================
// Node Types
// =============================================================================

enum class BTreeNodeType : uint8_t {
    kInternalNode = 1,
    kLeafNode = 2,
};

// =============================================================================
// Node Header (serialized at the start of each B+ tree page)
// =============================================================================

/**
 * Common header for both internal and leaf nodes.
 * Stored at the beginning of the page data (after the slotted page header).
 *
 * Layout in raw page bytes (starting at offset 0 of the node's data area):
 *   [1B: node_type] [4B: parent_page_id] [2B: num_keys] [4B: next_page_id]
 *
 * Total: 11 bytes
 */
struct BTreeNodeHeader {
    BTreeNodeType node_type = BTreeNodeType::kLeafNode;
    PageId parent_page_id = INVALID_PAGE_ID;
    uint16_t num_keys = 0;
    PageId next_page_id = INVALID_PAGE_ID;  // For leaf: next sibling; unused for internal
};

static constexpr uint16_t BTREE_NODE_HEADER_SIZE = 11;

// =============================================================================
// Internal Node
// =============================================================================

/**
 * Internal (non-leaf) node in the B+ tree.
 *
 * Layout after header:
 *   [4B: child_0] [4B: key_0] [4B: child_1] [4B: key_1] ... [4B: child_N]
 *
 * With N keys, there are N+1 child pointers.
 * key[i] is the separator between child[i] and child[i+1].
 * All keys in child[i] are < key[i] <= all keys in child[i+1].
 *
 * Max keys per internal node:
 *   Available space = PAGE_SIZE - PageHeader(20) - BTreeNodeHeader(11) = 4065
 *   Each key-child pair = 8 bytes, plus one extra child pointer = 4 bytes
 *   Max keys = (4065 - 4) / 8 = 507
 */
class InternalNode {
public:
    /// Maximum keys in an internal node
    static constexpr uint16_t MAX_KEYS = 507;

    explicit InternalNode(Page* page);

    // Accessors
    [[nodiscard]] BTreeNodeHeader& GetHeader() { return header_; }
    [[nodiscard]] const BTreeNodeHeader& GetHeader() const { return header_; }
    [[nodiscard]] uint16_t GetNumKeys() const { return header_.num_keys; }
    [[nodiscard]] PageId GetParentPageId() const { return header_.parent_page_id; }
    void SetParentPageId(PageId pid) { header_.parent_page_id = pid; }

    /// Get the child page ID at the given index (0 to num_keys)
    [[nodiscard]] PageId GetChildAt(uint16_t index) const;

    /// Set the child page ID at the given index
    void SetChildAt(uint16_t index, PageId child_id);

    /// Get the key at the given index (0 to num_keys-1)
    [[nodiscard]] KeyType GetKeyAt(uint16_t index) const;

    /// Set the key at the given index
    void SetKeyAt(uint16_t index, KeyType key);

    /**
     * Find the child index for a given key using binary search.
     * Returns the index of the child that should contain this key.
     */
    [[nodiscard]] uint16_t FindChild(KeyType key, const KeyComparator& cmp) const;

    /**
     * Insert a new (key, right_child) pair after the given position.
     * Used during child splits.
     */
    void InsertAfter(uint16_t pos, KeyType key, PageId right_child);

    /// Serialize the node state into the underlying page buffer
    void SerializeToPage();

    /// Deserialize node state from the underlying page buffer
    void DeserializeFromPage();

    [[nodiscard]] bool IsFull() const { return header_.num_keys >= MAX_KEYS; }

private:
    Page* page_;
    BTreeNodeHeader header_;
    std::vector<PageId> children_;   // Size: num_keys + 1
    std::vector<KeyType> keys_;      // Size: num_keys
};

// =============================================================================
// Leaf Node
// =============================================================================

/**
 * Leaf node in the B+ tree. Stores actual (key, RID) data pairs.
 *
 * Layout after header:
 *   [4B: key_0] [4B: page_id_0] [2B: slot_id_0]
 *   [4B: key_1] [4B: page_id_1] [2B: slot_id_1]
 *   ...
 *
 * Each entry = 10 bytes (4B key + 6B RID)
 * Available space = PAGE_SIZE - PageHeader(20) - BTreeNodeHeader(11) = 4065
 * Max entries = 4065 / 10 = 406
 *
 * Leaf nodes are linked via next_page_id for range scans.
 */
class LeafNode {
public:
    /// Maximum entries in a leaf node
    static constexpr uint16_t MAX_KEYS = 406;

    explicit LeafNode(Page* page);

    // Accessors
    [[nodiscard]] BTreeNodeHeader& GetHeader() { return header_; }
    [[nodiscard]] const BTreeNodeHeader& GetHeader() const { return header_; }
    [[nodiscard]] uint16_t GetNumKeys() const { return header_.num_keys; }
    [[nodiscard]] PageId GetNextLeafId() const { return header_.next_page_id; }
    void SetNextLeafId(PageId pid) { header_.next_page_id = pid; }
    [[nodiscard]] PageId GetParentPageId() const { return header_.parent_page_id; }
    void SetParentPageId(PageId pid) { header_.parent_page_id = pid; }

    /// Get the key at the given index
    [[nodiscard]] KeyType GetKeyAt(uint16_t index) const;

    /// Get the RID at the given index
    [[nodiscard]] RID GetRIDAt(uint16_t index) const;

    /**
     * Find the position for a key using binary search.
     * Returns the index of the first key >= the search key.
     */
    [[nodiscard]] uint16_t FindKeyPosition(KeyType key, const KeyComparator& cmp) const;

    /**
     * Insert a (key, RID) pair at the correct sorted position.
     * @return true if inserted, false if node is full
     */
    bool Insert(KeyType key, const RID& rid, const KeyComparator& cmp);

    /**
     * Delete the entry matching (key, RID).
     * @return true if found and deleted, false otherwise
     */
    bool Delete(KeyType key, const RID& rid, const KeyComparator& cmp);

    /**
     * Find all RIDs matching the given key.
     */
    [[nodiscard]] std::vector<RID> FindAll(KeyType key, const KeyComparator& cmp) const;

    /// Serialize the node state into the underlying page buffer
    void SerializeToPage();

    /// Deserialize node state from the underlying page buffer
    void DeserializeFromPage();

    [[nodiscard]] bool IsFull() const { return header_.num_keys >= MAX_KEYS; }

    /// Access the underlying key/RID arrays (used during splits)
    [[nodiscard]] const std::vector<KeyType>& GetKeys() const { return keys_; }
    [[nodiscard]] const std::vector<RID>& GetRIDs() const { return rids_; }
    void SetKeys(std::vector<KeyType> keys) { keys_ = std::move(keys); }
    void SetRIDs(std::vector<RID> rids) { rids_ = std::move(rids); }
    void SetNumKeys(uint16_t n) { header_.num_keys = n; }

private:
    Page* page_;
    BTreeNodeHeader header_;
    std::vector<KeyType> keys_;   // Size: num_keys
    std::vector<RID> rids_;       // Size: num_keys
};

// =============================================================================
// B+ Tree Index
// =============================================================================

/**
 * The complete B+ tree index structure. Manages a tree of internal and leaf
 * nodes stored in buffer pool pages, supporting insert, delete, point lookup,
 * and range scan operations.
 */
class BPlusTree {
public:
    /**
     * Create or open a B+ tree index.
     * @param bpm Buffer pool manager
     * @param root_page_id Existing root page (INVALID_PAGE_ID to create new)
     * @param comparator Key comparison function
     */
    BPlusTree(BufferPoolManager* bpm,
              PageId root_page_id = INVALID_PAGE_ID,
              KeyComparator comparator = DefaultKeyComparator);

    // =========================================================================
    // Core Operations
    // =========================================================================

    /**
     * Insert a (key, RID) pair into the index.
     * Handles leaf splits, internal splits, and root splits as needed.
     * @return true on success, false on failure (e.g., out of pages)
     */
    bool Insert(KeyType key, const RID& rid);

    /**
     * Delete a (key, RID) pair from the index.
     * Uses lazy deletion (no merge/redistribute).
     * @return true if the entry was found and deleted
     */
    bool Delete(KeyType key, const RID& rid);

    /**
     * Find all RIDs matching the given key (point lookup).
     * Handles duplicate keys by scanning the leaf chain.
     */
    [[nodiscard]] std::vector<RID> Lookup(KeyType key);

    /**
     * Range scan: find all RIDs with keys in [low, high].
     * Follows the leaf chain for efficient sequential access.
     */
    [[nodiscard]] std::vector<std::pair<KeyType, RID>> RangeScan(KeyType low, KeyType high);

    // =========================================================================
    // Accessors
    // =========================================================================

    [[nodiscard]] PageId GetRootPageId() const { return root_page_id_; }
    [[nodiscard]] bool IsEmpty() const { return root_page_id_ == INVALID_PAGE_ID; }

private:
    // =========================================================================
    // Internal Helpers
    // =========================================================================

    /**
     * Find the leaf page that should contain the given key.
     * Traverses from root to leaf following separator keys.
     * Returns the leaf page (pinned). Caller must unpin.
     */
    Page* FindLeafPage(KeyType key);

    /**
     * Split a full leaf node. Creates a new right sibling, moves half the
     * entries to it, and propagates the split key up to the parent.
     */
    void SplitLeaf(Page* leaf_page, LeafNode& leaf_node);

    /**
     * Split a full internal node. Creates a new right sibling, moves half the
     * keys/children to it, and propagates the middle key up to the parent.
     */
    void SplitInternal(Page* internal_page, InternalNode& internal_node);

    /**
     * Insert a separator key into the parent of a split node.
     * If the parent is full, recursively splits the parent.
     * If the root splits, creates a new root.
     */
    void InsertIntoParent(Page* left_page, KeyType split_key, Page* right_page);

    /**
     * Create a new root node containing the given left/right children
     * and separator key. Called when the current root splits.
     */
    void CreateNewRoot(PageId left_id, KeyType split_key, PageId right_id);

    // =========================================================================
    // Data Members
    // =========================================================================

    BufferPoolManager* bpm_;
    PageId root_page_id_;
    KeyComparator comparator_;
};

}  // namespace courtdb
