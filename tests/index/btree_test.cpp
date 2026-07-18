/**
 * =============================================================================
 * CourtDB - B+ Tree Index Unit Tests
 * =============================================================================
 *
 * Comprehensive tests covering:
 *   - Internal/Leaf node operations
 *   - Single key insert and lookup
 *   - Multiple inserts with ordering
 *   - Leaf node splits
 *   - Internal node splits
 *   - Root splits (tree height growth)
 *   - Duplicate key handling
 *   - Range scans
 *   - Deletion
 *   - Stress tests with thousands of keys
 *   - Randomized insert/lookup verification
 *
 * =============================================================================
 */

#include "index/btree.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <memory>
#include <numeric>
#include <random>
#include <set>
#include <vector>

namespace courtdb {
namespace {

// =============================================================================
// Test Fixture
// =============================================================================

class BPlusTreeTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_file_ = "/tmp/courtdb_btree_test_" +
                     std::to_string(reinterpret_cast<uintptr_t>(this)) + ".db";
        std::filesystem::remove(test_file_);
        disk_manager_ = std::make_unique<DiskManager>(test_file_);
        bpm_ = std::make_unique<BufferPoolManager>(100, disk_manager_.get());
    }

    void TearDown() override {
        tree_.reset();
        bpm_.reset();
        disk_manager_.reset();
        std::filesystem::remove(test_file_);
    }

    void CreateTree() {
        tree_ = std::make_unique<BPlusTree>(bpm_.get());
    }

    std::string test_file_;
    std::unique_ptr<DiskManager> disk_manager_;
    std::unique_ptr<BufferPoolManager> bpm_;
    std::unique_ptr<BPlusTree> tree_;
};

// =============================================================================
// Basic Operations
// =============================================================================

TEST_F(BPlusTreeTest, EmptyTree) {
    CreateTree();
    EXPECT_TRUE(tree_->IsEmpty());
    EXPECT_EQ(tree_->GetRootPageId(), INVALID_PAGE_ID);

    auto results = tree_->Lookup(42);
    EXPECT_TRUE(results.empty());
}

TEST_F(BPlusTreeTest, InsertSingleKey) {
    CreateTree();

    RID rid(1, 0);
    EXPECT_TRUE(tree_->Insert(100, rid));
    EXPECT_FALSE(tree_->IsEmpty());

    auto results = tree_->Lookup(100);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0], rid);
}

TEST_F(BPlusTreeTest, InsertMultipleKeys) {
    CreateTree();

    for (uint32_t i = 0; i < 10; ++i) {
        EXPECT_TRUE(tree_->Insert(i * 10, RID(i, 0)));
    }

    // Verify all keys can be looked up
    for (uint32_t i = 0; i < 10; ++i) {
        auto results = tree_->Lookup(i * 10);
        ASSERT_EQ(results.size(), 1u) << "Key " << i * 10 << " not found";
        EXPECT_EQ(results[0].page_id, i);
    }

    // Non-existent key
    auto missing = tree_->Lookup(15);
    EXPECT_TRUE(missing.empty());
}

TEST_F(BPlusTreeTest, InsertReverseOrder) {
    CreateTree();

    for (int i = 99; i >= 0; --i) {
        EXPECT_TRUE(tree_->Insert(static_cast<uint32_t>(i), RID(static_cast<uint32_t>(i), 0)));
    }

    // Verify in forward order
    for (uint32_t i = 0; i < 100; ++i) {
        auto results = tree_->Lookup(i);
        ASSERT_EQ(results.size(), 1u) << "Key " << i << " not found";
        EXPECT_EQ(results[0].page_id, i);
    }
}

// =============================================================================
// Duplicate Keys
// =============================================================================

TEST_F(BPlusTreeTest, DuplicateKeys) {
    CreateTree();

    // Insert same key with different RIDs
    for (uint32_t i = 0; i < 5; ++i) {
        EXPECT_TRUE(tree_->Insert(42, RID(i, static_cast<uint16_t>(i))));
    }

    auto results = tree_->Lookup(42);
    ASSERT_EQ(results.size(), 5u);

    // All RIDs should be present (order may vary within same key)
    std::set<PageId> found_pages;
    for (const auto& rid : results) {
        found_pages.insert(rid.page_id);
    }
    for (uint32_t i = 0; i < 5; ++i) {
        EXPECT_TRUE(found_pages.count(i) > 0) << "RID with page_id " << i << " missing";
    }
}

