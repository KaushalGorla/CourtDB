/**
 * =============================================================================
 * CourtDB - Heap File Implementation
 * =============================================================================
 *
 * The heap file organizes table data as a linked list of pages.
 * Records are inserted into the first page with sufficient space.
 * A free-space directory tracks which pages have room, avoiding
 * full-chain scans on every insert.
 *
 * Page chain structure:
 *   [Page A] → [Page B] → [Page C] → ... → [Page Z (last)]
 *        ↓
 *   Each page's next_page_id links to the successor.
 *   first_page_id_ points to head, last_page_id_ points to tail.
 *
 * =============================================================================
 */

#include "storage/heap_file.h"

#include <algorithm>
#include <cassert>

namespace courtdb {

// =============================================================================
// HeapFile Construction
// =============================================================================

HeapFile::HeapFile(BufferPoolManager* bpm, PageId first_page_id)
    : bpm_(bpm),
      first_page_id_(first_page_id),
      last_page_id_(first_page_id) {

    assert(bpm != nullptr);

    if (first_page_id_ == INVALID_PAGE_ID) {
        // Create a brand new heap file with one initial page
        first_page_id_ = AllocateNewPage();
        last_page_id_ = first_page_id_;
    } else {
        // Existing heap file — walk the chain to find the last page and count records
        PageId current = first_page_id_;
        while (current != INVALID_PAGE_ID) {
            Page* page = bpm_->FetchPage(current);
            if (page == nullptr) break;

            page_count_++;
            record_count_ += page->GetNumRecords();

            if (page->HasSpaceFor(64)) {  // Heuristic: at least 64 bytes free
                pages_with_space_.push_back(current);
            }

            PageId next = page->GetNextPageId();
            if (next == INVALID_PAGE_ID) {
                last_page_id_ = current;
            }

            bpm_->UnpinPage(current, false);
            current = next;
        }
    }
}

// =============================================================================
// Record Operations
// =============================================================================

RID HeapFile::InsertRecord(const uint8_t* data, uint16_t length) {
    if (data == nullptr || length == 0) {
        return RID{};
    }

    // Find a page with sufficient space
    PageId target_page_id = FindPageWithSpace(length);

    // Fetch the target page
    Page* page = bpm_->FetchPage(target_page_id);
    if (page == nullptr) {
        return RID{};
    }

    // Insert the record
    auto slot = page->InsertRecord(data, length);
    if (!slot.has_value()) {
        // Page was full despite our tracking — allocate a new page
        bpm_->UnpinPage(target_page_id, false);

        target_page_id = AllocateNewPage();
        page = bpm_->FetchPage(target_page_id);
        if (page == nullptr) {
            return RID{};
        }
        slot = page->InsertRecord(data, length);
        if (!slot.has_value()) {
            bpm_->UnpinPage(target_page_id, true);
            return RID{};  // Record too large for a single page
        }
    }

    // Update free space tracking
    if (!page->HasSpaceFor(64)) {
        // Remove from free space directory
        auto it = std::find(pages_with_space_.begin(), pages_with_space_.end(), target_page_id);
        if (it != pages_with_space_.end()) {
            pages_with_space_.erase(it);
        }
    }

    record_count_++;
    bpm_->UnpinPage(target_page_id, true);

    return RID{target_page_id, slot.value()};
}

bool HeapFile::DeleteRecord(const RID& rid) {
    if (!rid.IsValid()) {
        return false;
    }

    Page* page = bpm_->FetchPage(rid.page_id);
    if (page == nullptr) {
        return false;
    }

    bool deleted = page->DeleteRecord(rid.slot_id);
    if (deleted) {
        record_count_--;

        // Page now has more space — add to free space directory if not already there
        auto it = std::find(pages_with_space_.begin(), pages_with_space_.end(), rid.page_id);
        if (it == pages_with_space_.end()) {
            pages_with_space_.push_back(rid.page_id);
        }
    }

    bpm_->UnpinPage(rid.page_id, deleted);
    return deleted;
}

bool HeapFile::GetRecord(const RID& rid, const uint8_t** out_data, uint16_t* out_length) {
    if (!rid.IsValid()) {
        return false;
    }

    Page* page = bpm_->FetchPage(rid.page_id);
    if (page == nullptr) {
        return false;
    }

    auto result = page->GetRecord(rid.slot_id);
    if (!result.has_value()) {
        bpm_->UnpinPage(rid.page_id, false);
        return false;
    }

    *out_data = result.value().first;
    *out_length = result.value().second;

    // NOTE: Caller is responsible for calling UnpinPage after reading the data.
    // The page remains pinned until the caller is done with the pointer.
    // This is an intentional design choice to avoid copying record data.
    return true;
}

RID HeapFile::UpdateRecord(const RID& rid, const uint8_t* data, uint16_t length) {
    if (!rid.IsValid()) {
        return RID{};
    }

    Page* page = bpm_->FetchPage(rid.page_id);
    if (page == nullptr) {
        return RID{};
    }

    // Try in-place update
    if (page->UpdateRecord(rid.slot_id, data, length)) {
        bpm_->UnpinPage(rid.page_id, true);
        return rid;  // Same RID, updated in place
    }

    // In-place update failed (new record is larger and doesn't fit)
    // Delete the old record and insert at a new location
    page->DeleteRecord(rid.slot_id);
    record_count_--;
    bpm_->UnpinPage(rid.page_id, true);

    // Insert the new version
    return InsertRecord(data, length);
}

// =============================================================================
// Iteration
// =============================================================================

HeapFileIterator HeapFile::Begin() {
    return HeapFileIterator(bpm_, first_page_id_);
}

// =============================================================================
// Bulk Operations
// =============================================================================

std::vector<RID> HeapFile::BulkInsert(
    const std::vector<std::pair<const uint8_t*, uint16_t>>& records) {

    std::vector<RID> rids;
    rids.reserve(records.size());

    // Try to batch inserts into the current page to minimize pin/unpin
    PageId current_page_id = FindPageWithSpace(64);  // Start with a page that has space
    Page* current_page = bpm_->FetchPage(current_page_id);

    if (current_page == nullptr) {
        // Fallback to single inserts
        for (const auto& [data, length] : records) {
            rids.push_back(InsertRecord(data, length));
        }
        return rids;
    }

    for (const auto& [data, length] : records) {
        auto slot = current_page->InsertRecord(data, length);

        if (slot.has_value()) {
            rids.emplace_back(current_page_id, slot.value());
            record_count_++;
        } else {
            // Current page is full — unpin and get a new one
            bpm_->UnpinPage(current_page_id, true);

            // Remove from free space directory
            auto it = std::find(pages_with_space_.begin(),
                                pages_with_space_.end(), current_page_id);
            if (it != pages_with_space_.end()) {
                pages_with_space_.erase(it);
            }

            // Allocate or find a new page
            current_page_id = FindPageWithSpace(length);
            current_page = bpm_->FetchPage(current_page_id);

            if (current_page == nullptr) {
                rids.emplace_back();  // Invalid RID
                continue;
            }

            slot = current_page->InsertRecord(data, length);
            if (slot.has_value()) {
                rids.emplace_back(current_page_id, slot.value());
                record_count_++;
            } else {
                rids.emplace_back();  // Record too large
            }
        }
    }

    // Unpin the last page
    if (current_page != nullptr) {
        // Update free space tracking
        if (!current_page->HasSpaceFor(64)) {
            auto it = std::find(pages_with_space_.begin(),
                                pages_with_space_.end(), current_page_id);
            if (it != pages_with_space_.end()) {
                pages_with_space_.erase(it);
            }
        }
        bpm_->UnpinPage(current_page_id, true);
    }

    return rids;
}

// =============================================================================
// Private Helpers
// =============================================================================

PageId HeapFile::AllocateNewPage() {
    Page* new_page = bpm_->NewPage();
    if (new_page == nullptr) {
        return INVALID_PAGE_ID;
    }

    PageId new_page_id = new_page->GetPageId();
    new_page->SetFlags(static_cast<uint16_t>(PageType::kData));

    // Link the new page into the chain
    if (last_page_id_ != INVALID_PAGE_ID && last_page_id_ != new_page_id) {
        Page* last_page = bpm_->FetchPage(last_page_id_);
        if (last_page != nullptr) {
            last_page->SetNextPageId(new_page_id);
            bpm_->UnpinPage(last_page_id_, true);
        }
    }

    last_page_id_ = new_page_id;
    page_count_++;
    pages_with_space_.push_back(new_page_id);

    bpm_->UnpinPage(new_page_id, true);
    return new_page_id;
}

PageId HeapFile::FindPageWithSpace(uint16_t record_length) {
    // Check the free space directory first
    for (auto it = pages_with_space_.begin(); it != pages_with_space_.end(); ++it) {
        Page* page = bpm_->FetchPage(*it);
        if (page == nullptr) {
            it = pages_with_space_.erase(it);
            --it;
            continue;
        }

        if (page->HasSpaceFor(record_length)) {
            bpm_->UnpinPage(*it, false);
            return *it;
        }

        // Page doesn't have enough space anymore — remove from directory
        bpm_->UnpinPage(*it, false);
        it = pages_with_space_.erase(it);
        --it;
    }

    // No page with space found — allocate a new one
    return AllocateNewPage();
}

// =============================================================================
// HeapFileIterator Implementation
// =============================================================================

HeapFileIterator::HeapFileIterator(BufferPoolManager* bpm, PageId first_page_id)
    : bpm_(bpm), current_page_id_(first_page_id), current_slot_(0) {

    if (current_page_id_ == INVALID_PAGE_ID) {
        valid_ = false;
        return;
    }

    current_page_ = bpm_->FetchPage(current_page_id_);
    if (current_page_ == nullptr) {
        valid_ = false;
        return;
    }

    // Find the first valid record
    if (current_page_->GetNumSlots() == 0) {
        // Empty page — try next pages
        AdvanceToNextRecord();
    } else {
        // Check if slot 0 is valid
        auto result = current_page_->GetRecord(0);
        if (result.has_value()) {
            valid_ = true;
            current_rid_ = RID{current_page_id_, 0};
        } else {
            // Slot 0 is empty — advance
            current_slot_ = 0;
            AdvanceToNextRecord();
        }
    }
}

std::pair<const uint8_t*, uint16_t> HeapFileIterator::GetRecord() const {
    assert(valid_);
    auto result = current_page_->GetRecord(current_slot_);
    assert(result.has_value());
    return result.value();
}

void HeapFileIterator::Next() {
    if (!valid_) return;
    AdvanceToNextRecord();
}

void HeapFileIterator::AdvanceToNextRecord() {
    // Try remaining slots in the current page
    if (current_page_ != nullptr) {
        uint16_t num_slots = current_page_->GetNumSlots();
        for (uint16_t slot = current_slot_ + 1; slot < num_slots; ++slot) {
            auto result = current_page_->GetRecord(slot);
            if (result.has_value()) {
                current_slot_ = slot;
                current_rid_ = RID{current_page_id_, slot};
                valid_ = true;
                return;
            }
        }

        // No more valid slots in this page — move to next page
        PageId next_page_id = current_page_->GetNextPageId();
        bpm_->UnpinPage(current_page_id_, false);
        current_page_ = nullptr;

        // Walk through subsequent pages
        while (next_page_id != INVALID_PAGE_ID) {
            current_page_ = bpm_->FetchPage(next_page_id);
            if (current_page_ == nullptr) {
                break;
            }

            current_page_id_ = next_page_id;
            uint16_t slots = current_page_->GetNumSlots();

            for (uint16_t slot = 0; slot < slots; ++slot) {
                auto result = current_page_->GetRecord(slot);
                if (result.has_value()) {
                    current_slot_ = slot;
                    current_rid_ = RID{current_page_id_, slot};
                    valid_ = true;
                    return;
                }
            }

            // This page has no valid records — move on
            next_page_id = current_page_->GetNextPageId();
            bpm_->UnpinPage(current_page_id_, false);
            current_page_ = nullptr;
        }
    }

    // No more records anywhere
    valid_ = false;
    current_rid_ = RID{};
}

}  // namespace courtdb
