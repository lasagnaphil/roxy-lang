#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ===== Object Header =====

// Precedes all heap-allocated objects in memory
// Layout: [roxy_object_header][object data...]
typedef struct {
    uint64_t weak_generation;   // Random generation for weak refs; 0 = dead/tombstoned
    uint32_t ref_count;         // Reference count for ref borrows
    uint32_t type_id;           // Type identifier for runtime type info
} roxy_object_header;

// ===== Builtin type IDs =====
#define ROXY_TYPEID_STRING  1
#define ROXY_TYPEID_LIST    2
#define ROXY_TYPEID_MAP     3
// User-defined struct type IDs start at 100

// ===== Allocation =====

// Allocate a new object with the given data size and type ID.
// Returns pointer to object data (after the header).
void* roxy_alloc(uint32_t data_size, uint32_t type_id);

// Free an object. Tombstones weak_generation (sets to 0), then frees memory.
void roxy_free(void* data);

// Get the object header from a data pointer.
roxy_object_header* roxy_get_header(void* data);

// ===== Reference Counting =====

void roxy_ref_inc(void* data);
void roxy_ref_dec(void* data);

// ===== Weak References =====

typedef struct {
    void* ptr;
    uint64_t generation;
} roxy_weak;

roxy_weak roxy_weak_create(void* data);
bool roxy_weak_valid(void* ptr, uint64_t generation);

// ===== String Header =====

// Stored in object data after roxy_object_header
// Layout: [roxy_object_header][roxy_string_header][char data + null terminator]
typedef struct {
    uint32_t length;    // String length (excluding null terminator)
    uint32_t capacity;  // Allocated capacity (including null terminator)
} roxy_string_header;

// ===== String Operations =====

// Allocate a new string from a C literal (copies data).
void* roxy_string_from_literal(const char* data, uint32_t length);

// Get pointer to character data (after string header).
char* roxy_string_chars(void* s);

// Get string length.
int32_t roxy_string_len(void* s);

// Print a string followed by newline.
void roxy_print(void* s);

// Concatenate two strings. Returns a new string.
void* roxy_string_concat(void* a, void* b);

// String equality / inequality.
bool roxy_string_eq(void* a, void* b);
bool roxy_string_ne(void* a, void* b);

// ===== to_string conversions =====

void* roxy_bool_to_string(bool val);
void* roxy_i32_to_string(int32_t val);
void* roxy_i64_to_string(int64_t val);
void* roxy_f32_to_string(float val);
void* roxy_f64_to_string(double val);
void* roxy_string_to_string(void* val);

// ===== List Header =====

// Stored in object data after roxy_object_header.
// Layout: [roxy_object_header][roxy_list_header]
// Elements live in a separate malloc'd buffer of `capacity * element_slot_count`
// u32 slots; element[i] starts at &elements[i * element_slot_count].
typedef struct {
    uint32_t length;
    uint32_t capacity;
    uint32_t element_slot_count;  // u32 slots per element (1, 2, or N for structs)
    uint8_t  element_is_inline;   // 1 = primitive packed in slots; 0 = struct (caller provides ptr)
    uint8_t  _pad[3];
    uint32_t* elements;
} roxy_list_header;

// ===== List Operations =====
//
// All element reads/writes use byte-pointer values: callers pass a pointer to
// `element_slot_count * 4` bytes. `*_get` / `*_pop` return pointers into the
// list's backing storage (valid until the next mutation).

void* roxy_list_alloc(int32_t element_slot_count, int32_t element_is_inline);
void  roxy_list_init(void* self, int32_t capacity);
void  roxy_list_delete(void* self);
int32_t roxy_list_len(void* self);
int32_t roxy_list_cap(void* self);
void  roxy_list_push(void* self, const void* value_src);
void* roxy_list_pop(void* self);
void* roxy_list_get(void* self, int32_t index);
void  roxy_list_set(void* self, int32_t index, const void* value_src);
void* roxy_list_copy(void* src);

// ===== Map Key Kind =====

#define ROXY_MAP_KEY_INTEGER  0
#define ROXY_MAP_KEY_FLOAT32  1
#define ROXY_MAP_KEY_FLOAT64  2
#define ROXY_MAP_KEY_STRING   3
#define ROXY_MAP_KEY_STRUCT   4

// ===== Map Header =====