// =============================================================================
// Leaf Splits
// =============================================================================

TEST_F(BPlusTreeTest, LeafSplit) {
    CreateTree();

    // Insert enough keys to trigger at least one leaf split
    // LeafNode::MAX_KEYS = 406, so inserting 500 keys will split
    for (uint32_t i = 0; i < 500; ++i) {
        EXPECT_TRUE(tree_->Insert(i, RID(i, 0))) << "Failed at key " << i;
    }

    // Verify all keys are still findable
    for (uint32_t i = 0; i < 500; ++i) {
        auto results = tree_->Lookup(i);
        ASSERT_EQ(results.size(), 1u) << "Key " << i << " not found after split";
        EXPECT_EQ(results[0].page_id, i);
    }
}

TEST_F(BPlusTreeTest, MultipleLeafSplits) {
    CreateTree();

    // Insert enough for multiple splits (3+ leaves)
    constexpr uint32_t NUM_KEYS = 1500;
    for (uint32_t i = 0; i < NUM_KEYS; ++i) {
        EXPECT_TRUE(tree_->Insert(i, RID(i, 0))) << "Failed at key " << i;
    }

    // Verify all keys
    for (uint32_t i = 0; i < NUM_KEYS; ++i) {
        auto results = tree_->Lookup(i);
        ASSERT_EQ(results.size(), 1u) << "Key " << i << " not found";
    }
}

// =============================================================================
// Internal Node Splits / Root Splits
// =============================================================================

TEST_F(BPlusTreeTest, RootSplit) {
    CreateTree();

    // First leaf split creates a new root (height goes from 1 to 2)
    for (uint32_t i = 0; i < 500; ++i) {
        tree_->Insert(i, RID(i, 0));
    }

    // The root should now be an internal node
    Page* root = bpm_->FetchPage(tree_->GetRootPageId());
    ASSERT_NE(root, nullptr);
    auto type = static_cast<BTreeNodeType>(root->GetData()[20]);
    EXPECT_EQ(type, BTreeNodeType::kInternalNode);
    bpm_->UnpinPage(tree_->GetRootPageId(), false);
}

TEST_F(BPlusTreeTest, LargeTreeMultipleLevels) {
    CreateTree();

    // Insert enough keys to create a tree with at least 3 levels
    // With 406 keys/leaf and 507 keys/internal, need 406*507 = ~200K for level 3
    // Let's use 5000 keys which will create 2-3 levels
    constexpr uint32_t NUM_KEYS = 5000;
    for (uint32_t i = 0; i < NUM_KEYS; ++i) {
        EXPECT_TRUE(tree_->Insert(i, RID(i, static_cast<uint16_t>(i % 100))))
            << "Failed at key " << i;
    }

    // Verify random subset
    std::mt19937 rng(42);
    std::uniform_int_distribution<uint32_t> dist(0, NUM_KEYS - 1);
    for (int i = 0; i < 500; ++i) {
        uint32_t key = dist(rng);
        auto results = tree_->Lookup(key);
        ASSERT_EQ(results.size(), 1u) << "Key " << key << " not found";
        EXPECT_EQ(results[0].page_id, key);
    }
}

// =============================================================================
// Range Scans
// =============================================================================

TEST_F(BPlusTreeTest, RangeScanBasic) {
    CreateTree();

    for (uint32_t i = 0; i < 100; ++i) {
        tree_->Insert(i * 2, RID(i, 0));  // Even numbers 0..198
    }

    // Range [10, 30] should return 10, 12, 14, ..., 30 (11 results)
    auto results = tree_->RangeScan(10, 30);
    EXPECT_EQ(results.size(), 11u);

    // Verify ordering
    for (size_t i = 0; i < results.size(); ++i) {
        EXPECT_EQ(results[i].first, static_cast<uint32_t>(10 + i * 2));
    }
}

