#pragma once

#include "roxy/core/types.hpp"

namespace rx {

// Forward declarations
struct RoxyVM;

// Dispatch tag for hash/equality based on key type
enum class MapKeyKind : u8 {
    Integer,    // i8..i64, u8..u64, bool, enum — hash/compare the u64 bits
    Float32,    // Normalize -0→+0, then hash bit representation
    Float64,
    String,     // Dereference pointer, FNV-1a hash, string_equals for eq
};

// Map header - stored in object data after ObjectHeader
// Memory layout: [ObjectHeader][MapHeader]
// Separate malloc'd arrays for distances, keys, values
struct MapHeader {
    u32 length;          // Number of live entries
    u32 capacity;        // Number of buckets (always power of 2, 0 if not allocated)
    MapKeyKind key_kind; // Dispatch tag for hash/equality
    u8 value_slot_count; // u32 slots per value (1 for primitives ≤ 4B, 2 for 8B, N for structs)
    bool value_is_inline;// true = value fits in a single register (primitive); false = struct, source is a pointer
    u8* distances;       // Per-bucket Robin Hood distance+1 (0 = empty)
    u64* keys;           // Key storage (capacity entries)
    u32* values;         // Value storage: capacity * value_slot_count u32 slots
};

// Get the MapHeader from map data pointer
inline MapHeader* get_map_header(void* data) {
    return static_cast<MapHeader*>(data);
}

inline const MapHeader* get_map_header(const void* data) {
    return static_cast<const MapHeader*>(data);
}

// Allocate a new map.
// `value_slot_count` is the number of u32 slots per value (1 for small primitives,
// 2 for 8-byte types like i64/f64/string/pointers, N for structs).
// `value_is_inline` indicates whether values pass through registers by value
// (true, for primitives) or by pointer (false, for structs). This controls how
// insert/get interpret their register arguments.
// Returns pointer to map data (MapHeader).
void* map_alloc(RoxyVM* vm, MapKeyKind key_kind, u32 capacity,
                u8 value_slot_count = 2, bool value_is_inline = true);

// Deep-copy a map
void* map_copy(RoxyVM* vm, void* src);

// Get map length
inline u32 map_length(const void* data) {
    return get_map_header(data)->length;
}

// Check if a key exists in the map
bool map_contains(const void* data, u64 key);

// Get value pointer by key. Returns pointer into the map's value storage on
// success (valid until the next insert/remove/clear), nullptr if key not found
// (sets *error). The caller reads `value_slot_count * 4` bytes from the returned
// pointer. For primitive values, this is also convertible to u64 via memcpy.
const u32* map_get_ptr(const void* data, u64 key, const char** error);

// Insert or update a key-value pair. `value_src` points to
// `value_slot_count * 4` bytes of value data that will be copied into the map.
void map_insert(void* data, u64 key, const u32* value_src);

// Remove a key. Returns true if key was present, false otherwise.
bool map_remove(void* data, u64 key);

// Remove all entries (keeps allocated memory)
void map_clear(void* data);

// Return all keys as a new List<K>
void* map_keys(RoxyVM* vm, void* data);

// Return all values as a new List<V>
void* map_values(RoxyVM* vm, void* data);

// Register the map object type (call at initialization)
u32 register_map_type();

// Get the registered map type ID
u32 get_map_type_id();

}
