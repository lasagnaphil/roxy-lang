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

// Stored in object data after roxy_object_header
// Layout: [roxy_object_header][roxy_list_header]
// Elements stored in a separate malloc'd buffer
typedef struct {
    uint32_t length;
    uint32_t capacity;
    uint64_t* elements;  // Separate malloc'd buffer
} roxy_list_header;

// ===== List Operations =====

void* roxy_list_alloc(void);
void roxy_list_init(void* self, int32_t capacity);
void roxy_list_delete(void* self);
int32_t roxy_list_len(void* self);
int32_t roxy_list_cap(void* self);
void roxy_list_push(void* self, uint64_t value);
uint64_t roxy_list_pop(void* self);
uint64_t roxy_list_get(void* self, int32_t index);
void roxy_list_set(void* self, int32_t index, uint64_t value);
void* roxy_list_copy(void* src);

// ===== Map Key Kind =====

#define ROXY_MAP_KEY_INTEGER  0
#define ROXY_MAP_KEY_FLOAT32  1
#define ROXY_MAP_KEY_FLOAT64  2
#define ROXY_MAP_KEY_STRING   3

// ===== Map Header =====

// Stored in object data after roxy_object_header
// Layout: [roxy_object_header][roxy_map_header]
// Separate malloc'd arrays for distances, keys, values
typedef struct {
    uint32_t length;          // Number of live entries
    uint32_t capacity;        // Number of buckets (power of 2, or 0)
    uint8_t key_kind;         // ROXY_MAP_KEY_* dispatch tag
    uint8_t _pad[3];
    uint8_t* distances;       // Per-bucket Robin Hood distance+1 (0 = empty)
    uint64_t* keys;
    uint64_t* values;
} roxy_map_header;

// ===== Map Operations =====

void* roxy_map_alloc(void);
void roxy_map_init(void* self, int32_t key_kind, int32_t capacity);
void roxy_map_delete(void* self);
int32_t roxy_map_len(void* self);
bool roxy_map_contains(void* self, uint64_t key);
uint64_t roxy_map_get(void* self, uint64_t key);
void roxy_map_insert(void* self, uint64_t key, uint64_t value);
bool roxy_map_remove(void* self, uint64_t key);
void roxy_map_clear(void* self);
void* roxy_map_keys(void* self);
void* roxy_map_values(void* self);
void* roxy_map_copy(void* src);

// Map index operators (same as get/insert but different native name)
uint64_t roxy_map_index(void* self, uint64_t key);
void roxy_map_index_mut(void* self, uint64_t key, uint64_t value);

// Internal map iteration (used by generated code for noncopyable element cleanup)
int32_t roxy_map_iter_capacity(void* self);
int32_t roxy_map_iter_next_occupied(void* self, int32_t idx);
uint64_t roxy_map_iter_key_at(void* self, int32_t idx);
uint64_t roxy_map_iter_value_at(void* self, int32_t idx);

// ===== Hash Functions =====

int64_t roxy_bool_hash(bool val);
int64_t roxy_i8_hash(int8_t val);
int64_t roxy_i16_hash(int16_t val);
int64_t roxy_i32_hash(int32_t val);
int64_t roxy_i64_hash(int64_t val);
int64_t roxy_u8_hash(uint8_t val);
int64_t roxy_u16_hash(uint16_t val);
int64_t roxy_u32_hash(uint32_t val);
int64_t roxy_u64_hash(uint64_t val);
int64_t roxy_f32_hash(float val);
int64_t roxy_f64_hash(double val);
int64_t roxy_string_hash(void* val);

#ifdef __cplusplus
}
#endif