TEST_F(BPlusTreeTest, RangeScanAcrossLeaves) {
    CreateTree();

    // Insert enough to span multiple leaves
    for (uint32_t i = 0; i < 1000; ++i) {
        tree_->Insert(i, RID(i, 0));
    }

    // Scan a range that spans leaf boundaries
    auto results = tree_->RangeScan(200, 400);
    EXPECT_EQ(results.size(), 201u);

    // Verify ordering
    for (size_t i = 0; i < results.size(); ++i) {
        EXPECT_EQ(results[i].first, static_cast<uint32_t>(200 + i));
    }
}

TEST_F(BPlusTreeTest, RangeScanEmpty) {
    CreateTree();

    for (uint32_t i = 0; i < 100; ++i) {
        tree_->Insert(i, RID(i, 0));
    }

    // Range with no matching keys
    auto results = tree_->RangeScan(200, 300);
    EXPECT_TRUE(results.empty());
}

TEST_F(BPlusTreeTest, RangeScanSingleKey) {
    CreateTree();

    for (uint32_t i = 0; i < 50; ++i) {
        tree_->Insert(i * 10, RID(i, 0));
    }

    auto results = tree_->RangeScan(30, 30);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].first, 30u);
}

// =============================================================================
// Deletion
// =============================================================================

TEST_F(BPlusTreeTest, DeleteSingleKey) {
    CreateTree();

    tree_->Insert(50, RID(5, 0));
    EXPECT_EQ(tree_->Lookup(50).size(), 1u);

    EXPECT_TRUE(tree_->Delete(50, RID(5, 0)));
    EXPECT_TRUE(tree_->Lookup(50).empty());
}

TEST_F(BPlusTreeTest, DeleteNonExistentKey) {
    CreateTree();

    tree_->Insert(10, RID(1, 0));
    EXPECT_FALSE(tree_->Delete(99, RID(1, 0)));  // Key doesn't exist
    EXPECT_FALSE(tree_->Delete(10, RID(2, 0)));  // Key exists but RID doesn't match
}

TEST_F(BPlusTreeTest, DeleteFromEmptyTree) {
    CreateTree();
    EXPECT_FALSE(tree_->Delete(42, RID(1, 0)));
}

TEST_F(BPlusTreeTest, DeleteDuplicateKey) {
    CreateTree();

    tree_->Insert(100, RID(1, 0));
    tree_->Insert(100, RID(2, 0));
    tree_->Insert(100, RID(3, 0));

    EXPECT_EQ(tree_->Lookup(100).size(), 3u);

    EXPECT_TRUE(tree_->Delete(100, RID(2, 0)));
    EXPECT_EQ(tree_->Lookup(100).size(), 2u);

    EXPECT_TRUE(tree_->Delete(100, RID(1, 0)));
    EXPECT_EQ(tree_->Lookup(100).size(), 1u);

    auto remaining = tree_->Lookup(100);
    ASSERT_EQ(remaining.size(), 1u);
    EXPECT_EQ(remaining[0], RID(3, 0));
}

TEST_F(BPlusTreeTest, DeleteMany) {
    CreateTree();

    for (uint32_t i = 0; i < 200; ++i) {
        tree_->Insert(i, RID(i, 0));
    }

    // Delete every other key
    for (uint32_t i = 0; i < 200; i += 2) {
        EXPECT_TRUE(tree_->Delete(i, RID(i, 0))) << "Failed to delete key " << i;
    }

    // Verify deletions
    for (uint32_t i = 0; i < 200; ++i) {
        auto results = tree_->Lookup(i);
        if (i % 2 == 0) {
            EXPECT_TRUE(results.empty()) << "Key " << i << " should be deleted";
        } else {
            ASSERT_EQ(results.size(), 1u) << "Key " << i << " should exist";
            EXPECT_EQ(results[0].page_id, i);
        }
    }
}

// =============================================================================
// Stress Tests
// =============================================================================

