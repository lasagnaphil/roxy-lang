#include "roxy/vm/map.hpp"
#include "roxy/vm/object.hpp"
#include "roxy/vm/list.hpp"
#include "roxy/vm/string.hpp"
#include "roxy/vm/value.hpp"
#include "roxy/vm/interpreter.hpp"

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

// FNV-1a over arbitrary bytes — used for Struct keys.
static u64 hash_bytes(const u8* data, size_t length) {
    u64 h = 14695981039346656037ULL;
    for (size_t i = 0; i < length; i++) {
        h ^= static_cast<u64>(data[i]);
        h *= 1099511628211ULL;
    }
    return h;
}

// Read the leading u64 worth of bytes from a slot array. For 1-slot keys
// (only theoretical — primitives use 2-slot u64-packed storage), the upper
// bytes are 0. For 2-slot keys, this is the full u64 bit-packed key.
static u64 read_packed_u64(const u32* key_src) {
    u64 packed = 0;
    memcpy(&packed, key_src, sizeof(u64));
    return packed;
}

static u64 map_hash_key(RoxyVM* vm, const u32* key_src, const MapHeader* hdr) {
    switch (hdr->key_kind) {
        case MapKeyKind::Integer:
            return hash_integer(read_packed_u64(key_src));
        case MapKeyKind::Float32: {
            // Normalize -0.0 to +0.0
            f32 val;
            memcpy(&val, key_src, sizeof(f32));
            if (val == 0.0f) val = 0.0f;
            u32 bits;
            memcpy(&bits, &val, sizeof(u32));
            return hash_integer(static_cast<u64>(bits));
        }
        case MapKeyKind::Float64: {
            f64 val;
            memcpy(&val, key_src, sizeof(f64));
            if (val == 0.0) val = 0.0;
            u64 bits;
            memcpy(&bits, &val, sizeof(u64));
            return hash_integer(bits);
        }
        case MapKeyKind::String: {
            u64 ptr_bits = read_packed_u64(key_src);
            void* str = reinterpret_cast<void*>(ptr_bits);
            if (!str) return 0;
            return get_string_header(str)->hash;
        }
        case MapKeyKind::Struct:
            // Custom Hash impl: dispatch through user-defined `hash()` method
            // (the IR builder stored its function index in hdr at construction
            // time). Otherwise fall back to bytewise FNV-1a.
            if (vm && hdr->hash_fn_index != UINT32_MAX) {
                u64 args[1] = { reinterpret_cast<u64>(key_src) };
                return call_user_function(vm, hdr->hash_fn_index, args, 1);
            }
            return hash_bytes(reinterpret_cast<const u8*>(key_src),
                              static_cast<size_t>(hdr->key_slot_count) * sizeof(u32));
    }
    return hash_integer(read_packed_u64(key_src));
}

// --- Key equality ---

static bool map_keys_equal(RoxyVM* vm, const u32* a, const u32* b, const MapHeader* hdr) {
    switch (hdr->key_kind) {
        case MapKeyKind::Integer:
            return read_packed_u64(a) == read_packed_u64(b);
        case MapKeyKind::Float32: {
            f32 fa, fb;
            memcpy(&fa, a, sizeof(f32));
            memcpy(&fb, b, sizeof(f32));
            return fa == fb;
        }
        case MapKeyKind::Float64: {
            f64 fa, fb;
            memcpy(&fa, a, sizeof(f64));
            memcpy(&fb, b, sizeof(f64));
            return fa == fb;
        }
        case MapKeyKind::String: {
            u64 a_bits = read_packed_u64(a);
            u64 b_bits = read_packed_u64(b);
            return string_equals(reinterpret_cast<void*>(a_bits),
                                 reinterpret_cast<void*>(b_bits));
        }
        case MapKeyKind::Struct:
            // Custom Eq impl: dispatch through user-defined `eq(other)` method.
            // Otherwise fall back to bytewise memcmp.
            //
            // Calling-convention bridge: `self: ref K` is always a pointer
            // (1 register). `other: K` follows Roxy's struct-arg ABI:
            //   ≤2-slot K  → packed bytes in 1 register
            //   3-4 slot K → packed bytes across 2 registers
            //   5+ slot K  → pointer in 1 register (large struct convention)
            // We pack `b`'s bytes accordingly before invoking via
            // call_user_function (which copies args linearly into the called
            // function's register window — the prologue then unpacks).
            if (vm && hdr->eq_fn_index != UINT32_MAX) {
                u8 ksc = hdr->key_slot_count;
                u64 args[3];
                args[0] = reinterpret_cast<u64>(a);
                u32 argc;
                if (ksc <= 2) {
                    args[1] = 0;
                    memcpy(&args[1], b, static_cast<size_t>(ksc) * sizeof(u32));
                    argc = 2;
                } else if (ksc <= 4) {
                    args[1] = 0;
                    args[2] = 0;
                    memcpy(&args[1], b, sizeof(u64));
                    memcpy(&args[2], b + 2, static_cast<size_t>(ksc - 2) * sizeof(u32));
                    argc = 3;
                } else {
                    // Large struct: pass pointer (matches Roxy ABI for >4 slot).
                    args[1] = reinterpret_cast<u64>(b);
                    argc = 2;
                }
                return call_user_function(vm, hdr->eq_fn_index, args, argc) != 0;
            }
            return memcmp(a, b, static_cast<size_t>(hdr->key_slot_count) * sizeof(u32)) == 0;
    }
    return read_packed_u64(a) == read_packed_u64(b);
}

