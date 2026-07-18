#pragma once
/**
 * =============================================================================
 * CourtDB - Recovery Manager
 * =============================================================================
 *
 * Purpose:
 *   Performs crash recovery using the Write-Ahead Log. After a crash, the
 *   recovery manager replays logged operations to bring the database to a
 *   consistent state.
 *
 * Responsibilities:
 *   - Redo all operations from the log after a crash
 *   - Coordinate checkpoints (snapshot of dirty page state)
 *   - Determine recovery start point using checkpoint records
 *   - Apply physical redo (re-insert/delete/update records on pages)
 *
 * Recovery Algorithm (ARIES-simplified, redo-only):
 *   1. ANALYSIS: Scan log from last checkpoint to determine dirty pages
 *   2. REDO: Replay all operations on pages whose LSN < log record LSN
 *
 * Time Complexity:
 *   - Recovery: O(L) where L = log records since last checkpoint
 *   - Checkpoint: O(D) where D = dirty pages (flush cost)
 *
 * Important Invariants:
 *   - Recovery is idempotent (can be run multiple times safely)
 *   - After recovery, the database is in the state of the last committed write
 *   - Checkpoints reduce recovery time by limiting redo scope
 *
 * Design Decisions:
 *   - Redo-only (no undo): simpler, appropriate for single-user embedded DB
 *   - Physical redo: apply exact byte changes to pages
 *   - Checkpoint = flush all dirty pages + write checkpoint record
 *   - No transaction concept (all writes are implicitly committed)
 *
 * =============================================================================
 */

#include "buffer/buffer_pool_manager.h"
#include "recovery/log_manager.h"

#include <cstdint>
#include <unordered_set>

namespace courtdb {

// =============================================================================
// Recovery Manager
// =============================================================================

class RecoveryManager {
public:
    /**
     * Create a recovery manager.
     * @param log_manager The WAL manager
     * @param bpm The buffer pool manager
     * @param disk_manager The disk manager (for direct page access during recovery)
     */
    RecoveryManager(LogManager* log_manager,
                    BufferPoolManager* bpm,
                    DiskManager* disk_manager);

    // =========================================================================
    // Recovery
    // =========================================================================

    /**
     * Perform crash recovery by replaying the WAL.
     * Should be called on database startup.
     *
     * @return Number of log records replayed
     */
    uint32_t Recover();

    // =========================================================================
    // Logging Operations (called by higher-level components)
    // =========================================================================

    /**
     * Log an INSERT operation before modifying the page.
     * @return The LSN of the log record
     */
    LSN LogInsert(PageId page_id, uint16_t slot_id,
                  const uint8_t* data, uint16_t length);

    /**
     * Log a DELETE operation before modifying the page.
     */
    LSN LogDelete(PageId page_id, uint16_t slot_id,
                  const uint8_t* old_data, uint16_t old_length);

    /**
     * Log an UPDATE operation before modifying the page.
     */
    LSN LogUpdate(PageId page_id, uint16_t slot_id,
                  const uint8_t* old_data, uint16_t old_length,
                  const uint8_t* new_data, uint16_t new_length);

    /**
     * Log a NEW PAGE allocation.
     */
    LSN LogNewPage(PageId page_id);

    // =========================================================================
    // Checkpointing
    // =========================================================================

    /**
     * Perform a checkpoint:
     *   1. Write CHECKPOINT_BEGIN record
     *   2. Flush all dirty pages to disk
     *   3. Write CHECKPOINT_END record with dirty page table
     *   4. Flush the log
     *
     * After a checkpoint, recovery only needs to replay records
     * after the checkpoint.
     */
    void Checkpoint();

    // =========================================================================
    // Accessors
    // =========================================================================

    [[nodiscard]] uint32_t GetRedoCount() const { return redo_count_; }

private:
    // =========================================================================
    // Internal Helpers
    // =========================================================================

    /**
     * Redo a single log record by applying its changes to the affected page.
     * Only applies if the page's LSN is less than the log record's LSN
     * (i.e., the change was not already applied before the crash).
     */
    void RedoRecord(const LogRecord& record);

    // =========================================================================
    // Data Members
    // =========================================================================

    LogManager* log_manager_;
    BufferPoolManager* bpm_;
    DiskManager* disk_manager_;
    uint32_t redo_count_ = 0;  ///< Records replayed during last recovery
};

}  // namespace courtdb
