/**
 * =============================================================================
 * CourtDB - Record Serialization Implementation
 * =============================================================================
 *
 * Binary serialization format for NBA play-by-play records.
 * Optimized for compact storage and fast deserialization.
 *
 * All variable-length strings use a 2-byte length prefix followed by raw bytes.
 * Fixed-size fields are stored directly without padding.
 *
 * =============================================================================
 */

#include "storage/record.h"

#include <cassert>
#include <cstring>

namespace courtdb {

// =============================================================================
// Serialization
// =============================================================================

std::vector<uint8_t> RecordSerializer::Serialize(const NBARecord& record) {
    std::vector<uint8_t> buf;
    buf.reserve(ComputeSerializedSize(record));

    WriteString(buf, record.game_id);
    WriteFixed(buf, record.season);
    WriteFixed(buf, record.quarter);
    WriteString(buf, record.clock);
    WriteString(buf, record.team);
    WriteString(buf, record.player);
    WriteFixed(buf, record.event_type);
    WriteString(buf, record.description);
    WriteFixed(buf, record.points);
    WriteFixed(buf, record.shot_distance);
    WriteFixed(buf, record.home_score);
    WriteFixed(buf, record.away_score);

    return buf;
}

NBARecord RecordSerializer::Deserialize(const uint8_t* data, uint16_t length) {
    NBARecord record;
    uint16_t offset = 0;

    record.game_id = ReadString(data, offset);
    record.season = ReadFixed<uint16_t>(data, offset);
    record.quarter = ReadFixed<uint8_t>(data, offset);
    record.clock = ReadString(data, offset);
    record.team = ReadString(data, offset);
    record.player = ReadString(data, offset);
    record.event_type = ReadFixed<uint8_t>(data, offset);
    record.description = ReadString(data, offset);
    record.points = ReadFixed<uint8_t>(data, offset);
    record.shot_distance = ReadFixed<uint16_t>(data, offset);
    record.home_score = ReadFixed<uint16_t>(data, offset);
    record.away_score = ReadFixed<uint16_t>(data, offset);

    assert(offset <= length);
    return record;
}

uint16_t RecordSerializer::ComputeSerializedSize(const NBARecord& record) {
    uint16_t size = 0;

    // String fields: 2 bytes length prefix + string data each
    size += 2 + static_cast<uint16_t>(record.game_id.size());
    size += 2 + static_cast<uint16_t>(record.clock.size());
    size += 2 + static_cast<uint16_t>(record.team.size());
    size += 2 + static_cast<uint16_t>(record.player.size());
    size += 2 + static_cast<uint16_t>(record.description.size());

    // Fixed fields: season(2) + quarter(1) + event_type(1) + points(1) +
    //               shot_distance(2) + home_score(2) + away_score(2) = 11 bytes
    size += 11;

    return size;
}

// =============================================================================
// Private Helpers
// =============================================================================

void RecordSerializer::WriteString(std::vector<uint8_t>& buf, const std::string& s) {
    uint16_t len = static_cast<uint16_t>(s.size());
    // Write length prefix
    buf.push_back(static_cast<uint8_t>(len & 0xFF));
    buf.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
    // Write string data
    buf.insert(buf.end(), s.begin(), s.end());
}

std::string RecordSerializer::ReadString(const uint8_t* data, uint16_t& offset) {
    uint16_t len = static_cast<uint16_t>(data[offset]) |
                   (static_cast<uint16_t>(data[offset + 1]) << 8);
    offset += 2;
    std::string result(reinterpret_cast<const char*>(data + offset), len);
    offset += len;
    return result;
}

template <typename T>
void RecordSerializer::WriteFixed(std::vector<uint8_t>& buf, T value) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&value);
    buf.insert(buf.end(), bytes, bytes + sizeof(T));
}

template <typename T>
T RecordSerializer::ReadFixed(const uint8_t* data, uint16_t& offset) {
    T value;
    std::memcpy(&value, data + offset, sizeof(T));
    offset += sizeof(T);
    return value;
}

// Explicit template instantiations
template void RecordSerializer::WriteFixed<uint8_t>(std::vector<uint8_t>&, uint8_t);
template void RecordSerializer::WriteFixed<uint16_t>(std::vector<uint8_t>&, uint16_t);
template uint8_t RecordSerializer::ReadFixed<uint8_t>(const uint8_t*, uint16_t&);
template uint16_t RecordSerializer::ReadFixed<uint16_t>(const uint8_t*, uint16_t&);

}  // namespace courtdb
