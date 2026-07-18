/**
 * =============================================================================
 * CourtDB - Disk Manager Benchmarks
 * =============================================================================
 *
 * Measures:
 *   - Page allocation throughput
 *   - Sequential write throughput
 *   - Sequential read throughput
 *   - Random read latency
 *   - Free list allocation vs file extension
 *
 * =============================================================================
 */

#include "disk/disk_manager.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <random>
#include <vector>

using namespace courtdb;
using Clock = std::chrono::high_resolution_clock;

static constexpr const char* BENCH_FILE = "/tmp/courtdb_bench.db";

static double ElapsedMs(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

// =============================================================================
// Benchmarks
// =============================================================================

static void BenchSequentialWrite() {
    printf("\n=== Sequential Write ===\n");
    std::filesystem::remove(BENCH_FILE);

    DiskManager dm(BENCH_FILE);
    constexpr int NUM_PAGES = 10000;

    uint8_t buffer[PAGE_SIZE];
    std::memset(buffer, 0xAA, PAGE_SIZE);

    // Allocate all pages first
    std::vector<PageId> pages;
    pages.reserve(NUM_PAGES);
    for (int i = 0; i < NUM_PAGES; ++i) {
        pages.push_back(dm.AllocatePage());
    }

    // Time sequential writes
    auto start = Clock::now();
    for (auto pid : pages) {
        dm.WritePage(pid, buffer);
    }
    dm.Flush();
    auto end = Clock::now();

    double ms = ElapsedMs(start, end);
    double mb = (double)(NUM_PAGES * PAGE_SIZE) / (1024 * 1024);

    printf("  Pages written: %d\n", NUM_PAGES);
    printf("  Data: %.1f MB\n", mb);
    printf("  Time: %.2f ms\n", ms);
    printf("  Throughput: %.1f MB/s\n", mb / (ms / 1000.0));
    printf("  Avg per page: %.1f us\n", ms * 1000.0 / NUM_PAGES);
}

static void BenchSequentialRead() {
    printf("\n=== Sequential Read ===\n");
    // Reuses file from write benchmark

    DiskManager dm(BENCH_FILE);
    constexpr int NUM_PAGES = 10000;
    uint8_t buffer[PAGE_SIZE];

    auto start = Clock::now();
    for (uint32_t i = 1; i <= NUM_PAGES; ++i) {
        dm.ReadPage(i, buffer);
    }
    auto end = Clock::now();

    double ms = ElapsedMs(start, end);
    double mb = (double)(NUM_PAGES * PAGE_SIZE) / (1024 * 1024);

    printf("  Pages read: %d\n", NUM_PAGES);
    printf("  Data: %.1f MB\n", mb);
    printf("  Time: %.2f ms\n", ms);
    printf("  Throughput: %.1f MB/s\n", mb / (ms / 1000.0));
    printf("  Avg per page: %.1f us\n", ms * 1000.0 / NUM_PAGES);
}

static void BenchRandomRead() {
    printf("\n=== Random Read ===\n");

    DiskManager dm(BENCH_FILE);
    uint32_t page_count = dm.GetPageCount();
    uint8_t buffer[PAGE_SIZE];

    std::mt19937 rng(42);
    std::uniform_int_distribution<uint32_t> dist(1, page_count - 1);

    constexpr int NUM_READS = 10000;

    auto start = Clock::now();
    for (int i = 0; i < NUM_READS; ++i) {
        dm.ReadPage(dist(rng), buffer);
    }
    auto end = Clock::now();

    double ms = ElapsedMs(start, end);
    printf("  Random reads: %d\n", NUM_READS);
    printf("  Time: %.2f ms\n", ms);
    printf("  Avg latency: %.1f us/read\n", ms * 1000.0 / NUM_READS);
}

static void BenchAllocation() {
    printf("\n=== Page Allocation ===\n");
    std::filesystem::remove(BENCH_FILE);

    DiskManager dm(BENCH_FILE);
    constexpr int NUM_PAGES = 10000;

    auto start = Clock::now();
    for (int i = 0; i < NUM_PAGES; ++i) {
        dm.AllocatePage();
    }
    auto end = Clock::now();

    double ms = ElapsedMs(start, end);
    printf("  Pages allocated: %d\n", NUM_PAGES);
    printf("  Time: %.2f ms\n", ms);
    printf("  Avg per allocation: %.1f us\n", ms * 1000.0 / NUM_PAGES);
}

static void BenchFreeListReuse() {
    printf("\n=== Free List Reuse ===\n");
    std::filesystem::remove(BENCH_FILE);

    DiskManager dm(BENCH_FILE);
    constexpr int NUM_PAGES = 5000;

    // Allocate pages
    std::vector<PageId> pages;
    for (int i = 0; i < NUM_PAGES; ++i) {
        pages.push_back(dm.AllocatePage());
    }

    // Deallocate all
    for (auto pid : pages) {
        dm.DeallocatePage(pid);
    }

    // Reallocate from free list
    auto start = Clock::now();
    for (int i = 0; i < NUM_PAGES; ++i) {
        dm.AllocatePage();
    }
    auto end = Clock::now();

    double ms = ElapsedMs(start, end);
    printf("  Pages reallocated from free list: %d\n", NUM_PAGES);
    printf("  Time: %.2f ms\n", ms);
    printf("  Avg per allocation: %.1f us\n", ms * 1000.0 / NUM_PAGES);
}

// =============================================================================
// Main
// =============================================================================

int main() {
    printf("CourtDB Disk Manager Benchmarks\n");
    printf("================================\n");

    BenchSequentialWrite();
    BenchSequentialRead();
    BenchRandomRead();
    BenchAllocation();
    BenchFreeListReuse();

    // Cleanup
    std::filesystem::remove(BENCH_FILE);

    printf("\nDone.\n");
    return 0;
}