// --- Internal helpers ---

static inline u32* map_key_ptr(const MapHeader* header, u32 pos) {
    return header->keys + static_cast<size_t>(pos) * header->key_slot_count;
}

static inline u32* map_value_ptr(const MapHeader* header, u32 pos) {
    return header->values + static_cast<size_t>(pos) * header->value_slot_count;
}

static void map_alloc_buckets(MapHeader* header, u32 capacity) {
    assert(capacity > 0 && (capacity & (capacity - 1)) == 0); // Must be power of 2
    header->capacity = capacity;
    header->distances = static_cast<u8*>(calloc(capacity, sizeof(u8)));
    header->keys = static_cast<u32*>(calloc(
        static_cast<size_t>(capacity) * header->key_slot_count, sizeof(u32)));
    header->values = static_cast<u32*>(calloc(
        static_cast<size_t>(capacity) * header->value_slot_count, sizeof(u32)));
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
// `key_src` and `value_src` each point to slot_count*4 bytes to copy.
//
// Robin Hood with variable-sized keys AND values needs ping-pong scratch
// buffers for both, so the entry being placed survives multiple displacements
// in the chain. Stack-allocated for typical sizes; heap fallback otherwise.
static void map_insert_internal(RoxyVM* vm, MapHeader* header,
                                const u32* key_src, const u32* value_src) {
    u32 mask = header->capacity - 1;
    u8 ksc = header->key_slot_count;
    u8 vsc = header->value_slot_count;
    u64 hash = map_hash_key(vm, key_src, header);
    u32 pos = static_cast<u32>(hash) & mask;
    u8 dist = 1;

    u32 stack_ka[16], stack_kb[16];
    u32 stack_va[16], stack_vb[16];
    u32* kbuf_a = (ksc <= 16) ? stack_ka : static_cast<u32*>(malloc(sizeof(u32) * ksc));
    u32* kbuf_b = (ksc <= 16) ? stack_kb : static_cast<u32*>(malloc(sizeof(u32) * ksc));
    u32* vbuf_a = (vsc <= 16) ? stack_va : static_cast<u32*>(malloc(sizeof(u32) * vsc));
    u32* vbuf_b = (vsc <= 16) ? stack_vb : static_cast<u32*>(malloc(sizeof(u32) * vsc));

    // Defensive copy of caller's bytes — value_src/key_src may alias the map's
    // own storage during map_grow's rehash; subsequent swaps would clobber.
    memcpy(kbuf_a, key_src, sizeof(u32) * ksc);
    memcpy(vbuf_a, value_src, sizeof(u32) * vsc);
    u32* ksrc = kbuf_a;
    u32* kscratch = kbuf_b;
    u32* vsrc = vbuf_a;
    u32* vscratch = vbuf_b;

    while (true) {
        if (header->distances[pos] == 0) {
            header->distances[pos] = dist;
            memcpy(map_key_ptr(header, pos), ksrc, sizeof(u32) * ksc);
            memcpy(map_value_ptr(header, pos), vsrc, sizeof(u32) * vsc);
            if (kbuf_a != stack_ka) free(kbuf_a);
            if (kbuf_b != stack_kb) free(kbuf_b);
            if (vbuf_a != stack_va) free(vbuf_a);
            if (vbuf_b != stack_vb) free(vbuf_b);
            return;
        }

        if (header->distances[pos] < dist) {
            u8 tmp_dist = header->distances[pos];
            memcpy(kscratch, map_key_ptr(header, pos), sizeof(u32) * ksc);
            memcpy(vscratch, map_value_ptr(header, pos), sizeof(u32) * vsc);
            header->distances[pos] = dist;
            memcpy(map_key_ptr(header, pos), ksrc, sizeof(u32) * ksc);
            memcpy(map_value_ptr(header, pos), vsrc, sizeof(u32) * vsc);
            dist = tmp_dist;
            u32* tmp = ksrc; ksrc = kscratch; kscratch = tmp;
            tmp = vsrc; vsrc = vscratch; vscratch = tmp;
        }

        pos = (pos + 1) & mask;
        dist++;
        assert(dist < 255);
    }
}

static void map_grow(RoxyVM* vm, MapHeader* header) {
    u32 old_capacity = header->capacity;
    u8* old_distances = header->distances;
    u32* old_keys = header->keys;
    u32* old_values = header->values;
    u8 ksc = header->key_slot_count;
    u8 vsc = header->value_slot_count;

    u32 new_capacity = old_capacity == 0 ? 8 : old_capacity * 2;
    map_alloc_buckets(header, new_capacity);
    header->length = 0;

    for (u32 i = 0; i < old_capacity; i++) {
        if (old_distances[i] != 0) {
            const u32* old_key_ptr = old_keys + static_cast<size_t>(i) * ksc;
            const u32* old_val_ptr = old_values + static_cast<size_t>(i) * vsc;
            map_insert_internal(vm, header, old_key_ptr, old_val_ptr);
            header->length++;
        }
    }

    free(old_distances);
    free(old_keys);
    free(old_values);
}

// --- Public API ---

void* map_alloc(RoxyVM* vm, MapKeyKind key_kind, u32 capacity,
                u8 key_slot_count, bool key_is_inline,
                u8 value_slot_count, bool value_is_inline,
                u32 hash_fn_index, u32 eq_fn_index) {
    u32 data_size = sizeof(MapHeader);
    void* data = object_alloc(vm, g_map_type_id, data_size);
    if (!data) return nullptr;

    MapHeader* header = get_map_header(data);
    header->length = 0;
    header->capacity = 0;
    header->key_kind = key_kind;
    header->key_slot_count = key_slot_count > 0 ? key_slot_count : 2;
    header->key_is_inline = key_is_inline;
    header->value_slot_count = value_slot_count > 0 ? value_slot_count : 2;
    header->value_is_inline = value_is_inline;
    header->hash_fn_index = hash_fn_index;
    header->eq_fn_index = eq_fn_index;
    header->distances = nullptr;
    header->keys = nullptr;
    header->values = nullptr;

    if (capacity > 0) {
        u32 actual = 8;
        while (actual < capacity) actual *= 2;
        map_alloc_buckets(header, actual);
    }

    return data;
}

void* map_copy(RoxyVM* vm, void* src) {
    if (!src) return nullptr;
    const MapHeader* src_header = get_map_header(src);

    void* dst = map_alloc(vm, src_header->key_kind, src_header->capacity,
                          src_header->key_slot_count, src_header->key_is_inline,
                          src_header->value_slot_count, src_header->value_is_inline,
                          src_header->hash_fn_index, src_header->eq_fn_index);
    if (!dst) return nullptr;

    MapHeader* dst_header = get_map_header(dst);
    if (src_header->capacity > 0) {
        dst_header->length = src_header->length;
        memcpy(dst_header->distances, src_header->distances, sizeof(u8) * src_header->capacity);
        memcpy(dst_header->keys, src_header->keys,
               sizeof(u32) * static_cast<size_t>(src_header->capacity) * src_header->key_slot_count);
        memcpy(dst_header->values, src_header->values,
               sizeof(u32) * static_cast<size_t>(src_header->capacity) * src_header->value_slot_count);
    }
    return dst;
}

bool map_contains(RoxyVM* vm, const void* data, const u32* key_src) {
    const MapHeader* header = get_map_header(data);
    if (header->capacity == 0 || header->length == 0) return false;

    u32 mask = header->capacity - 1;
    u64 hash = map_hash_key(vm, key_src, header);
    u32 pos = static_cast<u32>(hash) & mask;
    u8 dist = 1;

    while (true) {
        if (header->distances[pos] == 0) return false;
        if (header->distances[pos] < dist) return false;
        if (header->distances[pos] == dist &&
            map_keys_equal(vm, map_key_ptr(header, pos), key_src, header)) {
            return true;
        }
        pos = (pos + 1) & mask;
        dist++;
    }
}

const u32* map_get_ptr(RoxyVM* vm, const void* data, const u32* key_src, const char** error) {
    if (data == nullptr) {
        *error = "Null map reference";
        return nullptr;
    }

    const MapHeader* header = get_map_header(data);
    if (header->capacity == 0 || header->length == 0) {
        *error = "Map key not found";
        return nullptr;
    }

    u32 mask = header->capacity - 1;
    u64 hash = map_hash_key(vm, key_src, header);
    u32 pos = static_cast<u32>(hash) & mask;
    u8 dist = 1;

    while (true) {
        if (header->distances[pos] == 0) {
            *error = "Map key not found";
            return nullptr;
        }
        if (header->distances[pos] < dist) {
            *error = "Map key not found";
            return nullptr;
        }
        if (header->distances[pos] == dist &&
            map_keys_equal(vm, map_key_ptr(header, pos), key_src, header)) {
            return map_value_ptr(header, pos);
        }
        pos = (pos + 1) & mask;
        dist++;
    }
}

void map_insert(RoxyVM* vm, void* data, const u32* key_src, const u32* value_src) {
    MapHeader* header = get_map_header(data);
    u8 vsc = header->value_slot_count;

    if (header->capacity > 0 && header->length > 0) {
        u32 mask = header->capacity - 1;
        u64 hash = map_hash_key(vm, key_src, header);
        u32 pos = static_cast<u32>(hash) & mask;
        u8 dist = 1;

        while (true) {
            if (header->distances[pos] == 0) break;
            if (header->distances[pos] < dist) break;
            if (header->distances[pos] == dist &&
                map_keys_equal(vm, map_key_ptr(header, pos), key_src, header)) {
                memcpy(map_value_ptr(header, pos), value_src, sizeof(u32) * vsc);
                return;
            }
            pos = (pos + 1) & mask;
            dist++;
        }
    }

    if (header->capacity == 0 || (header->length + 1) > header->capacity * 4 / 5) {
        map_grow(vm, header);
    }

    map_insert_internal(vm, header, key_src, value_src);
    header->length++;
}

bool map_remove(RoxyVM* vm, void* data, const u32* key_src) {
    MapHeader* header = get_map_header(data);
    if (header->capacity == 0 || header->length == 0) return false;

    u8 ksc = header->key_slot_count;
    u8 vsc = header->value_slot_count;
    u32 mask = header->capacity - 1;
    u64 hash = map_hash_key(vm, key_src, header);
    u32 pos = static_cast<u32>(hash) & mask;
    u8 dist = 1;

    while (true) {
        if (header->distances[pos] == 0) return false;
        if (header->distances[pos] < dist) return false;
        if (header->distances[pos] == dist &&
            map_keys_equal(vm, map_key_ptr(header, pos), key_src, header)) {
            break;
        }
        pos = (pos + 1) & mask;
        dist++;
    }

    header->length--;
    while (true) {
        u32 next = (pos + 1) & mask;
        if (header->distances[next] <= 1) {
            header->distances[pos] = 0;
            memset(map_key_ptr(header, pos), 0, sizeof(u32) * ksc);
            memset(map_value_ptr(header, pos), 0, sizeof(u32) * vsc);
            return true;
        }
        header->distances[pos] = header->distances[next] - 1;
        memcpy(map_key_ptr(header, pos), map_key_ptr(header, next), sizeof(u32) * ksc);
        memcpy(map_value_ptr(header, pos), map_value_ptr(header, next), sizeof(u32) * vsc);
        pos = next;
    }
}

void map_clear(void* data) {
    MapHeader* header = get_map_header(data);
    header->length = 0;
    if (header->capacity > 0) {
        memset(header->distances, 0, sizeof(u8) * header->capacity);
        memset(header->keys, 0,
               sizeof(u32) * static_cast<size_t>(header->capacity) * header->key_slot_count);
        memset(header->values, 0,
               sizeof(u32) * static_cast<size_t>(header->capacity) * header->value_slot_count);
    }
}

void* map_keys(RoxyVM* vm, void* data) {
    const MapHeader* header = get_map_header(data);
    // Mirror the key layout into the produced List<K>: struct keys produce a
    // non-inline list of those structs; primitive keys produce an inline list.
    void* lst = list_alloc(vm, header->length, header->key_slot_count, header->key_is_inline);
    if (!lst) return nullptr;

    for (u32 i = 0; i < header->capacity; i++) {
        if (header->distances[i] != 0) {
            list_push_slots(lst, map_key_ptr(header, i));
        }
    }
    return lst;
}

void* map_values(RoxyVM* vm, void* data) {
    const MapHeader* header = get_map_header(data);
    void* lst = list_alloc(vm, header->length, header->value_slot_count, header->value_is_inline);
    if (!lst) return nullptr;

    for (u32 i = 0; i < header->capacity; i++) {
        if (header->distances[i] != 0) {
            list_push_slots(lst, map_value_ptr(header, i));
        }
    }
    return lst;
}

}