// Function-pointer types for custom Hash/Eq dispatch on struct keys.
// hash_fn takes a pointer to key bytes (key_slot_count*4 bytes wide) and
// returns a u64 hash. eq_fn takes two such pointers and returns bool.
// nullptr = no custom dispatch; runtime falls back to bytewise.
typedef uint64_t (*roxy_map_hash_fn)(const void* key_src);
typedef bool     (*roxy_map_eq_fn)(const void* a, const void* b);

// Stored in object data after roxy_object_header.
// Layout: [roxy_object_header][roxy_map_header]
// Bucket arrays for distances/keys/values are separate malloc'd buffers. Both
// keys and values now live in variable-sized u32-slot arrays sized
// `capacity * key_slot_count` and `capacity * value_slot_count` respectively,
// so struct keys (and struct values) live inline.
typedef struct {
    uint32_t length;            // Number of live entries
    uint32_t capacity;          // Number of buckets (power of 2, or 0)
    uint8_t  key_kind;          // ROXY_MAP_KEY_* dispatch tag
    uint8_t  key_slot_count;    // u32 slots per key (2 for primitives, N for structs)
    uint8_t  key_is_inline;     // 1 = primitive (value packed into slots); 0 = struct (caller passes ptr)
    uint8_t  value_slot_count;  // u32 slots per value
    uint8_t  value_is_inline;   // 1 = primitive value; 0 = struct (caller provides ptr)
    uint8_t  _pad[3];
    roxy_map_hash_fn hash_fn;   // nullptr = bytewise hash (Struct key kind only)
    roxy_map_eq_fn   eq_fn;     // nullptr = bytewise eq (Struct key kind only)
    uint8_t*  distances;        // Per-bucket Robin Hood distance+1 (0 = empty)
    uint32_t* keys;             // capacity * key_slot_count u32 slots
    uint32_t* values;           // capacity * value_slot_count u32 slots
} roxy_map_header;

// ===== Map Operations =====
//
// Both key and value reads/writes use byte-pointer arguments: callers pass a
// pointer to `key_slot_count * 4` / `value_slot_count * 4` bytes. `*_get` /
// `*_index` return pointers into the map's backing storage (valid until the
// next insert/remove).

void* roxy_map_alloc(int32_t key_slot_count, int32_t key_is_inline,
                     int32_t value_slot_count, int32_t value_is_inline,
                     roxy_map_hash_fn hash_fn, roxy_map_eq_fn eq_fn);
void  roxy_map_init(void* self, int32_t key_kind, int32_t capacity);
void  roxy_map_delete(void* self);
int32_t roxy_map_len(void* self);
bool  roxy_map_contains(void* self, const void* key_src);
void* roxy_map_get(void* self, const void* key_src);
void  roxy_map_insert(void* self, const void* key_src, const void* value_src);
bool  roxy_map_remove(void* self, const void* key_src);
void  roxy_map_clear(void* self);
void* roxy_map_keys(void* self);
void* roxy_map_values(void* self);
void* roxy_map_copy(void* src);

// Map index operators — same as get/insert under a different native name.
void* roxy_map_index(void* self, const void* key_src);
void  roxy_map_index_mut(void* self, const void* key_src, const void* value_src);

// Internal map iteration (used by generated code for noncopyable element cleanup)
int32_t roxy_map_iter_capacity(void* self);
int32_t roxy_map_iter_next_occupied(void* self, int32_t idx);
uint64_t roxy_map_iter_key_at(void* self, int32_t idx);
uint64_t roxy_map_iter_value_at(void* self, int32_t idx);

// ===== Hash Functions =====

uint64_t roxy_bool_hash(bool val);
uint64_t roxy_i8_hash(int8_t val);
uint64_t roxy_i16_hash(int16_t val);
uint64_t roxy_i32_hash(int32_t val);
uint64_t roxy_i64_hash(int64_t val);
uint64_t roxy_u8_hash(uint8_t val);
uint64_t roxy_u16_hash(uint16_t val);
uint64_t roxy_u32_hash(uint32_t val);
uint64_t roxy_u64_hash(uint64_t val);
uint64_t roxy_f32_hash(float val);
uint64_t roxy_f64_hash(double val);
uint64_t roxy_string_hash(void* val);

#ifdef __cplusplus
}
#endif
