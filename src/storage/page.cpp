/**
 * =============================================================================
 * CourtDB - Page Implementation
 * =============================================================================
 *
 * Implements the slotted page layout for variable-length record storage.
 *
 * Memory Layout:
 *   Bytes [0..19]:         PageHeader (serialized)
 *   Bytes [20..20+4*N-1]:  Slot directory (N entries, 4 bytes each)
 *   Bytes [free..end-1]:   Free space
 *   Bytes [end..4095]:     Record data (grows backward from page end)
 *
 * =============================================================================
 */

#include "storage/page.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <stdexcept>

namespace courtdb {

// =============================================================================
// Constants for serialization offsets
// =============================================================================

static constexpr uint16_t HEADER_SIZE = 20;       // sizeof(PageHeader) serialized
static constexpr uint16_t SLOT_ENTRY_SIZE = 4;    // sizeof(SlotEntry) serialized

// =============================================================================
// Construction
// =============================================================================

Page::Page() {
    std::memset(data_, 0, PAGE_SIZE);
    header_.free_space_offset = HEADER_SIZE;  // Slot directory starts right after header
    header_.free_space_end = PAGE_SIZE;       // Records start at end of page
}

Page::Page(PageId page_id) : Page() {
    header_.page_id = page_id;
}

Page::Page(Page&& other) noexcept {
    std::memcpy(data_, other.data_, PAGE_SIZE);
    header_ = other.header_;
    slots_ = std::move(other.slots_);
    // Invalidate source
    other.header_.page_id = INVALID_PAGE_ID;
}

Page& Page::operator=(Page&& other) noexcept {
    if (this != &other) {
        std::memcpy(data_, other.data_, PAGE_SIZE);
        header_ = other.header_;
        slots_ = std::move(other.slots_);
        other.header_.page_id = INVALID_PAGE_ID;
    }
    return *this;
}

// =============================================================================
// Record Operations
// =============================================================================

std::optional<uint16_t> Page::InsertRecord(const uint8_t* data, uint16_t length) {
    if (length == 0 || data == nullptr) {
        return std::nullopt;
    }

    // Determine the slot to use (reuse empty slot or allocate new one)
    auto empty_slot = FindEmptySlot();
    uint16_t slot_id;
    uint16_t space_needed = length;

    if (empty_slot.has_value()) {
        slot_id = empty_slot.value();
        // No additional slot directory space needed
    } else {
        slot_id = header_.num_slots;
        space_needed += SLOT_ENTRY_SIZE;  // Need space for new slot entry
    }

    // Check if the record fits in the free region between slot directory and data area
    uint16_t slot_overhead = empty_slot.has_value() ? 0 : SLOT_ENTRY_SIZE;
    if (length + slot_overhead > (header_.free_space_end - header_.free_space_offset)) {
        return std::nullopt;
    }

    // Write record data at the end (growing backward)
    uint16_t record_offset = header_.free_space_end - length;
    std::memcpy(data_ + record_offset, data, length);

    // Update or create slot entry
    if (empty_slot.has_value()) {
        slots_[slot_id] = {record_offset, length};
    } else {
        slots_.push_back({record_offset, length});
        header_.num_slots++;
        header_.free_space_offset += SLOT_ENTRY_SIZE;
    }

    // Update header
    header_.free_space_end = record_offset;
    header_.num_records++;

    return slot_id;
}

bool Page::DeleteRecord(uint16_t slot_id) {
    if (slot_id >= header_.num_slots) {
        return false;
    }

    if (slots_[slot_id].IsEmpty()) {
        return false;  // Already deleted
    }

    // Mark slot as empty (tombstone)
    slots_[slot_id].offset = 0;
    slots_[slot_id].length = 0;
    header_.num_records--;

    return true;
}

std::optional<std::pair<const uint8_t*, uint16_t>> Page::GetRecord(uint16_t slot_id) const {
    if (slot_id >= header_.num_slots) {
        return std::nullopt;
    }

    const auto& slot = slots_[slot_id];
    if (slot.IsEmpty()) {
        return std::nullopt;  // Deleted record
    }

    return std::make_pair(data_ + slot.offset, slot.length);
}

bool Page::UpdateRecord(uint16_t slot_id, const uint8_t* data, uint16_t length) {
    if (slot_id >= header_.num_slots || slots_[slot_id].IsEmpty()) {
        return false;
    }

    auto& slot = slots_[slot_id];

    if (length <= slot.length) {
        // Fits in existing space — update in place
        std::memcpy(data_ + slot.offset, data, length);
        // Note: we don't reclaim the extra bytes here (deferred to Compact)
        slot.length = length;
        return true;
    }

    // New record is larger — check if there's room at the end
    uint16_t new_space_needed = length - slot.length;
    if (new_space_needed > GetFreeSpace()) {
        return false;  // Caller must delete + re-insert (possibly on different page)
    }

    // Delete old record and insert new data at the end
    // We don't actually reclaim the old space until Compact(), but we
    // do write the new record into the free area.
    uint16_t new_offset = header_.free_space_end - length;
    std::memcpy(data_ + new_offset, data, length);
    slot.offset = new_offset;
    slot.length = length;
    header_.free_space_end = new_offset;

    return true;
}

// =============================================================================
// Space Management
// =============================================================================

void Page::Compact() {
    // Collect all live records with their slot indices
    struct LiveRecord {
        uint16_t slot_id;
        uint16_t original_offset;
        uint16_t length;
    };
    std::vector<LiveRecord> live_records;
    live_records.reserve(header_.num_records);

    for (uint16_t i = 0; i < header_.num_slots; ++i) {
        if (!slots_[i].IsEmpty()) {
            live_records.push_back({i, slots_[i].offset, slots_[i].length});
        }
    }

    // Sort by original offset descending (records closer to page end first)
    // This ensures we pack them tightly at the end
    std::sort(live_records.begin(), live_records.end(),
              [](const LiveRecord& a, const LiveRecord& b) {
                  return a.original_offset > b.original_offset;
              });

    // Repack records starting from the end of the page
    uint16_t write_pos = PAGE_SIZE;

    // Use a temporary buffer to avoid overwriting during compaction
    uint8_t temp[PAGE_SIZE];
    std::memcpy(temp, data_, PAGE_SIZE);

    for (auto& rec : live_records) {
        write_pos -= rec.length;
        std::memcpy(data_ + write_pos, temp + rec.original_offset, rec.length);
        slots_[rec.slot_id].offset = write_pos;
    }

    // Update free space boundary
    header_.free_space_end = write_pos;
}

uint16_t Page::GetFreeSpace() const {
    if (header_.free_space_end <= header_.free_space_offset) {
        return 0;
    }
    return header_.free_space_end - header_.free_space_offset;
}

bool Page::HasSpaceFor(uint16_t record_length) const {
    // Need space for the record data + possibly a new slot entry
    auto empty_slot = FindEmptySlot();
    uint16_t total_needed = record_length;
    if (!empty_slot.has_value()) {
        total_needed += SLOT_ENTRY_SIZE;
    }
    return GetFreeSpace() >= total_needed;
}

// =============================================================================
// Serialization
// =============================================================================

void Page::SerializeToBuffer() {
    // Write header
    uint16_t pos = 0;
    std::memcpy(data_ + pos, &header_.page_id, sizeof(header_.page_id));
    pos += sizeof(header_.page_id);  // 4 bytes

    std::memcpy(data_ + pos, &header_.num_slots, sizeof(header_.num_slots));
    pos += sizeof(header_.num_slots);  // 2 bytes

    std::memcpy(data_ + pos, &header_.free_space_offset, sizeof(header_.free_space_offset));
    pos += sizeof(header_.free_space_offset);  // 2 bytes

    std::memcpy(data_ + pos, &header_.free_space_end, sizeof(header_.free_space_end));
    pos += sizeof(header_.free_space_end);  // 2 bytes

    std::memcpy(data_ + pos, &header_.num_records, sizeof(header_.num_records));
    pos += sizeof(header_.num_records);  // 2 bytes

    std::memcpy(data_ + pos, &header_.next_page_id, sizeof(header_.next_page_id));
    pos += sizeof(header_.next_page_id);  // 4 bytes

    std::memcpy(data_ + pos, &header_.flags, sizeof(header_.flags));
    pos += sizeof(header_.flags);  // 2 bytes
    // Total header: 18 bytes, padded to 20
    pos = HEADER_SIZE;

    // Write slot directory
    for (uint16_t i = 0; i < header_.num_slots; ++i) {
        std::memcpy(data_ + pos, &slots_[i].offset, sizeof(slots_[i].offset));
        pos += sizeof(slots_[i].offset);
        std::memcpy(data_ + pos, &slots_[i].length, sizeof(slots_[i].length));
        pos += sizeof(slots_[i].length);
    }
}

void Page::DeserializeFromBuffer() {
    uint16_t pos = 0;

    // Read header
    std::memcpy(&header_.page_id, data_ + pos, sizeof(header_.page_id));
    pos += sizeof(header_.page_id);

    std::memcpy(&header_.num_slots, data_ + pos, sizeof(header_.num_slots));
    pos += sizeof(header_.num_slots);

    std::memcpy(&header_.free_space_offset, data_ + pos, sizeof(header_.free_space_offset));
    pos += sizeof(header_.free_space_offset);

    std::memcpy(&header_.free_space_end, data_ + pos, sizeof(header_.free_space_end));
    pos += sizeof(header_.free_space_end);

    std::memcpy(&header_.num_records, data_ + pos, sizeof(header_.num_records));
    pos += sizeof(header_.num_records);

    std::memcpy(&header_.next_page_id, data_ + pos, sizeof(header_.next_page_id));
    pos += sizeof(header_.next_page_id);

    std::memcpy(&header_.flags, data_ + pos, sizeof(header_.flags));
    pos += sizeof(header_.flags);
    pos = HEADER_SIZE;

    // Read slot directory
    slots_.resize(header_.num_slots);
    for (uint16_t i = 0; i < header_.num_slots; ++i) {
        std::memcpy(&slots_[i].offset, data_ + pos, sizeof(slots_[i].offset));
        pos += sizeof(slots_[i].offset);
        std::memcpy(&slots_[i].length, data_ + pos, sizeof(slots_[i].length));
        pos += sizeof(slots_[i].length);
    }
}

// =============================================================================
// Private Helpers
// =============================================================================

uint16_t Page::SlotDirectoryEnd() const {
    return HEADER_SIZE + (header_.num_slots * SLOT_ENTRY_SIZE);
}

std::optional<uint16_t> Page::FindEmptySlot() const {
    for (uint16_t i = 0; i < header_.num_slots; ++i) {
        if (slots_[i].IsEmpty()) {
            return i;
        }
    }
    return std::nullopt;
}

}  // namespace courtdb
