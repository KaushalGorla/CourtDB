/**
 * =============================================================================
 * CourtDB - Query Executor Implementation
 * =============================================================================
 *
 * Implements the Volcano-style iterator model for query processing.
 * Each operator pulls one tuple at a time from its child, processes it,
 * and passes it up to the parent.
 *
 * Key implementation notes:
 *   - TableScan uses HeapFileIterator for sequential access
 *   - IndexScan materializes RIDs then fetches tuples lazily
 *   - Filter is a thin pass-through with predicate evaluation
 *   - Sort/Aggregation materialize input on Init() (blocking operators)
 *   - TopN uses std::partial_sort for O(N log K) performance
 *
 * =============================================================================
 */

#include "query/executor.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
#include <queue>

namespace courtdb {

// =============================================================================
// Table Scan Executor
// =============================================================================

TableScanExecutor::TableScanExecutor(HeapFile* heap_file, BufferPoolManager* bpm)
    : heap_file_(heap_file), bpm_(bpm) {}

void TableScanExecutor::Init() {
    iterator_ = heap_file_->Begin();
    initialized_ = true;
}

bool TableScanExecutor::Next(Tuple* output) {
    if (!initialized_) {
        Init();
    }

    while (iterator_.IsValid()) {
        auto [data, length] = iterator_.GetRecord();
        *output = RecordSerializer::Deserialize(data, length);
        iterator_.Next();
        return true;
    }

    return false;
}

// =============================================================================
// Index Scan Executor
// =============================================================================

IndexScanExecutor::IndexScanExecutor(BPlusTree* index, HeapFile* heap_file,
                                     BufferPoolManager* bpm, KeyType key)
    : index_(index), heap_file_(heap_file), bpm_(bpm),
      low_key_(key), high_key_(key), is_range_(false) {}

IndexScanExecutor::IndexScanExecutor(BPlusTree* index, HeapFile* heap_file,
                                     BufferPoolManager* bpm,
                                     KeyType low, KeyType high)
    : index_(index), heap_file_(heap_file), bpm_(bpm),
      low_key_(low), high_key_(high), is_range_(true) {}

void IndexScanExecutor::Init() {
    current_idx_ = 0;

    if (is_range_) {
        results_ = index_->RangeScan(low_key_, high_key_);
    } else {
        auto rids = index_->Lookup(low_key_);
        results_.clear();
        results_.reserve(rids.size());
        for (auto& rid : rids) {
            results_.emplace_back(low_key_, rid);
        }
    }
}

bool IndexScanExecutor::Next(Tuple* output) {
    while (current_idx_ < results_.size()) {
        const auto& [key, rid] = results_[current_idx_++];

        const uint8_t* data;
        uint16_t length;
        if (heap_file_->GetRecord(rid, &data, &length)) {
            *output = RecordSerializer::Deserialize(data, length);
            bpm_->UnpinPage(rid.page_id, false);
            return true;
        }
        // Record might have been deleted — skip
    }

    return false;
}

// =============================================================================
// Filter Executor
// =============================================================================

FilterExecutor::FilterExecutor(std::shared_ptr<Executor> child, Predicate predicate)
    : child_(std::move(child)), predicate_(std::move(predicate)) {}

void FilterExecutor::Init() {
    child_->Init();
}

bool FilterExecutor::Next(Tuple* output) {
    Tuple tuple;
    while (child_->Next(&tuple)) {
        if (predicate_(tuple)) {
            *output = std::move(tuple);
            return true;
        }
    }
    return false;
}

// =============================================================================
// Projection Executor
// =============================================================================

ProjectionExecutor::ProjectionExecutor(std::shared_ptr<Executor> child,
                                       ProjectionFunc projection)
    : child_(std::move(child)), projection_(std::move(projection)) {}

void ProjectionExecutor::Init() {
    child_->Init();
}

bool ProjectionExecutor::Next(Tuple* output) {
    Tuple tuple;
    if (child_->Next(&tuple)) {
        *output = projection_(tuple);
        return true;
    }
    return false;
}

// =============================================================================
// Sort Executor
// =============================================================================

SortExecutor::SortExecutor(std::shared_ptr<Executor> child, TupleComparator comparator)
    : child_(std::move(child)), comparator_(std::move(comparator)) {}

void SortExecutor::Init() {
    sorted_tuples_.clear();
    current_idx_ = 0;

    // Materialize all input
    child_->Init();
    Tuple tuple;
    while (child_->Next(&tuple)) {
        sorted_tuples_.push_back(std::move(tuple));
    }

    // Sort
    std::sort(sorted_tuples_.begin(), sorted_tuples_.end(), comparator_);
}

bool SortExecutor::Next(Tuple* output) {
    if (current_idx_ < sorted_tuples_.size()) {
        *output = sorted_tuples_[current_idx_++];
        return true;
    }
    return false;
}

// =============================================================================
// Limit Executor
// =============================================================================

LimitExecutor::LimitExecutor(std::shared_ptr<Executor> child, uint32_t limit)
    : child_(std::move(child)), limit_(limit) {}

void LimitExecutor::Init() {
    child_->Init();
    emitted_ = 0;
}

bool LimitExecutor::Next(Tuple* output) {
    if (emitted_ >= limit_) {
        return false;
    }

    Tuple tuple;
    if (child_->Next(&tuple)) {
        *output = std::move(tuple);
        emitted_++;
        return true;
    }
    return false;
}

// =============================================================================
// Aggregation Executor
// =============================================================================

AggregationExecutor::AggregationExecutor(std::shared_ptr<Executor> child,
                                         GroupKeyExtractor group_key_extractor,
                                         std::vector<AggSpec> agg_specs)
    : child_(std::move(child)),
      group_key_extractor_(std::move(group_key_extractor)),
      agg_specs_(std::move(agg_specs)) {}

void AggregationExecutor::Init() {
    results_.clear();
    current_idx_ = 0;

    size_t num_aggs = agg_specs_.size();

    // Hash map: group_key → accumulator
    std::unordered_map<std::string, GroupAccumulator> groups;

    // Materialize and aggregate all input
    child_->Init();
    Tuple tuple;
    while (child_->Next(&tuple)) {
        std::string group_key = group_key_extractor_(tuple);

        auto& acc = groups[group_key];
        if (acc.sums.empty()) {
            acc.sums.resize(num_aggs, 0.0);
            acc.mins.resize(num_aggs, std::numeric_limits<double>::max());
            acc.maxs.resize(num_aggs, std::numeric_limits<double>::lowest());
        }

        acc.count++;

        for (size_t i = 0; i < num_aggs; ++i) {
            double val = agg_specs_[i].value_extractor(tuple);
            acc.sums[i] += val;
            acc.mins[i] = std::min(acc.mins[i], val);
            acc.maxs[i] = std::max(acc.maxs[i], val);
        }
    }

    // Compute final results
    results_.reserve(groups.size());
    for (auto& [key, acc] : groups) {
        AggResult result;
        result.group_key = key;
        result.values.resize(num_aggs);

        for (size_t i = 0; i < num_aggs; ++i) {
            switch (agg_specs_[i].type) {
                case AggType::kCount:
                    result.values[i] = static_cast<double>(acc.count);
                    break;
                case AggType::kSum:
                    result.values[i] = acc.sums[i];
                    break;
                case AggType::kAvg:
                    result.values[i] = acc.count > 0 ? acc.sums[i] / acc.count : 0.0;
                    break;
                case AggType::kMin:
                    result.values[i] = acc.mins[i];
                    break;
                case AggType::kMax:
                    result.values[i] = acc.maxs[i];
                    break;
            }
        }

        results_.push_back(std::move(result));
    }
}

bool AggregationExecutor::Next(Tuple* output) {
    // The aggregation executor's Next() emits one "result tuple" per group.
    // Since our Tuple type is NBARecord, we encode aggregation results
    // into the description field as a string for simplicity.
    // In a real system, you'd have a separate result schema.
    if (current_idx_ < results_.size()) {
        const auto& result = results_[current_idx_++];

        // Encode result into a Tuple
        output->game_id = "";
        output->team = result.group_key;
        output->description = "";

        // Pack aggregate values into numeric fields for downstream use
        if (!result.values.empty()) {
            output->home_score = static_cast<uint16_t>(result.values[0]);
        }
        if (result.values.size() > 1) {
            output->away_score = static_cast<uint16_t>(result.values[1]);
        }
        if (result.values.size() > 2) {
            output->shot_distance = static_cast<uint16_t>(result.values[2]);
        }

        // Store count in season field for convenience
        output->season = static_cast<uint16_t>(results_[current_idx_ - 1].values.empty()
                         ? 0 : results_[current_idx_ - 1].values[0]);

        return true;
    }
    return false;
}

// =============================================================================
// Top-N Executor
// =============================================================================

TopNExecutor::TopNExecutor(std::shared_ptr<Executor> child,
                           TupleComparator comparator, uint32_t limit)
    : child_(std::move(child)), comparator_(std::move(comparator)), limit_(limit) {}

void TopNExecutor::Init() {
    top_tuples_.clear();
    current_idx_ = 0;

    // Materialize all input
    child_->Init();
    Tuple tuple;
    std::vector<Tuple> all_tuples;
    while (child_->Next(&tuple)) {
        all_tuples.push_back(std::move(tuple));
    }

    // Partial sort to find top-K
    uint32_t k = std::min(limit_, static_cast<uint32_t>(all_tuples.size()));
    if (k > 0) {
        std::partial_sort(all_tuples.begin(),
                          all_tuples.begin() + k,
                          all_tuples.end(),
                          comparator_);
        top_tuples_.assign(all_tuples.begin(), all_tuples.begin() + k);
    }
}

bool TopNExecutor::Next(Tuple* output) {
    if (current_idx_ < top_tuples_.size()) {
        *output = top_tuples_[current_idx_++];
        return true;
    }
    return false;
}

}  // namespace courtdb
