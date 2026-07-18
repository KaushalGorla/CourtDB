/**
 * =============================================================================
 * CourtDB - Log Record Implementation
 * =============================================================================
 */

#include "recovery/log_record.h"

#include <cassert>
#include <cstring>

namespace courtdb {

// =============================================================================
// Factory Methods
// =============================================================================

LogRecord LogRecord::MakeInsert(LSN lsn, PageId page_id, uint16_t slot_id,
                                const uint8_t* data, uint16_t length) {
    LogRecord record;
    record.header_.type = LogRecordType::kInsert;
    record.header_.lsn = lsn;
    record.header_.page_id = page_id;
    record.header_.slot_id = slot_id;
    record.header_.payload_size = length;
    record.payload_.assign(data, data + length);
    record.header_.total_size = LogRecordHeader::HEADER_SIZE + length;
    return record;
}

LogRecord LogRecord::MakeDelete(LSN lsn, PageId page_id, uint16_t slot_id,
                                const uint8_t* old_data, uint16_t old_length) {
    LogRecord record;
    record.header_.type = LogRecordType::kDelete;
    record.header_.lsn = lsn;
    record.header_.page_id = page_id;
    record.header_.slot_id = slot_id;
    record.header_.payload_size = old_length;
    record.payload_.assign(old_data, old_data + old_length);
    record.header_.total_size = LogRecordHeader::HEADER_SIZE + old_length;
    return record;
}

LogRecord LogRecord::MakeUpdate(LSN lsn, PageId page_id, uint16_t slot_id,
                                const uint8_t* old_data, uint16_t old_length,
                                const uint8_t* new_data, uint16_t new_length) {
    LogRecord record;
    record.header_.type = LogRecordType::kUpdate;
    record.header_.lsn = lsn;
    record.header_.page_id = page_id;
    record.header_.slot_id = slot_id;

    // Payload: [2B old_len][old_data][2B new_len][new_data]
    uint16_t payload_size = 2 + old_length + 2 + new_length;
    record.header_.payload_size = payload_size;
    record.payload_.resize(payload_size);

    uint16_t offset = 0;
    std::memcpy(record.payload_.data() + offset, &old_length, 2); offset += 2;
    std::memcpy(record.payload_.data() + offset, old_data, old_length); offset += old_length;
    std::memcpy(record.payload_.data() + offset, &new_length, 2); offset += 2;
    std::memcpy(record.payload_.data() + offset, new_data, new_length);

    record.header_.total_size = LogRecordHeader::HEADER_SIZE + payload_size;
    return record;
}

LogRecord LogRecord::MakeNewPage(LSN lsn, PageId page_id) {
    LogRecord record;
    record.header_.type = LogRecordType::kNewPage;
    record.header_.lsn = lsn;
    record.header_.page_id = page_id;
    record.header_.slot_id = 0;
    record.header_.payload_size = 0;
    record.header_.total_size = LogRecordHeader::HEADER_SIZE;
    return record;
}

LogRecord LogRecord::MakeCheckpointBegin(LSN lsn) {
    LogRecord record;
    record.header_.type = LogRecordType::kCheckpointBegin;
    record.header_.lsn = lsn;
    record.header_.page_id = INVALID_PAGE_ID;
    record.header_.slot_id = 0;
    record.header_.payload_size = 0;
    record.header_.total_size = LogRecordHeader::HEADER_SIZE;
    return record;
}

LogRecord LogRecord::MakeCheckpointEnd(LSN lsn,
                                       const std::vector<std::pair<PageId, LSN>>& dirty_pages) {
    LogRecord record;
    record.header_.type = LogRecordType::kCheckpointEnd;
    record.header_.lsn = lsn;
    record.header_.page_id = INVALID_PAGE_ID;
    record.header_.slot_id = 0;

    // Payload: [4B count][ (4B page_id + 8B lsn) * count ]
    uint32_t count = static_cast<uint32_t>(dirty_pages.size());
    uint16_t payload_size = 4 + count * 12;
    record.header_.payload_size = payload_size;
    record.payload_.resize(payload_size);

    uint16_t offset = 0;
    std::memcpy(record.payload_.data() + offset, &count, 4); offset += 4;
    for (const auto& [pid, page_lsn] : dirty_pages) {
        std::memcpy(record.payload_.data() + offset, &pid, 4); offset += 4;
        std::memcpy(record.payload_.data() + offset, &page_lsn, 8); offset += 8;
    }

    record.header_.total_size = LogRecordHeader::HEADER_SIZE + payload_size;
    return record;
}

// =============================================================================
// Serialization
// =============================================================================

std::vector<uint8_t> LogRecord::Serialize() const {
    std::vector<uint8_t> buf(header_.total_size);
    uint16_t offset = 0;

    // Header
    std::memcpy(buf.data() + offset, &header_.total_size, 4); offset += 4;
    std::memcpy(buf.data() + offset, &header_.lsn, 8); offset += 8;
    buf[offset++] = static_cast<uint8_t>(header_.type);
    std::memcpy(buf.data() + offset, &header_.page_id, 4); offset += 4;
    std::memcpy(buf.data() + offset, &header_.slot_id, 2); offset += 2;
    std::memcpy(buf.data() + offset, &header_.payload_size, 2); offset += 2;

    // Payload
    if (!payload_.empty()) {
        std::memcpy(buf.data() + offset, payload_.data(), payload_.size());
    }

    return buf;
}

LogRecord LogRecord::Deserialize(const uint8_t* data, uint32_t length) {
    LogRecord record;
    uint16_t offset = 0;

    // Header
    std::memcpy(&record.header_.total_size, data + offset, 4); offset += 4;
    std::memcpy(&record.header_.lsn, data + offset, 8); offset += 8;
    record.header_.type = static_cast<LogRecordType>(data[offset++]);
    std::memcpy(&record.header_.page_id, data + offset, 4); offset += 4;
    std::memcpy(&record.header_.slot_id, data + offset, 2); offset += 2;
    std::memcpy(&record.header_.payload_size, data + offset, 2); offset += 2;

    // Payload
    if (record.header_.payload_size > 0 && offset + record.header_.payload_size <= length) {
        record.payload_.assign(data + offset,
                               data + offset + record.header_.payload_size);
    }

    return record;
}

// =============================================================================
// Accessors
// =============================================================================

std::pair<const uint8_t*, uint16_t> LogRecord::GetInsertData() const {
    assert(header_.type == LogRecordType::kInsert);
    return {payload_.data(), header_.payload_size};
}

std::pair<const uint8_t*, uint16_t> LogRecord::GetDeleteData() const {
    assert(header_.type == LogRecordType::kDelete);
    return {payload_.data(), header_.payload_size};
}

LogRecord::UpdateData LogRecord::GetUpdateData() const {
    assert(header_.type == LogRecordType::kUpdate);

    UpdateData result{};
    uint16_t offset = 0;

    uint16_t old_len;
    std::memcpy(&old_len, payload_.data() + offset, 2); offset += 2;
    result.old_data = payload_.data() + offset;
    result.old_length = old_len;
    offset += old_len;

    uint16_t new_len;
    std::memcpy(&new_len, payload_.data() + offset, 2); offset += 2;
    result.new_data = payload_.data() + offset;
    result.new_length = new_len;

    return result;
}

std::vector<std::pair<PageId, LSN>> LogRecord::GetDirtyPageTable() const {
    assert(header_.type == LogRecordType::kCheckpointEnd);

    std::vector<std::pair<PageId, LSN>> table;
    if (payload_.empty()) return table;

    uint16_t offset = 0;
    uint32_t count;
    std::memcpy(&count, payload_.data() + offset, 4); offset += 4;

    table.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        PageId pid;
        LSN page_lsn;
        std::memcpy(&pid, payload_.data() + offset, 4); offset += 4;
        std::memcpy(&page_lsn, payload_.data() + offset, 8); offset += 8;
        table.emplace_back(pid, page_lsn);
    }

    return table;
}

}  // namespace courtdb
