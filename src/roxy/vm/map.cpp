#include "roxy/vm/map.hpp"
#include "roxy/vm/object.hpp"
#include "roxy/vm/list.hpp"
#include "roxy/vm/string.hpp"
#include "roxy/vm/value.hpp"

#include <cassert>
#include <cstdlib>
#include <cstring>

namespace rx {

// Global map type ID (registered once at startup)
static u32 g_map_type_id = UINT32_MAX;

u32 register_map_type() {
    if (g_map_type_id == UINT32_MAX) {
        g_map_type_id = register_object_type("map", 0, nullptr);
    }
    return g_map_type_id;
}

u32 get_map_type_id() {
    return g_map_type_id;
}

// --- Hash functions ---

// SplitMix64 bit mixer for integer keys
static u64 hash_integer(u64 x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

// FNV-1a hash for string keys
static u64 hash_string_bytes(const char* data, u32 length) {
    u64 h = 14695981039346656037ULL;
    for (u32 i = 0; i < length; i++) {
        h ^= static_cast<u64>(static_cast<u8>(data[i]));
        h *= 1099511628211ULL;
    }
    return h;
}

static u64 map_hash_key(u64 key, MapKeyKind kind) {
    switch (kind) {
        case MapKeyKind::Integer:
            return hash_integer(key);
        case MapKeyKind::Float32: {
            // Normalize -0.0 to +0.0
            f32 val;
            memcpy(&val, &key, sizeof(f32));
            if (val == 0.0f) val = 0.0f; // Normalize negative zero
            u32 bits;
            memcpy(&bits, &val, sizeof(u32));
            return hash_integer(static_cast<u64>(bits));
        }
        case MapKeyKind::Float64: {
            // Normalize -0.0 to +0.0
            f64 val;
            memcpy(&val, &key, sizeof(f64));
            if (val == 0.0) val = 0.0; // Normalize negative zero
            u64 bits;
            memcpy(&bits, &val, sizeof(u64));
            return hash_integer(bits);
        }
        case MapKeyKind::String: {
            void* str = reinterpret_cast<void*>(key);
            if (!str) return 0;
            return hash_string_bytes(string_chars(str), string_length(str));
        }
    }
    return hash_integer(key);
}

// --- Key equality ---

static bool map_keys_equal(u64 a, u64 b, MapKeyKind kind) {
    switch (kind) {
        case MapKeyKind::Integer:
            return a == b;
        case MapKeyKind::Float32: {
            f32 fa, fb;
            memcpy(&fa, &a, sizeof(f32));
            memcpy(&fb, &b, sizeof(f32));
            return fa == fb;
        }
        case MapKeyKind::Float64: {
            f64 fa, fb;
            memcpy(&fa, &a, sizeof(f64));
            memcpy(&fb, &b, sizeof(f64));
            return fa == fb;
        }
        case MapKeyKind::String: {
            void* str_a = reinterpret_cast<void*>(a);
            void* str_b = reinterpret_cast<void*>(b);
            return string_equals(str_a, str_b);
        }
    }
    return a == b;
}

// --- Internal helpers ---

static void map_alloc_buckets(MapHeader* header, u32 capacity) {
    assert(capacity > 0 && (capacity & (capacity - 1)) == 0); // Must be power of 2
    header->capacity = capacity;
    header->distances = static_cast<u8*>(calloc(capacity, sizeof(u8)));
    header->keys = static_cast<u64*>(calloc(capacity, sizeof(u64)));
    header->values = static_cast<u64*>(calloc(capacity, sizeof(u64)));
}

static void map_free_buckets(MapHeader* header) {
    free(header->distances);
    free(header->keys);
    free(header->values);
    header->distances = nullptr;
    header->keys = nullptr;
    header->values = nullptr;
    header->capacity = 0;
}

// Insert without grow check (used during rehash). Assumes capacity > 0.
static void map_insert_internal(MapHeader* header, u64 key, u64 value) {
    u32 mask = header->capacity - 1;
    u64 hash = map_hash_key(key, header->key_kind);
    u32 pos = static_cast<u32>(hash) & mask;
    u8 dist = 1; // Distance is stored as distance+1 (0 = empty)

    while (true) {
        if (header->distances[pos] == 0) {
            // Empty slot — place here
            header->distances[pos] = dist;
            header->keys[pos] = key;
            header->values[pos] = value;
            return;
        }

        // Robin Hood: steal from the rich (entries closer to their ideal position)
        if (header->distances[pos] < dist) {
            // Swap with existing entry
            u8 tmp_dist = header->distances[pos];
            u64 tmp_key = header->keys[pos];
            u64 tmp_val = header->values[pos];
            header->distances[pos] = dist;
            header->keys[pos] = key;
            header->values[pos] = value;
            dist = tmp_dist;
            key = tmp_key;
            value = tmp_val;
        }

        pos = (pos + 1) & mask;
        dist++;

        // Safety: dist should never reach 255 with reasonable load factors
        assert(dist < 255);
    }
}

static void map_grow(MapHeader* header) {
    u32 old_capacity = header->capacity;
    u8* old_distances = header->distances;
    u64* old_keys = header->keys;
    u64* old_values = header->values;

    u32 new_capacity = old_capacity == 0 ? 8 : old_capacity * 2;
    map_alloc_buckets(header, new_capacity);
    header->length = 0; // Will re-count during rehash

    // Rehash all existing entries
    u32 rehashed = 0;
    for (u32 i = 0; i < old_capacity; i++) {
        if (old_distances[i] != 0) {
            map_insert_internal(header, old_keys[i], old_values[i]);
            header->length++;
            rehashed++;
        }
    }

    free(old_distances);
    free(old_keys);
    free(old_values);
}

// --- Public API ---

void* map_alloc(RoxyVM* vm, MapKeyKind key_kind, u32 capacity) {
    u32 data_size = sizeof(MapHeader);
    void* data = object_alloc(vm, g_map_type_id, data_size);
    if (!data) return nullptr;

    MapHeader* header = get_map_header(data);
    header->length = 0;
    header->capacity = 0;
    header->key_kind = key_kind;
    header->distances = nullptr;
    header->keys = nullptr;
    header->values = nullptr;

    // Pre-allocate if requested
    if (capacity > 0) {
        // Round up to next power of 2
        u32 actual = 8;
        while (actual < capacity) actual *= 2;
        map_alloc_buckets(header, actual);
    }

    return data;
}

void* map_copy(RoxyVM* vm, void* src) {
    if (!src) return nullptr;
    const MapHeader* src_header = get_map_header(src);

    void* dst = map_alloc(vm, src_header->key_kind, src_header->capacity);
    if (!dst) return nullptr;

    MapHeader* dst_header = get_map_header(dst);
    if (src_header->capacity > 0) {
        dst_header->length = src_header->length;
        memcpy(dst_header->distances, src_header->distances, sizeof(u8) * src_header->capacity);
        memcpy(dst_header->keys, src_header->keys, sizeof(u64) * src_header->capacity);
        memcpy(dst_header->values, src_header->values, sizeof(u64) * src_header->capacity);
    }
    return dst;
}

bool map_contains(const void* data, u64 key) {
    const MapHeader* header = get_map_header(data);
    if (header->capacity == 0 || header->length == 0) return false;

    u32 mask = header->capacity - 1;
    u64 hash = map_hash_key(key, header->key_kind);
    u32 pos = static_cast<u32>(hash) & mask;
    u8 dist = 1;

    while (true) {
        if (header->distances[pos] == 0) return false;
        if (header->distances[pos] < dist) return false; // Early termination
        if (header->distances[pos] == dist &&
            map_keys_equal(header->keys[pos], key, header->key_kind)) {
            return true;
        }
        pos = (pos + 1) & mask;
        dist++;
    }
}

bool map_get(const void* data, u64 key, u64& out_value, const char** error) {
    if (data == nullptr) {
        *error = "Null map reference";
        return false;
    }

    const MapHeader* header = get_map_header(data);
    if (header->capacity == 0 || header->length == 0) {
        *error = "Map key not found";
        return false;
    }

    u32 mask = header->capacity - 1;
    u64 hash = map_hash_key(key, header->key_kind);
    u32 pos = static_cast<u32>(hash) & mask;
    u8 dist = 1;

    while (true) {
        if (header->distances[pos] == 0) {
            *error = "Map key not found";
            return false;
        }
        if (header->distances[pos] < dist) {
            *error = "Map key not found";
            return false;
        }
        if (header->distances[pos] == dist &&
            map_keys_equal(header->keys[pos], key, header->key_kind)) {
            out_value = header->values[pos];
            return true;
        }
        pos = (pos + 1) & mask;
        dist++;
    }
}

void map_insert(void* data, u64 key, u64 value) {
    MapHeader* header = get_map_header(data);

    // Check if key already exists — update in place
    if (header->capacity > 0 && header->length > 0) {
        u32 mask = header->capacity - 1;
        u64 hash = map_hash_key(key, header->key_kind);
        u32 pos = static_cast<u32>(hash) & mask;
        u8 dist = 1;

        while (true) {
            if (header->distances[pos] == 0) break;
            if (header->distances[pos] < dist) break;
            if (header->distances[pos] == dist &&
                map_keys_equal(header->keys[pos], key, header->key_kind)) {
                // Key exists — overwrite value
                header->values[pos] = value;
                return;
            }
            pos = (pos + 1) & mask;
            dist++;
        }
    }

    // New key — check load factor and grow if needed
    // Grow at ~80% load factor (capacity * 4/5)
    if (header->capacity == 0 || (header->length + 1) > header->capacity * 4 / 5) {
        map_grow(header);
    }

    map_insert_internal(header, key, value);
    header->length++;
}

bool map_remove(void* data, u64 key) {
    MapHeader* header = get_map_header(data);
    if (header->capacity == 0 || header->length == 0) return false;

    u32 mask = header->capacity - 1;
    u64 hash = map_hash_key(key, header->key_kind);
    u32 pos = static_cast<u32>(hash) & mask;
    u8 dist = 1;

    // Find the key
    while (true) {
        if (header->distances[pos] == 0) return false;
        if (header->distances[pos] < dist) return false;
        if (header->distances[pos] == dist &&
            map_keys_equal(header->keys[pos], key, header->key_kind)) {
            break; // Found it at pos
        }
        pos = (pos + 1) & mask;
        dist++;
    }

    // Backward-shift deletion: shift subsequent entries toward their ideal position
    header->length--;
    while (true) {
        u32 next = (pos + 1) & mask;
        if (header->distances[next] <= 1) {
            // Next slot is empty or at ideal position — stop
            header->distances[pos] = 0;
            header->keys[pos] = 0;
            header->values[pos] = 0;
            return true;
        }
        // Shift next entry backward
        header->distances[pos] = header->distances[next] - 1;
        header->keys[pos] = header->keys[next];
        header->values[pos] = header->values[next];
        pos = next;
    }
}

void map_clear(void* data) {
    MapHeader* header = get_map_header(data);
    header->length = 0;
    if (header->capacity > 0) {
        memset(header->distances, 0, sizeof(u8) * header->capacity);
        memset(header->keys, 0, sizeof(u64) * header->capacity);
        memset(header->values, 0, sizeof(u64) * header->capacity);
    }
}

void* map_keys(RoxyVM* vm, void* data) {
    const MapHeader* header = get_map_header(data);
    void* lst = list_alloc(vm, header->length, 2);
    if (!lst) return nullptr;

    for (u32 i = 0; i < header->capacity; i++) {
        if (header->distances[i] != 0) {
            list_push(lst, Value::from_u64(header->keys[i]));
        }
    }
    return lst;
}

void* map_values(RoxyVM* vm, void* data) {
    const MapHeader* header = get_map_header(data);
    void* lst = list_alloc(vm, header->length, 2);
    if (!lst) return nullptr;

    for (u32 i = 0; i < header->capacity; i++) {
        if (header->distances[i] != 0) {
            list_push(lst, Value::from_u64(header->values[i]));
        }
    }
    return lst;
}

}
