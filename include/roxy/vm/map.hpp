#pragma once

#include "roxy/core/types.hpp"
#include "roxy/rt/roxy_rt.h"

namespace rx {

// Forward declarations
struct RoxyVM;

// Dispatch tag for hash/equality based on key type. Values match the
// `ROXY_MAP_KEY_*` defines in roxy_rt.h so a `MapKeyKind` cast to/from
// `uint8_t` round-trips through the unified header's `key_kind` field.
enum class MapKeyKind : u8 {
    Integer = ROXY_MAP_KEY_INTEGER,    // i8..i64, u8..u64, bool, enum
    Float32 = ROXY_MAP_KEY_FLOAT32,    // Normalize -0→+0, then hash bit representation
    Float64 = ROXY_MAP_KEY_FLOAT64,
    String  = ROXY_MAP_KEY_STRING,     // Dereference pointer, hash via cached header field
    Struct  = ROXY_MAP_KEY_STRUCT,     // Bytewise FNV-1a hash + memcmp, or user Hash/Eq
};

// `MapHeader` is now a typedef of the unified C runtime header (see
// rt/roxy_rt.h). The header is a bridge layout — both the AOT function
// pointers (`hash_fn`/`eq_fn`) and the VM bytecode indices
// (`hash_fn_index`/`eq_fn_index`) coexist; Phase 5 collapses this by
// routing VM dispatch through a thread-local trampoline. VM code that
// switches on `header->key_kind` casts to `MapKeyKind`.
using MapHeader = roxy_map_header;

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
// `hash_fn_index` / `eq_fn_index` are bytecode function indices for the user's
// Hash and Eq methods on Struct keys (UINT32_MAX = no custom impl, fall back
// to bytewise). Ignored for non-struct key kinds.
// Returns pointer to map data (MapHeader).
void* map_alloc(RoxyVM* vm, MapKeyKind key_kind, u32 capacity,
                u8 key_slot_count = 2, bool key_is_inline = true,
                u8 value_slot_count = 2, bool value_is_inline = true,
                u32 hash_fn_index = UINT32_MAX, u32 eq_fn_index = UINT32_MAX);

// Deep-copy a map
void* map_copy(RoxyVM* vm, void* src);

// Get map length
inline u32 map_length(const void* data) {
    return get_map_header(data)->length;
}

// All key reads now use byte pointers. `key_src` points to
// `key_slot_count * 4` bytes that the runtime hashes/compares per the
// configured key kind. `vm` is required so the runtime can call user
// Hash/Eq methods on Struct keys via call_user_function.

bool map_contains(RoxyVM* vm, const void* data, const u32* key_src);

// Get value pointer by key. Returns pointer into the map's value storage on
// success (valid until the next insert/remove/clear), nullptr if key not found
// (sets *error). The caller reads `value_slot_count * 4` bytes from the returned
// pointer. For primitive values, this is also convertible to u64 via memcpy.
const u32* map_get_ptr(RoxyVM* vm, const void* data, const u32* key_src, const char** error);

// Insert or update a key-value pair. `key_src` and `value_src` each point to
// `key_slot_count * 4` and `value_slot_count * 4` bytes respectively.
void map_insert(RoxyVM* vm, void* data, const u32* key_src, const u32* value_src);

// Remove a key. Returns true if key was present, false otherwise.
bool map_remove(RoxyVM* vm, void* data, const u32* key_src);

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