TEST_F(BPlusTreeTest, StressSequentialInsert) {
    CreateTree();

    constexpr uint32_t NUM_KEYS = 10000;
    for (uint32_t i = 0; i < NUM_KEYS; ++i) {
        ASSERT_TRUE(tree_->Insert(i, RID(i, static_cast<uint16_t>(i % 1000))))
            << "Failed at key " << i;
    }

    // Verify a random sample
    std::mt19937 rng(123);
    std::uniform_int_distribution<uint32_t> dist(0, NUM_KEYS - 1);
    for (int i = 0; i < 1000; ++i) {
        uint32_t key = dist(rng);
        auto results = tree_->Lookup(key);
        ASSERT_EQ(results.size(), 1u) << "Key " << key << " not found";
        EXPECT_EQ(results[0].page_id, key);
    }
}

TEST_F(BPlusTreeTest, StressRandomInsert) {
    CreateTree();

    constexpr uint32_t NUM_KEYS = 5000;
    std::vector<uint32_t> keys(NUM_KEYS);
    std::iota(keys.begin(), keys.end(), 0);

    // Shuffle for random insertion order
    std::mt19937 rng(42);
    std::shuffle(keys.begin(), keys.end(), rng);

    for (uint32_t key : keys) {
        ASSERT_TRUE(tree_->Insert(key, RID(key, 0))) << "Failed at key " << key;
    }

    // Verify all keys exist
    for (uint32_t i = 0; i < NUM_KEYS; ++i) {
        auto results = tree_->Lookup(i);
        ASSERT_EQ(results.size(), 1u) << "Key " << i << " not found";
        EXPECT_EQ(results[0].page_id, i);
    }

    // Range scan should return sorted results
    auto range = tree_->RangeScan(1000, 2000);
    EXPECT_EQ(range.size(), 1001u);
    for (size_t i = 0; i < range.size(); ++i) {
        EXPECT_EQ(range[i].first, static_cast<uint32_t>(1000 + i));
    }
}

TEST_F(BPlusTreeTest, StressInsertDeleteCycles) {
    CreateTree();

    std::mt19937 rng(99);
    std::set<uint32_t> live_keys;

    for (int iteration = 0; iteration < 3000; ++iteration) {
        if (live_keys.empty() || rng() % 3 != 0) {
            // Insert a random key
            uint32_t key = rng() % 10000;
            tree_->Insert(key, RID(key, 0));
            live_keys.insert(key);
        } else {
            // Delete a random live key
            auto it = live_keys.begin();
            std::advance(it, rng() % live_keys.size());
            uint32_t key = *it;
            tree_->Delete(key, RID(key, 0));
            live_keys.erase(it);
        }
    }

    // Verify all live keys exist
    for (uint32_t key : live_keys) {
        auto results = tree_->Lookup(key);
        EXPECT_GE(results.size(), 1u) << "Live key " << key << " not found";
    }
}

TEST_F(BPlusTreeTest, StressRangeScanLarge) {
    CreateTree();

    // Insert 10000 sequential keys
    constexpr uint32_t NUM_KEYS = 10000;
    for (uint32_t i = 0; i < NUM_KEYS; ++i) {
        tree_->Insert(i, RID(i, 0));
    }

    // Full range scan
    auto all = tree_->RangeScan(0, NUM_KEYS - 1);
    EXPECT_EQ(all.size(), static_cast<size_t>(NUM_KEYS));

    // Verify sorted order
    for (size_t i = 1; i < all.size(); ++i) {
        EXPECT_LE(all[i - 1].first, all[i].first);
    }
}

// =============================================================================
// Node Serialization
// =============================================================================

