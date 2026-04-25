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
    Struct,     // POD struct keys: bytewise FNV-1a hash + memcmp equality
};

// Map header - stored in object data after ObjectHeader
// Memory layout: [ObjectHeader][MapHeader]
// Separate malloc'd arrays for distances, keys, values.
//
// Keys and values are both stored as variable-sized u32 slot arrays sized
// `capacity * key_slot_count` and `capacity * value_slot_count` respectively.
// For Integer/Float/String key kinds, key_slot_count is 2 (a single u64 stored
// across two u32 slots). For Struct keys, key_slot_count is the struct's slot
// count.
struct MapHeader {
    u32 length;          // Number of live entries
    u32 capacity;        // Number of buckets (always power of 2, 0 if not allocated)
    MapKeyKind key_kind; // Dispatch tag for hash/equality
    u8 key_slot_count;   // u32 slots per key (2 for primitives, N for struct keys)
    bool key_is_inline;  // true for primitives (caller passes value), false for structs (caller passes pointer)
    u8 value_slot_count; // u32 slots per value (1 for primitives ≤ 4B, 2 for 8B, N for structs)
    bool value_is_inline;// true = value fits in a single register (primitive); false = struct, source is a pointer
    u8 _pad;
    u8* distances;       // Per-bucket Robin Hood distance+1 (0 = empty)
    u32* keys;           // Key storage: capacity * key_slot_count u32 slots
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
// `key_slot_count` / `key_is_inline` describe the key layout symmetrically to
// values. For primitive keys, slot_count=2 + is_inline=true; for struct keys,
// slot_count is the struct's slot count + is_inline=false.
// Returns pointer to map data (MapHeader).
void* map_alloc(RoxyVM* vm, MapKeyKind key_kind, u32 capacity,
                u8 key_slot_count = 2, bool key_is_inline = true,
                u8 value_slot_count = 2, bool value_is_inline = true);

// Deep-copy a map
void* map_copy(RoxyVM* vm, void* src);

// Get map length
inline u32 map_length(const void* data) {
    return get_map_header(data)->length;
}

// All key reads now use byte pointers. `key_src` points to
// `key_slot_count * 4` bytes that the runtime hashes/compares per the
// configured key kind.

bool map_contains(const void* data, const u32* key_src);

// Get value pointer by key. Returns pointer into the map's value storage on
// success (valid until the next insert/remove/clear), nullptr if key not found
// (sets *error). The caller reads `value_slot_count * 4` bytes from the returned
// pointer. For primitive values, this is also convertible to u64 via memcpy.
const u32* map_get_ptr(const void* data, const u32* key_src, const char** error);

// Insert or update a key-value pair. `key_src` and `value_src` each point to
// `key_slot_count * 4` and `value_slot_count * 4` bytes respectively.
void map_insert(void* data, const u32* key_src, const u32* value_src);

// Remove a key. Returns true if key was present, false otherwise.
bool map_remove(void* data, const u32* key_src);

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
