/**
 * =============================================================================
 * CourtDB - Page Layout Benchmarks
 * =============================================================================
 *
 * Measures:
 *   - Insert throughput (records/second)
 *   - Access latency (ns/record)
 *   - Compaction time for fragmented pages
 *   - Serialization/deserialization round-trip time
 *
 * =============================================================================
 */

#include "storage/page.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

using namespace courtdb;
using Clock = std::chrono::high_resolution_clock;

// =============================================================================
// Helpers
// =============================================================================

static double ElapsedMs(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

static double ElapsedNs(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::nano>(end - start).count();
}

// =============================================================================
// Benchmarks
// =============================================================================

static void BenchInsertThroughput() {
    printf("\n=== Insert Throughput ===\n");

    constexpr int NUM_PAGES = 1000;
    constexpr uint16_t RECORD_SIZE = 64;
    uint8_t record[RECORD_SIZE];
    std::memset(record, 0xAB, RECORD_SIZE);

    int total_records = 0;
    auto start = Clock::now();

    for (int p = 0; p < NUM_PAGES; ++p) {
        Page page(p);
        while (page.InsertRecord(record, RECORD_SIZE).has_value()) {
            ++total_records;
        }
    }

    auto end = Clock::now();
    double ms = ElapsedMs(start, end);

    printf("  Pages: %d\n", NUM_PAGES);
    printf("  Records inserted: %d\n", total_records);
    printf("  Time: %.2f ms\n", ms);
    printf("  Throughput: %.0f records/ms (%.0f M records/sec)\n",
           total_records / ms, total_records / ms / 1000.0);
    printf("  Avg per record: %.1f ns\n", ms * 1e6 / total_records);
}

static void BenchAccessLatency() {
    printf("\n=== Access Latency ===\n");

    Page page(0);
    std::vector<uint16_t> slots;

    uint8_t record[50];
    std::memset(record, 0xCD, sizeof(record));
    while (auto s = page.InsertRecord(record, sizeof(record))) {
        slots.push_back(s.value());
    }

    int num_accesses = 100000;
    std::mt19937 rng(42);
    std::uniform_int_distribution<size_t> dist(0, slots.size() - 1);

    auto start = Clock::now();
    volatile const uint8_t* sink = nullptr;
    for (int i = 0; i < num_accesses; ++i) {
        auto result = page.GetRecord(slots[dist(rng)]);
        sink = result.value().first;
    }
    auto end = Clock::now();
    (void)sink;

    printf("  Records in page: %zu\n", slots.size());
    printf("  Random accesses: %d\n", num_accesses);
    printf("  Total time: %.2f ms\n", ElapsedMs(start, end));
    printf("  Avg latency: %.1f ns/access\n", ElapsedNs(start, end) / num_accesses);
}

static void BenchCompaction() {
    printf("\n=== Compaction ===\n");

    Page page(0);
    std::vector<uint16_t> slots;

    uint8_t record[80];
    std::memset(record, 0xEF, sizeof(record));
    while (auto s = page.InsertRecord(record, sizeof(record))) {
        slots.push_back(s.value());
    }

    // Delete every other record to create maximum fragmentation
    for (size_t i = 0; i < slots.size(); i += 2) {
        page.DeleteRecord(slots[i]);
    }

    int num_compactions = 10000;
    auto start = Clock::now();
    for (int i = 0; i < num_compactions; ++i) {
        page.Compact();
    }
    auto end = Clock::now();

    printf("  Records before delete: %zu\n", slots.size());
    printf("  Records after delete: %d\n", page.GetNumRecords());
    printf("  Compactions: %d\n", num_compactions);
    printf("  Total time: %.2f ms\n", ElapsedMs(start, end));
    printf("  Avg compaction: %.1f ns\n", ElapsedNs(start, end) / num_compactions);
}

static void BenchSerialization() {
    printf("\n=== Serialization Round-Trip ===\n");

    Page page(0);
    uint8_t record[60];
    std::memset(record, 0x11, sizeof(record));
    while (page.InsertRecord(record, sizeof(record)).has_value()) {}

    int num_iterations = 100000;
    auto start = Clock::now();
    for (int i = 0; i < num_iterations; ++i) {
        page.SerializeToBuffer();
        page.DeserializeFromBuffer();
    }
    auto end = Clock::now();

    printf("  Iterations: %d\n", num_iterations);
    printf("  Total time: %.2f ms\n", ElapsedMs(start, end));
    printf("  Avg round-trip: %.1f ns\n", ElapsedNs(start, end) / num_iterations);
}

// =============================================================================
// Main
// =============================================================================

int main() {
    printf("CourtDB Page Layout Benchmarks\n");
    printf("==============================\n");

    BenchInsertThroughput();
    BenchAccessLatency();
    BenchCompaction();
    BenchSerialization();

    printf("\nDone.\n");
    return 0;
}
