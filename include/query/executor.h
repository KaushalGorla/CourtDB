#pragma once
/**
 * =============================================================================
 * CourtDB - Query Executor (Volcano/Iterator Model)
 * =============================================================================
 *
 * Purpose:
 *   Implements a pull-based query execution engine using the Volcano iterator
 *   model. Each operator produces one tuple at a time via Next(), enabling
 *   pipelined execution without materializing intermediate results.
 *
 * Responsibilities:
 *   - Table scan (sequential scan over heap file)
 *   - Index scan (B+ tree point/range lookup)
 *   - Filter (predicate evaluation)
 *   - Projection (column selection)
 *   - Aggregation (GROUP BY with COUNT, SUM, AVG)
 *   - Sorting (external sort for ORDER BY)
 *   - Limit (stop after N tuples)
 *
 * Time Complexity:
 *   - TableScan: O(N) where N = total records
 *   - IndexScan: O(log N + K) where K = matching records
 *   - Filter: O(input) — one pass
 *   - Projection: O(1) per tuple
 *   - Aggregation: O(input) + O(G) where G = groups
 *   - Sort: O(N log N)
 *   - Limit: O(min(N, limit))
 *
 * Important Invariants:
 *   - Operators are composed into a tree (plan)
 *   - Each call to Next() advances exactly one tuple
 *   - Init() must be called before the first Next()
 *   - After Next() returns false, no more tuples are available
 *   - Operators do not own their children (shared_ptr for composition)
 *
 * Design Decisions:
 *   - Volcano model for simplicity and composability
 *   - Tuples are deserialized NBARecords (not raw bytes) for query processing
 *   - Aggregation materializes all groups (hash-based)
 *   - Sort materializes all input (in-memory for now)
 *   - Predicates are std::function for flexibility
 *
 * =============================================================================
 */

#include "buffer/buffer_pool_manager.h"
#include "index/btree.h"
#include "storage/heap_file.h"
#include "storage/record.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace courtdb {

// =============================================================================
// Tuple Type
// =============================================================================

/// A Tuple in CourtDB is a deserialized NBARecord.
/// This is the unit of data flowing between operators.
using Tuple = NBARecord;

// =============================================================================
// Predicate Types
// =============================================================================

/// A predicate function evaluates a tuple and returns true/false
using Predicate = std::function<bool(const Tuple&)>;

/// A key extractor pulls a uint32_t index key from a tuple
using KeyExtractor = std::function<KeyType(const Tuple&)>;

/// A group key extractor pulls a string group key from a tuple
using GroupKeyExtractor = std::function<std::string(const Tuple&)>;

/// A comparator for sorting tuples
using TupleComparator = std::function<bool(const Tuple&, const Tuple&)>;

// =============================================================================
// Aggregation Types
// =============================================================================

enum class AggType {
    kCount,
    kSum,
    kAvg,
    kMin,
    kMax,
};

/// Specifies an aggregation: what to aggregate and how
struct AggSpec {
    AggType type;
    /// Extracts the numeric value to aggregate from a tuple
    std::function<double(const Tuple&)> value_extractor;
    /// Name for the result (e.g., "avg_points")
    std::string result_name;
};

/// Result of a GROUP BY aggregation for one group
struct AggResult {
    std::string group_key;
    std::vector<double> values;  // One per AggSpec
};

// =============================================================================
// Base Executor Interface
// =============================================================================

/**
 * Abstract base class for all query operators.
 * Follows the Volcano iterator model: Init() → Next() → Next() → ... → false
 */
class Executor {
public:
    virtual ~Executor() = default;

    /// Initialize the operator (open files, prepare state)
    virtual void Init() = 0;

    /// Get the next tuple. Returns true if a tuple is available, false if done.
    virtual bool Next(Tuple* output) = 0;

    /// Reset the operator to scan from the beginning
    virtual void Reset() { Init(); }
};

// =============================================================================
// Table Scan Executor
// =============================================================================

/**
 * Scans all records in a heap file sequentially.
 * Deserializes each record into a Tuple for downstream processing.
 */
class TableScanExecutor : public Executor {
public:
    TableScanExecutor(HeapFile* heap_file, BufferPoolManager* bpm);

    void Init() override;
    bool Next(Tuple* output) override;

private:
    HeapFile* heap_file_;
    BufferPoolManager* bpm_;
    HeapFileIterator iterator_;
    bool initialized_ = false;
};

// =============================================================================
// Index Scan Executor
// =============================================================================

/**
 * Scans records using a B+ tree index for point or range lookups.
 * Much faster than table scan when selectivity is low.
 */
class IndexScanExecutor : public Executor {
public:
    /// Point lookup: find all tuples matching a single key
    IndexScanExecutor(BPlusTree* index, HeapFile* heap_file,
                      BufferPoolManager* bpm, KeyType key);

