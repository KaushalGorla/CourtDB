#pragma once
/**
 * =============================================================================
 * CourtDB - Record Serialization
 * =============================================================================
 *
 * Purpose:
 *   Defines the schema for NBA play-by-play records and provides efficient
 *   binary serialization/deserialization. This is the bridge between
 *   application-level data and the raw bytes stored in pages.
 *
 * Responsibilities:
 *   - Define the NBA play-by-play event schema
 *   - Serialize structured records into compact byte sequences
 *   - Deserialize byte sequences back into structured records
 *   - Handle variable-length fields (strings) efficiently
 *   - Provide a Record ID (RID) type for addressing records
 *
 * Time Complexity:
 *   - Serialize: O(n) where n is total field sizes
 *   - Deserialize: O(n)
 *   - Field access: O(1) on deserialized record
 *
 * Important Invariants:
 *   - Serialized format is deterministic (same input → same bytes)
 *   - All integers are stored in host byte order (embedded DB, no network)
 *   - Variable-length strings are length-prefixed (2-byte length)
 *   - A RID (page_id, slot_id) uniquely identifies a record in the database
 *
 * Design Decisions:
 *   - Fixed schema (not a generic tuple store) for maximum performance
 *   - Length-prefixed strings avoid null terminators and enable binary data
 *   - Compact binary format minimizes page space usage
 *   - No alignment padding in serialized form (space over access speed on disk)
 *
 * =============================================================================
 */

#include "storage/page.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace courtdb {

// =============================================================================
// Record ID (RID)
// =============================================================================

/**
 * A Record ID uniquely addresses a record within the database.
 * It's the combination of the page containing the record and
 * the slot within that page.
 */
struct RID {
    PageId page_id = INVALID_PAGE_ID;
    uint16_t slot_id = 0;

    RID() = default;
    RID(PageId pid, uint16_t sid) : page_id(pid), slot_id(sid) {}

    [[nodiscard]] bool IsValid() const { return page_id != INVALID_PAGE_ID; }

    bool operator==(const RID& other) const {
        return page_id == other.page_id && slot_id == other.slot_id;
    }

    bool operator!=(const RID& other) const { return !(*this == other); }

    bool operator<(const RID& other) const {
        if (page_id != other.page_id) return page_id < other.page_id;
        return slot_id < other.slot_id;
    }
};

// =============================================================================
// NBA Play-by-Play Record Schema
// =============================================================================

/**
 * Represents a single NBA play-by-play event.
 *
 * Schema:
 *   game_id       (string)  - Unique game identifier (e.g., "0022100001")
 *   season        (uint16)  - Season year (e.g., 2023)
 *   quarter       (uint8)   - Quarter number (1-4, 5+ for OT)
 *   clock         (string)  - Game clock (e.g., "11:42")
 *   team          (string)  - Team abbreviation (e.g., "LAL", "GSW")
 *   player        (string)  - Player name
 *   event_type    (uint8)   - Event type code (see EventType enum)
 *   description   (string)  - Full event description
 *   points        (uint8)   - Points scored on this play (0, 1, 2, or 3)
 *   shot_distance (uint16)  - Shot distance in feet (0 if not a shot)
 *   home_score    (uint16)  - Home team score after this event
 *   away_score    (uint16)  - Away team score after this event
 */
struct NBARecord {
    std::string game_id;
    uint16_t season = 0;
    uint8_t quarter = 0;
    std::string clock;
    std::string team;
    std::string player;
    uint8_t event_type = 0;
    std::string description;
    uint8_t points = 0;
    uint16_t shot_distance = 0;
    uint16_t home_score = 0;
    uint16_t away_score = 0;
};

// =============================================================================
// Event Type Codes
// =============================================================================

enum class EventType : uint8_t {
    kMadeShot = 1,
    kMissedShot = 2,
    kFreeThrow = 3,
    kRebound = 4,
    kTurnover = 5,
    kFoul = 6,
    kViolation = 7,
    kSubstitution = 8,
    kTimeout = 9,
    kJumpBall = 10,
    kEjection = 11,
    kPeriodStart = 12,
    kPeriodEnd = 13,
    kOther = 255,
};

// =============================================================================
// Record Serializer
// =============================================================================

/**
 * Handles conversion between NBARecord structs and compact byte arrays.
 *
 * Serialized format (all lengths in bytes):
 *   [2: game_id_len][N: game_id]
 *   [2: season]
 *   [1: quarter]
 *   [2: clock_len][N: clock]
 *   [2: team_len][N: team]
 *   [2: player_len][N: player]
 *   [1: event_type]
 *   [2: description_len][N: description]
 *   [1: points]
 *   [2: shot_distance]
 *   [2: home_score]
 *   [2: away_score]
 *
 * Fixed overhead: 19 bytes (for all fixed fields + length prefixes)
 * Variable: depends on string lengths
 */
class RecordSerializer {
public:
    /**
     * Serialize an NBARecord into a byte vector.
     * @param record The record to serialize
     * @return Serialized bytes
     */
    static std::vector<uint8_t> Serialize(const NBARecord& record);

    /**
     * Deserialize bytes into an NBARecord.
     * @param data Pointer to serialized bytes
     * @param length Number of bytes
     * @return The deserialized record
     */
    static NBARecord Deserialize(const uint8_t* data, uint16_t length);

    /**
     * Compute the serialized size of a record without actually serializing.
     * Useful for checking if a record fits in a page before inserting.
     */
    static uint16_t ComputeSerializedSize(const NBARecord& record);

private:
    /// Write a length-prefixed string into a buffer
    static void WriteString(std::vector<uint8_t>& buf, const std::string& s);

    /// Read a length-prefixed string from a buffer at the given offset
    static std::string ReadString(const uint8_t* data, uint16_t& offset);

    /// Write a fixed-size value into a buffer
    template <typename T>
    static void WriteFixed(std::vector<uint8_t>& buf, T value);

    /// Read a fixed-size value from a buffer at the given offset
    template <typename T>
    static T ReadFixed(const uint8_t* data, uint16_t& offset);
};

}  // namespace courtdb
