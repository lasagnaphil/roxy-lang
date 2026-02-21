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
    u8* distances;       // Per-bucket Robin Hood distance+1 (0 = empty)
    u64* keys;           // Key storage (capacity entries)
    u64* values;         // Value storage (capacity entries)
};

// Get the MapHeader from map data pointer
inline MapHeader* get_map_header(void* data) {
    return static_cast<MapHeader*>(data);
}

inline const MapHeader* get_map_header(const void* data) {
    return static_cast<const MapHeader*>(data);
}

// Allocate a new map
// Returns pointer to map data (MapHeader)
void* map_alloc(RoxyVM* vm, MapKeyKind key_kind, u32 capacity);

// Deep-copy a map
void* map_copy(RoxyVM* vm, void* src);

// Get map length
inline u32 map_length(const void* data) {
    return get_map_header(data)->length;
}

// Check if a key exists in the map
bool map_contains(const void* data, u64 key);

// Get value by key. Returns true on success, false if key not found (sets error).
bool map_get(const void* data, u64 key, u64& out_value, const char** error);

// Insert or update a key-value pair
void map_insert(void* data, u64 key, u64 value);

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