    /// Range lookup: find all tuples with key in [low, high]
    IndexScanExecutor(BPlusTree* index, HeapFile* heap_file,
                      BufferPoolManager* bpm, KeyType low, KeyType high);

    void Init() override;
    bool Next(Tuple* output) override;

private:
    BPlusTree* index_;
    HeapFile* heap_file_;
    BufferPoolManager* bpm_;
    KeyType low_key_;
    KeyType high_key_;
    bool is_range_;
    std::vector<std::pair<KeyType, RID>> results_;
    size_t current_idx_ = 0;
};

// =============================================================================
// Filter Executor
// =============================================================================

/**
 * Filters tuples from a child executor based on a predicate.
 * Only passes through tuples where predicate(tuple) == true.
 * Implements predicate pushdown when composed with other operators.
 */
class FilterExecutor : public Executor {
public:
    FilterExecutor(std::shared_ptr<Executor> child, Predicate predicate);

    void Init() override;
    bool Next(Tuple* output) override;

private:
    std::shared_ptr<Executor> child_;
    Predicate predicate_;
};

// =============================================================================
// Projection Executor
// =============================================================================

/**
 * Applies a transformation to each tuple (select specific fields,
 * compute derived values, rename fields).
 *
 * For CourtDB's fixed schema, projection is a tuple→tuple transform.
 */
class ProjectionExecutor : public Executor {
public:
    using ProjectionFunc = std::function<Tuple(const Tuple&)>;

    ProjectionExecutor(std::shared_ptr<Executor> child, ProjectionFunc projection);

    void Init() override;
    bool Next(Tuple* output) override;

private:
    std::shared_ptr<Executor> child_;
    ProjectionFunc projection_;
};

// =============================================================================
// Sort Executor
// =============================================================================

/**
 * Sorts all input tuples using the provided comparator.
 * Materializes the entire input on Init(), then emits tuples in sorted order.
 * Implements ORDER BY semantics.
 */
class SortExecutor : public Executor {
public:
    SortExecutor(std::shared_ptr<Executor> child, TupleComparator comparator);

    void Init() override;
    bool Next(Tuple* output) override;

private:
    std::shared_ptr<Executor> child_;
    TupleComparator comparator_;
    std::vector<Tuple> sorted_tuples_;
    size_t current_idx_ = 0;
};

// =============================================================================
// Limit Executor
// =============================================================================

/**
 * Stops emitting tuples after a specified count.
 * Implements LIMIT semantics.
 */
class LimitExecutor : public Executor {
public:
    LimitExecutor(std::shared_ptr<Executor> child, uint32_t limit);

    void Init() override;
    bool Next(Tuple* output) override;

private:
    std::shared_ptr<Executor> child_;
    uint32_t limit_;
    uint32_t emitted_ = 0;
};

// =============================================================================
// Aggregation Executor
// =============================================================================

/**
 * Performs GROUP BY aggregation with support for multiple aggregate functions.
 * Materializes all input, groups by key, computes aggregates, then emits results.
 *
 * Supports: COUNT, SUM, AVG, MIN, MAX
 *
 * If no group_key_extractor is provided, aggregates over all input (single group).
 */
class AggregationExecutor : public Executor {
public:
    AggregationExecutor(std::shared_ptr<Executor> child,
                        GroupKeyExtractor group_key_extractor,
                        std::vector<AggSpec> agg_specs);

    void Init() override;
    bool Next(Tuple* output) override;

    /// Access computed results directly (for programmatic use)
    [[nodiscard]] const std::vector<AggResult>& GetResults() const { return results_; }

private:
    std::shared_ptr<Executor> child_;
    GroupKeyExtractor group_key_extractor_;
    std::vector<AggSpec> agg_specs_;
    std::vector<AggResult> results_;
    size_t current_idx_ = 0;

    /// Internal accumulator per group
    struct GroupAccumulator {
        uint64_t count = 0;
        std::vector<double> sums;
        std::vector<double> mins;
        std::vector<double> maxs;
    };
};

// =============================================================================
// Top-N Executor (ORDER BY + LIMIT optimization)
// =============================================================================

/**
 * Optimized executor for ORDER BY ... LIMIT N queries.
 * Uses a partial sort (heap) to find top-N without fully sorting all input.
 * Time: O(N log K) where N = input size, K = limit.
 */
class TopNExecutor : public Executor {
public:
    TopNExecutor(std::shared_ptr<Executor> child,
                 TupleComparator comparator, uint32_t limit);

    void Init() override;
    bool Next(Tuple* output) override;

private:
    std::shared_ptr<Executor> child_;
    TupleComparator comparator_;
    uint32_t limit_;
    std::vector<Tuple> top_tuples_;
    size_t current_idx_ = 0;
};

}  // namespace courtdb