TEST_F(BPlusTreeTest, LeafNodeSerializationRoundTrip) {
    // Create a leaf node, serialize, deserialize, verify
    Page* page = bpm_->NewPage();
    ASSERT_NE(page, nullptr);

    {
        LeafNode leaf(page);
        leaf.GetHeader().node_type = BTreeNodeType::kLeafNode;
        leaf.GetHeader().parent_page_id = 42;
        leaf.GetHeader().num_keys = 0;
        leaf.SetNextLeafId(99);

        leaf.Insert(10, RID(1, 0), DefaultKeyComparator);
        leaf.Insert(20, RID(2, 1), DefaultKeyComparator);
        leaf.Insert(30, RID(3, 2), DefaultKeyComparator);

        leaf.SerializeToPage();
    }

    // Deserialize from the same page
    {
        LeafNode leaf2(page);
        EXPECT_EQ(leaf2.GetNumKeys(), 3);
        EXPECT_EQ(leaf2.GetParentPageId(), 42u);
        EXPECT_EQ(leaf2.GetNextLeafId(), 99u);

        EXPECT_EQ(leaf2.GetKeyAt(0), 10u);
        EXPECT_EQ(leaf2.GetKeyAt(1), 20u);
        EXPECT_EQ(leaf2.GetKeyAt(2), 30u);

        EXPECT_EQ(leaf2.GetRIDAt(0), RID(1, 0));
        EXPECT_EQ(leaf2.GetRIDAt(1), RID(2, 1));
        EXPECT_EQ(leaf2.GetRIDAt(2), RID(3, 2));
    }

    bpm_->UnpinPage(page->GetPageId(), false);
}

TEST_F(BPlusTreeTest, InternalNodeSerializationRoundTrip) {
    Page* page = bpm_->NewPage();
    ASSERT_NE(page, nullptr);

    {
        // Manually set header for internal node
        uint8_t* data = page->GetMutableData();
        data[20] = static_cast<uint8_t>(BTreeNodeType::kInternalNode);
        uint16_t zero = 0;
        PageId inv = INVALID_PAGE_ID;
        std::memcpy(data + 21, &inv, 4);  // parent
        std::memcpy(data + 25, &zero, 2); // num_keys
        std::memcpy(data + 27, &inv, 4);  // next

        InternalNode internal(page);
        internal.GetHeader().node_type = BTreeNodeType::kInternalNode;
        internal.GetHeader().parent_page_id = 7;
        internal.GetHeader().num_keys = 3;

        internal.SetChildAt(0, 10);
        internal.SetKeyAt(0, 100);
        internal.SetChildAt(1, 20);
        internal.SetKeyAt(1, 200);
        internal.SetChildAt(2, 30);
        internal.SetKeyAt(2, 300);
        internal.SetChildAt(3, 40);

        internal.SerializeToPage();
    }

    {
        InternalNode internal2(page);
        EXPECT_EQ(internal2.GetNumKeys(), 3);
        EXPECT_EQ(internal2.GetParentPageId(), 7u);

        EXPECT_EQ(internal2.GetChildAt(0), 10u);
        EXPECT_EQ(internal2.GetKeyAt(0), 100u);
        EXPECT_EQ(internal2.GetChildAt(1), 20u);
        EXPECT_EQ(internal2.GetKeyAt(1), 200u);
        EXPECT_EQ(internal2.GetChildAt(2), 30u);
        EXPECT_EQ(internal2.GetKeyAt(2), 300u);
        EXPECT_EQ(internal2.GetChildAt(3), 40u);
    }

    bpm_->UnpinPage(page->GetPageId(), false);
}

TEST_F(BPlusTreeTest, BinarySearchInLeaf) {
    Page* page = bpm_->NewPage();
    ASSERT_NE(page, nullptr);

    LeafNode leaf(page);
    leaf.GetHeader().node_type = BTreeNodeType::kLeafNode;
    leaf.GetHeader().num_keys = 0;

    // Insert keys in order
    for (uint32_t i = 0; i < 50; ++i) {
        leaf.Insert(i * 10, RID(i, 0), DefaultKeyComparator);
    }

    // Binary search should find exact positions
    EXPECT_EQ(leaf.FindKeyPosition(0, DefaultKeyComparator), 0);
    EXPECT_EQ(leaf.FindKeyPosition(10, DefaultKeyComparator), 1);
    EXPECT_EQ(leaf.FindKeyPosition(490, DefaultKeyComparator), 49);

    // Non-existent keys should return insertion position
    EXPECT_EQ(leaf.FindKeyPosition(5, DefaultKeyComparator), 1);    // Between 0 and 10
    EXPECT_EQ(leaf.FindKeyPosition(15, DefaultKeyComparator), 2);   // Between 10 and 20
    EXPECT_EQ(leaf.FindKeyPosition(500, DefaultKeyComparator), 50); // After all

    bpm_->UnpinPage(page->GetPageId(), false);
}

}  // namespace
}  // namespace courtdb
