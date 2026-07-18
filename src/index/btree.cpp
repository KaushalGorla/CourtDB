/**
 * =============================================================================
 * CourtDB - B+ Tree Index Implementation
 * =============================================================================
 *
 * Implements a disk-based B+ tree with the following characteristics:
 *   - All data in leaf nodes (linked for range scans)
 *   - Internal nodes contain separator keys + child pointers
 *   - Binary search within nodes for O(log fanout) per level
 *   - Split propagation upward (including root splits)
 *   - Lazy deletion (no merge/rebalance on delete)
 *   - Supports duplicate keys (multiple RIDs per key value)
 *
 * Node serialization format:
 *   Internal: [header][child_0][key_0][child_1][key_1]...[child_N]
 *   Leaf:     [header][key_0][rid_0][key_1][rid_1]...
 *
 * =============================================================================
 */

#include "index/btree.h"

#include <algorithm>
#include <cassert>
#include <cstring>

namespace courtdb {

// =============================================================================
// Internal Node Implementation
// =============================================================================

InternalNode::InternalNode(Page* page) : page_(page) {
    DeserializeFromPage();
}

PageId InternalNode::GetChildAt(uint16_t index) const {
    assert(index <= header_.num_keys);
    return children_[index];
}

void InternalNode::SetChildAt(uint16_t index, PageId child_id) {
    if (index >= children_.size()) {
        children_.resize(index + 1, INVALID_PAGE_ID);
    }
    children_[index] = child_id;
}

KeyType InternalNode::GetKeyAt(uint16_t index) const {
    assert(index < header_.num_keys);
    return keys_[index];
}

void InternalNode::SetKeyAt(uint16_t index, KeyType key) {
    if (index >= keys_.size()) {
        keys_.resize(index + 1, 0);
    }
    keys_[index] = key;
}

uint16_t InternalNode::FindChild(KeyType key, const KeyComparator& cmp) const {
    // Binary search for the child pointer
    // Find the first key > search key; the child is at that index
    uint16_t lo = 0, hi = header_.num_keys;
    while (lo < hi) {
        uint16_t mid = lo + (hi - lo) / 2;
        if (cmp(keys_[mid], key) <= 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;  // Child index (0-based)
}

void InternalNode::InsertAfter(uint16_t pos, KeyType key, PageId right_child) {
    assert(header_.num_keys < MAX_KEYS);

    // Insert key at position pos
    keys_.insert(keys_.begin() + pos, key);
    // Insert right_child at position pos + 1
    children_.insert(children_.begin() + pos + 1, right_child);
    header_.num_keys++;
}

void InternalNode::SerializeToPage() {
    uint8_t* data = page_->GetMutableData();

    // Skip the slotted page header (we use the raw page data area directly)
    // We'll use bytes starting after the standard page header (20 bytes)
    uint16_t offset = 20;  // After PageHeader

    // Write BTreeNodeHeader
    data[offset++] = static_cast<uint8_t>(header_.node_type);
    std::memcpy(data + offset, &header_.parent_page_id, 4); offset += 4;
    std::memcpy(data + offset, &header_.num_keys, 2); offset += 2;
    std::memcpy(data + offset, &header_.next_page_id, 4); offset += 4;

    // Write children and keys interleaved: child_0, key_0, child_1, key_1, ...
    for (uint16_t i = 0; i < header_.num_keys; ++i) {
        std::memcpy(data + offset, &children_[i], 4); offset += 4;
        std::memcpy(data + offset, &keys_[i], 4); offset += 4;
    }
    // Final child pointer
    if (header_.num_keys > 0) {
        std::memcpy(data + offset, &children_[header_.num_keys], 4); offset += 4;
    }
}

void InternalNode::DeserializeFromPage() {
    const uint8_t* data = page_->GetData();
    uint16_t offset = 20;  // After PageHeader

    // Read BTreeNodeHeader
    header_.node_type = static_cast<BTreeNodeType>(data[offset++]);
    std::memcpy(&header_.parent_page_id, data + offset, 4); offset += 4;
    std::memcpy(&header_.num_keys, data + offset, 2); offset += 2;
    std::memcpy(&header_.next_page_id, data + offset, 4); offset += 4;

    // Read children and keys
    keys_.resize(header_.num_keys);
    children_.resize(header_.num_keys + 1);

    for (uint16_t i = 0; i < header_.num_keys; ++i) {
        std::memcpy(&children_[i], data + offset, 4); offset += 4;
        std::memcpy(&keys_[i], data + offset, 4); offset += 4;
    }
    if (header_.num_keys > 0) {
        std::memcpy(&children_[header_.num_keys], data + offset, 4); offset += 4;
    }
}

// =============================================================================
// Leaf Node Implementation
// =============================================================================

LeafNode::LeafNode(Page* page) : page_(page) {
    DeserializeFromPage();
}

KeyType LeafNode::GetKeyAt(uint16_t index) const {
    assert(index < header_.num_keys);
    return keys_[index];
}

RID LeafNode::GetRIDAt(uint16_t index) const {
    assert(index < header_.num_keys);
    return rids_[index];
}

uint16_t LeafNode::FindKeyPosition(KeyType key, const KeyComparator& cmp) const {
    // Binary search for the first position where keys[pos] >= key
    uint16_t lo = 0, hi = header_.num_keys;
    while (lo < hi) {
        uint16_t mid = lo + (hi - lo) / 2;
        if (cmp(keys_[mid], key) < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

bool LeafNode::Insert(KeyType key, const RID& rid, const KeyComparator& cmp) {
    if (IsFull()) {
        return false;
    }

    // Find insertion position (maintain sorted order)
    uint16_t pos = FindKeyPosition(key, cmp);

    // Insert at position
    keys_.insert(keys_.begin() + pos, key);
    rids_.insert(rids_.begin() + pos, rid);
    header_.num_keys++;

    return true;
}

bool LeafNode::Delete(KeyType key, const RID& rid, const KeyComparator& cmp) {
    uint16_t pos = FindKeyPosition(key, cmp);

    // Scan forward through duplicates to find the exact (key, RID) match
    while (pos < header_.num_keys && cmp(keys_[pos], key) == 0) {
        if (rids_[pos] == rid) {
            keys_.erase(keys_.begin() + pos);
            rids_.erase(rids_.begin() + pos);
            header_.num_keys--;
            return true;
        }
        ++pos;
    }

    return false;
}

std::vector<RID> LeafNode::FindAll(KeyType key, const KeyComparator& cmp) const {
    std::vector<RID> results;
    uint16_t pos = FindKeyPosition(key, cmp);

    while (pos < header_.num_keys && cmp(keys_[pos], key) == 0) {
        results.push_back(rids_[pos]);
        ++pos;
    }

    return results;
}

void LeafNode::SerializeToPage() {
    uint8_t* data = page_->GetMutableData();
    uint16_t offset = 20;  // After PageHeader

    // Write BTreeNodeHeader
    data[offset++] = static_cast<uint8_t>(header_.node_type);
    std::memcpy(data + offset, &header_.parent_page_id, 4); offset += 4;
    std::memcpy(data + offset, &header_.num_keys, 2); offset += 2;
    std::memcpy(data + offset, &header_.next_page_id, 4); offset += 4;

    // Write key-RID pairs
    for (uint16_t i = 0; i < header_.num_keys; ++i) {
        std::memcpy(data + offset, &keys_[i], 4); offset += 4;
        std::memcpy(data + offset, &rids_[i].page_id, 4); offset += 4;
        std::memcpy(data + offset, &rids_[i].slot_id, 2); offset += 2;
    }
}

void LeafNode::DeserializeFromPage() {
    const uint8_t* data = page_->GetData();
    uint16_t offset = 20;  // After PageHeader

    // Read BTreeNodeHeader
    header_.node_type = static_cast<BTreeNodeType>(data[offset++]);
    std::memcpy(&header_.parent_page_id, data + offset, 4); offset += 4;
    std::memcpy(&header_.num_keys, data + offset, 2); offset += 2;
    std::memcpy(&header_.next_page_id, data + offset, 4); offset += 4;

    // Read key-RID pairs
    keys_.resize(header_.num_keys);
    rids_.resize(header_.num_keys);

    for (uint16_t i = 0; i < header_.num_keys; ++i) {
        std::memcpy(&keys_[i], data + offset, 4); offset += 4;
        std::memcpy(&rids_[i].page_id, data + offset, 4); offset += 4;
        std::memcpy(&rids_[i].slot_id, data + offset, 2); offset += 2;
    }
}

// =============================================================================
// B+ Tree Implementation
// =============================================================================

BPlusTree::BPlusTree(BufferPoolManager* bpm, PageId root_page_id, KeyComparator comparator)
    : bpm_(bpm), root_page_id_(root_page_id), comparator_(std::move(comparator)) {
    assert(bpm != nullptr);
}

// =============================================================================
// Insert
// =============================================================================

bool BPlusTree::Insert(KeyType key, const RID& rid) {
    // Case 1: Empty tree — create a new leaf root
    if (root_page_id_ == INVALID_PAGE_ID) {
        Page* root_page = bpm_->NewPage();
        if (root_page == nullptr) return false;

        root_page_id_ = root_page->GetPageId();

        // Initialize as leaf node
        LeafNode leaf(root_page);
        leaf.GetHeader().node_type = BTreeNodeType::kLeafNode;
        leaf.GetHeader().parent_page_id = INVALID_PAGE_ID;
        leaf.GetHeader().num_keys = 0;
        leaf.GetHeader().next_page_id = INVALID_PAGE_ID;
        leaf.SetKeys({});
        leaf.SetRIDs({});

        leaf.Insert(key, rid, comparator_);
        leaf.SerializeToPage();
        bpm_->UnpinPage(root_page_id_, true);
        return true;
    }

    // Case 2: Find the leaf page for this key
    Page* leaf_page = FindLeafPage(key);
    if (leaf_page == nullptr) return false;

    LeafNode leaf(leaf_page);

    // Try to insert directly
    if (leaf.Insert(key, rid, comparator_)) {
        leaf.SerializeToPage();
        bpm_->UnpinPage(leaf_page->GetPageId(), true);
        return true;
    }

    // Leaf is full — need to split
    // First, insert the new entry into a temporary overflow, then split
    auto all_keys = leaf.GetKeys();
    auto all_rids = leaf.GetRIDs();

    // Find position for new entry
    uint16_t pos = leaf.FindKeyPosition(key, comparator_);
    all_keys.insert(all_keys.begin() + pos, key);
    all_rids.insert(all_rids.begin() + pos, rid);

    // Split point: half the entries stay in left, half go to right
    uint16_t split_pos = static_cast<uint16_t>(all_keys.size() / 2);

    // Create new right leaf
    Page* right_page = bpm_->NewPage();
    if (right_page == nullptr) {
        bpm_->UnpinPage(leaf_page->GetPageId(), false);
        return false;
    }

    PageId right_page_id = right_page->GetPageId();

    // Initialize right leaf
    LeafNode right_leaf(right_page);
    right_leaf.GetHeader().node_type = BTreeNodeType::kLeafNode;
    right_leaf.GetHeader().parent_page_id = leaf.GetParentPageId();
    right_leaf.SetNextLeafId(leaf.GetNextLeafId());

    // Distribute keys
    std::vector<KeyType> right_keys(all_keys.begin() + split_pos, all_keys.end());
    std::vector<RID> right_rids(all_rids.begin() + split_pos, all_rids.end());
    right_leaf.SetKeys(std::move(right_keys));
    right_leaf.SetRIDs(std::move(right_rids));
    right_leaf.SetNumKeys(static_cast<uint16_t>(all_keys.size() - split_pos));

    // Left leaf keeps the first half
    std::vector<KeyType> left_keys(all_keys.begin(), all_keys.begin() + split_pos);
    std::vector<RID> left_rids(all_rids.begin(), all_rids.begin() + split_pos);
    leaf.SetKeys(std::move(left_keys));
    leaf.SetRIDs(std::move(left_rids));
    leaf.SetNumKeys(split_pos);
    leaf.SetNextLeafId(right_page_id);

    // The split key pushed to parent is the first key of the right leaf
    KeyType split_key = right_leaf.GetKeyAt(0);

    // Serialize both leaves
    leaf.SerializeToPage();
    right_leaf.SerializeToPage();

    // Insert split key into parent
    InsertIntoParent(leaf_page, split_key, right_page);

    bpm_->UnpinPage(leaf_page->GetPageId(), true);
    bpm_->UnpinPage(right_page_id, true);

    return true;
}

// =============================================================================
// Delete
// =============================================================================

bool BPlusTree::Delete(KeyType key, const RID& rid) {
    if (root_page_id_ == INVALID_PAGE_ID) {
        return false;
    }

    Page* leaf_page = FindLeafPage(key);
    if (leaf_page == nullptr) return false;

    LeafNode leaf(leaf_page);
    bool deleted = leaf.Delete(key, rid, comparator_);

    if (deleted) {
        leaf.SerializeToPage();
    }

    bpm_->UnpinPage(leaf_page->GetPageId(), deleted);
    return deleted;
}

// =============================================================================
// Lookup
// =============================================================================

std::vector<RID> BPlusTree::Lookup(KeyType key) {
    if (root_page_id_ == INVALID_PAGE_ID) {
        return {};
    }

    Page* leaf_page = FindLeafPage(key);
    if (leaf_page == nullptr) return {};

    LeafNode leaf(leaf_page);
    std::vector<RID> results = leaf.FindAll(key, comparator_);

    PageId next_leaf_id = leaf.GetNextLeafId();
    bpm_->UnpinPage(leaf_page->GetPageId(), false);

    // Continue scanning into next leaves if there are more duplicates
    while (next_leaf_id != INVALID_PAGE_ID && !results.empty()) {
        Page* next_page = bpm_->FetchPage(next_leaf_id);
        if (next_page == nullptr) break;

        LeafNode next_leaf(next_page);

        // Check if the first key in this leaf still matches
        if (next_leaf.GetNumKeys() == 0 ||
            comparator_(next_leaf.GetKeyAt(0), key) != 0) {
            bpm_->UnpinPage(next_leaf_id, false);
            break;
        }

        auto more = next_leaf.FindAll(key, comparator_);
        results.insert(results.end(), more.begin(), more.end());

        next_leaf_id = next_leaf.GetNextLeafId();
        bpm_->UnpinPage(next_page->GetPageId(), false);
    }

    return results;
}

// =============================================================================
// Range Scan
// =============================================================================

std::vector<std::pair<KeyType, RID>> BPlusTree::RangeScan(KeyType low, KeyType high) {
    if (root_page_id_ == INVALID_PAGE_ID) {
        return {};
    }

    std::vector<std::pair<KeyType, RID>> results;

    // Find the leaf containing 'low'
    Page* leaf_page = FindLeafPage(low);
    if (leaf_page == nullptr) return {};

    // Scan through leaf nodes
    while (leaf_page != nullptr) {
        LeafNode leaf(leaf_page);
        PageId current_page_id = leaf_page->GetPageId();

        uint16_t start_pos = leaf.FindKeyPosition(low, comparator_);

        for (uint16_t i = start_pos; i < leaf.GetNumKeys(); ++i) {
            KeyType k = leaf.GetKeyAt(i);
            if (comparator_(k, high) > 0) {
                // Past the upper bound — done
                bpm_->UnpinPage(current_page_id, false);
                return results;
            }
            results.emplace_back(k, leaf.GetRIDAt(i));
        }

        // Move to next leaf
        PageId next_id = leaf.GetNextLeafId();
        bpm_->UnpinPage(current_page_id, false);

        if (next_id == INVALID_PAGE_ID) {
            break;
        }
        leaf_page = bpm_->FetchPage(next_id);
    }

    return results;
}

// =============================================================================
// Private: FindLeafPage
// =============================================================================

Page* BPlusTree::FindLeafPage(KeyType key) {
    Page* page = bpm_->FetchPage(root_page_id_);
    if (page == nullptr) return nullptr;

    // Check node type by reading the type byte
    while (true) {
        const uint8_t* data = page->GetData();
        auto node_type = static_cast<BTreeNodeType>(data[20]);  // After PageHeader

        if (node_type == BTreeNodeType::kLeafNode) {
            return page;  // Found the leaf — return pinned
        }

        // Internal node — find the correct child
        InternalNode internal(page);
        uint16_t child_idx = internal.FindChild(key, comparator_);
        PageId child_page_id = internal.GetChildAt(child_idx);

        bpm_->UnpinPage(page->GetPageId(), false);

        page = bpm_->FetchPage(child_page_id);
        if (page == nullptr) return nullptr;
    }
}

// =============================================================================
// Private: InsertIntoParent
// =============================================================================

void BPlusTree::InsertIntoParent(Page* left_page, KeyType split_key, Page* right_page) {
    PageId left_id = left_page->GetPageId();
    PageId right_id = right_page->GetPageId();

    // Check if left is the root
    // Read parent from the node header
    const uint8_t* left_data = left_page->GetData();
    PageId parent_id;
    std::memcpy(&parent_id, left_data + 21, 4);  // offset 20 (type) + 1 = 21 for parent

    if (parent_id == INVALID_PAGE_ID) {
        // Left is the root — create a new root
        CreateNewRoot(left_id, split_key, right_id);
        return;
    }

    // Fetch the parent page
    Page* parent_page = bpm_->FetchPage(parent_id);
    if (parent_page == nullptr) return;

    InternalNode parent_node(parent_page);

    if (!parent_node.IsFull()) {
        // Parent has room — find position and insert
        uint16_t pos = parent_node.FindChild(split_key, comparator_);
        parent_node.InsertAfter(pos, split_key, right_id);
        parent_node.SerializeToPage();
        bpm_->UnpinPage(parent_id, true);
    } else {
        // Parent is full — need to split the parent too
        // Collect all keys and children including the new entry
        auto all_keys = parent_node.GetHeader().num_keys;
        std::vector<KeyType> keys;
        std::vector<PageId> children;

        for (uint16_t i = 0; i < all_keys; ++i) {
            keys.push_back(parent_node.GetKeyAt(i));
        }
        for (uint16_t i = 0; i <= all_keys; ++i) {
            children.push_back(parent_node.GetChildAt(i));
        }

        // Insert new key at correct position
        uint16_t pos = parent_node.FindChild(split_key, comparator_);
        keys.insert(keys.begin() + pos, split_key);
        children.insert(children.begin() + pos + 1, right_id);

        // Split: middle key goes up, left/right get their halves
        uint16_t mid = static_cast<uint16_t>(keys.size() / 2);
        KeyType push_up_key = keys[mid];

        // Left parent keeps [0, mid)
        parent_node.GetHeader().num_keys = mid;
        std::vector<KeyType> left_keys(keys.begin(), keys.begin() + mid);
        std::vector<PageId> left_children(children.begin(), children.begin() + mid + 1);

        // Create right parent with [mid+1, end)
        Page* new_parent_page = bpm_->NewPage();
        if (new_parent_page == nullptr) {
            bpm_->UnpinPage(parent_id, false);
            return;
        }

        PageId new_parent_id = new_parent_page->GetPageId();
        InternalNode new_parent(new_parent_page);
        new_parent.GetHeader().node_type = BTreeNodeType::kInternalNode;
        new_parent.GetHeader().parent_page_id = parent_node.GetParentPageId();
        new_parent.GetHeader().num_keys = static_cast<uint16_t>(keys.size() - mid - 1);

        // Set keys and children for new parent
        for (uint16_t i = mid + 1; i < keys.size(); ++i) {
            new_parent.SetKeyAt(i - mid - 1, keys[i]);
        }
        for (uint16_t i = mid + 1; i < children.size(); ++i) {
            new_parent.SetChildAt(i - mid - 1, children[i]);
        }

        // Update left parent
        for (uint16_t i = 0; i < mid; ++i) {
            parent_node.SetKeyAt(i, left_keys[i]);
        }
        for (uint16_t i = 0; i <= mid; ++i) {
            parent_node.SetChildAt(i, left_children[i]);
        }

        // Update children's parent pointers in the new parent
        for (uint16_t i = 0; i <= new_parent.GetNumKeys(); ++i) {
            PageId child_id = new_parent.GetChildAt(i);
            Page* child_page = bpm_->FetchPage(child_id);
            if (child_page != nullptr) {
                uint8_t* child_data = child_page->GetMutableData();
                std::memcpy(child_data + 21, &new_parent_id, 4);
                bpm_->UnpinPage(child_id, true);
            }
        }

        parent_node.SerializeToPage();
        new_parent.SerializeToPage();

        // Propagate split upward
        InsertIntoParent(parent_page, push_up_key, new_parent_page);

        bpm_->UnpinPage(parent_id, true);
        bpm_->UnpinPage(new_parent_id, true);
    }
}

// =============================================================================
// Private: CreateNewRoot
// =============================================================================

void BPlusTree::CreateNewRoot(PageId left_id, KeyType split_key, PageId right_id) {
    Page* root_page = bpm_->NewPage();
    if (root_page == nullptr) return;

    PageId new_root_id = root_page->GetPageId();

    // Initialize new root as internal node
    InternalNode root_node(root_page);
    root_node.GetHeader().node_type = BTreeNodeType::kInternalNode;
    root_node.GetHeader().parent_page_id = INVALID_PAGE_ID;
    root_node.GetHeader().num_keys = 1;
    root_node.GetHeader().next_page_id = INVALID_PAGE_ID;
    root_node.SetKeyAt(0, split_key);
    root_node.SetChildAt(0, left_id);
    root_node.SetChildAt(1, right_id);
    root_node.SerializeToPage();

    // Update children's parent pointers
    Page* left_page = bpm_->FetchPage(left_id);
    if (left_page != nullptr) {
        uint8_t* data = left_page->GetMutableData();
        std::memcpy(data + 21, &new_root_id, 4);  // parent_page_id
        bpm_->UnpinPage(left_id, true);
    }

    Page* right_page = bpm_->FetchPage(right_id);
    if (right_page != nullptr) {
        uint8_t* data = right_page->GetMutableData();
        std::memcpy(data + 21, &new_root_id, 4);  // parent_page_id
        bpm_->UnpinPage(right_id, true);
    }

    root_page_id_ = new_root_id;
    bpm_->UnpinPage(new_root_id, true);
}

}  // namespace courtdb
