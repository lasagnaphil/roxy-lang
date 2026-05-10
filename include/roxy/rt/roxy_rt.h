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

// ===== Runtime Context =====
//
// `roxy_ctx` carries the per-call-site state native runtime functions need —
// allocator handle, exception bookkeeping, and an embedder-defined `user_data`
// pointer (typically a game engine handle). It is accessed via thread-local
// storage so generated AOT code and runtime helpers don't need to thread a
// `roxy_ctx*` parameter through every call.
//
// In VM mode `roxy_ctx` is the first member of `RoxyVM`, and the interpreter
// calls `roxy_set_ctx(&vm->ctx)` before entering Roxy code. In AOT mode the
// generated `main()` (or the embedder, via `roxy::ScopedContext`) does the
// same. The `allocator` slot is a function-pointer vtable (see
// `roxy_allocator` below); `exception_state` and `user_data` are
// embedder-defined `void*` placeholders today.
struct roxy_allocator;
typedef struct roxy_ctx {
    struct roxy_allocator* allocator;
    void* exception_state;
    void* user_data;
} roxy_ctx;

// ===== Allocator vtable =====
//
// Function-pointer table that lets the runtime use any underlying allocator
// (slab in VM mode, slab again in AOT mode via the global `roxy_rt_init`,
// or `malloc` as a defensive fallback for code that runs before any context
// is active). `userdata` is opaque allocator state — for the slab impl it's
// a `SlabAllocator*`; for malloc it's unused.
typedef struct roxy_allocator {
    // Allocate `total_size` bytes (including the `roxy_object_header`); fill
    // `*out_generation` with a fresh random non-zero generation. Returns the
    // raw header pointer (caller writes header fields), or NULL on failure.
    void* (*alloc)(void* userdata, uint32_t total_size, uint64_t* out_generation);

    // Free a previously-allocated header. Implementations must tombstone the
    // header's `weak_generation` to 0 before reclaiming so stale weak refs
    // see "dead".
    void (*free)(void* userdata, void* header_ptr);

    // Optional: returns true if `ptr` is owned by this allocator. The slab
    // impl answers precisely via a sorted index; the malloc impl
    // unconditionally returns true. Used by the VM's ASSERT_HEAP closure-
    // capture validation; degraded to a tautology in malloc mode.
    bool (*owns)(void* userdata, void* ptr);

    void* userdata;
} roxy_allocator;

// Default malloc-based allocator. Used when no `roxy_ctx` is active or when
// `ctx->allocator` is null. Generations are produced from a thread-local
// xorshift64; weak-ref soundness is best-effort because the OS may reuse
// freed addresses (unlike the slab path which keeps freed memory mapped).
extern roxy_allocator roxy_malloc_allocator;

// Zero-initialize a context. Safe to call again after `roxy_ctx_destroy`.
void roxy_ctx_init(roxy_ctx* ctx);

// Tear down any owned state. Currently a no-op; reserved for future
// allocator/exception-state cleanup.
void roxy_ctx_destroy(roxy_ctx* ctx);

// Replace the current thread's active context. Pass `nullptr` to clear it.
void roxy_set_ctx(roxy_ctx* ctx);

// Retrieve the current thread's active context, or `nullptr` if none is set.
roxy_ctx* roxy_get_ctx(void);

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

// Read the weak generation field from an object header.
// Returns 0 if data is null or already tombstoned.
uint64_t roxy_weak_generation(void* data);

#ifdef __cplusplus
} // extern "C"

// ===== C++ RAII Wrappers =====
//
// Embedder-facing C++ types mirroring Roxy's reference categories. They wrap
// `roxy_alloc`/`roxy_free`/`roxy_ref_inc`/`roxy_ref_dec`/`roxy_weak_*` so
// generated headers can hand the embedder typed factory functions instead of
// raw pointers.

#include <cassert>
#include <cstddef>
#include <cstring>
#include <utility>

namespace roxy {

// RAII guard for thread-local context activation.
//
// Saves the previous context on construction, swaps in `ctx`, and restores the
// previous context on destruction. Use this in embedder code to scope a Roxy
// call sequence:
//
//     roxy_ctx ctx;
//     roxy_ctx_init(&ctx);
//     ctx.user_data = &my_game_engine;
//     {
//         roxy::ScopedContext guard(&ctx);
//         update_player(entity, dt);
//     }
//     roxy_ctx_destroy(&ctx);
class ScopedContext {
public:
    explicit ScopedContext(roxy_ctx* ctx) : m_prev(roxy_get_ctx()) {
        roxy_set_ctx(ctx);
    }
    ~ScopedContext() { roxy_set_ctx(m_prev); }

    ScopedContext(const ScopedContext&) = delete;
    ScopedContext& operator=(const ScopedContext&) = delete;

private:
    roxy_ctx* m_prev;
};

using destructor_fn = void (*)(void*);

// Unique ownership: owns one allocation, calls user destructor + roxy_free
// on scope exit. Move-only, no copy.
template <typename T>
class uniq {
public:
    uniq() : m_ptr(nullptr), m_destructor(nullptr) {}

    uniq(T* ptr, void (*dtor)(T*))
        : m_ptr(ptr), m_destructor(reinterpret_cast<destructor_fn>(dtor)) {}

    ~uniq() { reset(); }

    uniq(uniq&& other) noexcept
        : m_ptr(other.m_ptr), m_destructor(other.m_destructor) {
        other.m_ptr = nullptr;
    }

    uniq& operator=(uniq&& other) noexcept {
        if (this != &other) {
            reset();
            m_ptr = other.m_ptr;
            m_destructor = other.m_destructor;
            other.m_ptr = nullptr;
        }
        return *this;
    }

    uniq(const uniq&) = delete;
    uniq& operator=(const uniq&) = delete;

    T* get() const { return m_ptr; }
    T* operator->() const { return m_ptr; }
    T& operator*() const { return *m_ptr; }
    explicit operator bool() const { return m_ptr != nullptr; }

    // Release ownership without running the destructor.
    T* release() {
        T* p = m_ptr;
        m_ptr = nullptr;
        return p;
    }

    void reset() {
        if (m_ptr) {
            if (m_destructor) m_destructor(m_ptr);
            roxy_free(m_ptr);
            m_ptr = nullptr;
        }
    }

private:
    T* m_ptr;
    destructor_fn m_destructor;
};

// Shared reference: increments ref count on copy, decrements on destruction.
// Last reference triggers `roxy_ref_dec` which frees if there is no outstanding
// `uniq` owner.
template <typename T>
class ref {
public:
    ref() : m_ptr(nullptr) {}

    explicit ref(T* ptr) : m_ptr(ptr) {
        if (m_ptr) roxy_ref_inc(m_ptr);
    }

    ~ref() {
        if (m_ptr) roxy_ref_dec(m_ptr);
    }

    ref(const ref& other) : m_ptr(other.m_ptr) {
        if (m_ptr) roxy_ref_inc(m_ptr);
    }

    ref& operator=(const ref& other) {
        if (this != &other) {
            if (m_ptr) roxy_ref_dec(m_ptr);
            m_ptr = other.m_ptr;
            if (m_ptr) roxy_ref_inc(m_ptr);
        }
        return *this;
    }

    ref(ref&& other) noexcept : m_ptr(other.m_ptr) {
        other.m_ptr = nullptr;
    }

    ref& operator=(ref&& other) noexcept {
        if (this != &other) {
            if (m_ptr) roxy_ref_dec(m_ptr);
            m_ptr = other.m_ptr;
            other.m_ptr = nullptr;
        }
        return *this;
    }

    T* get() const { return m_ptr; }
    T* operator->() const { return m_ptr; }
    T& operator*() const { return *m_ptr; }
    explicit operator bool() const { return m_ptr != nullptr; }

private:
    T* m_ptr;
};

// Weak reference: non-owning, generation-checked. `valid()` returns false once
// the referenced object has been freed.
template <typename T>
class weak {
public:
    weak() : m_ptr(nullptr), m_generation(0) {}

    explicit weak(T* ptr)
        : m_ptr(ptr)
        , m_generation(ptr ? roxy_weak_generation(ptr) : 0) {}

    bool valid() const {
        return m_ptr != nullptr && roxy_weak_valid(m_ptr, m_generation);
    }

    T* lock() const {
        assert(valid() && "weak reference is dangling");
        return m_ptr;
    }

    T* lock_or_null() const {
        return valid() ? m_ptr : nullptr;
    }

    weak(const weak&) = default;
    weak& operator=(const weak&) = default;
    weak(weak&&) noexcept = default;
    weak& operator=(weak&&) noexcept = default;

private:
    T* m_ptr;
    uint64_t m_generation;
};

// ===== Container / String Wrappers =====
//
// Thin non-owning facades over the type-erased roxy_rt C functions. These
// match the existing `rx::RoxyString`/`rx::RoxyList`/`rx::RoxyMap` shape but
// drop the VM dependency, so generated AOT code and embedder C++ can use them
// uniformly.

class String {
public:
    String() : m_data(nullptr) {}
    explicit String(void* data) : m_data(data) {}

    static String alloc(const char* data, uint32_t length) {
        return String(roxy_string_from_literal(data, length));
    }
    static String alloc(const char* data) {
        return alloc(data, static_cast<uint32_t>(std::strlen(data)));
    }

    int32_t length() const { return roxy_string_len(m_data); }
    const char* c_str() const { return roxy_string_chars(m_data); }

    bool equals(String other) const {
        return roxy_string_eq(m_data, other.m_data);
    }
    String concat(String other) const {
        return String(roxy_string_concat(m_data, other.m_data));
    }

    bool is_valid() const { return m_data != nullptr; }
    void* data() const { return m_data; }

private:
    void* m_data;
};

template <typename T>
class List {
public:
    static constexpr int32_t slot_count =
        static_cast<int32_t>((sizeof(T) + sizeof(uint32_t) - 1) / sizeof(uint32_t));

    static List<T> alloc(int32_t capacity = 0) {
        void* data = roxy_list_alloc(slot_count, /*element_is_inline=*/1);
        if (data) roxy_list_init(data, capacity);
        return List<T>(data);
    }

    List() : m_data(nullptr) {}
    explicit List(void* data) : m_data(data) {}

    void push(const T& value) {
        roxy_list_push(m_data, &value);
    }
    T pop() {
        T value;
        std::memcpy(&value, roxy_list_pop(m_data), sizeof(T));
        return value;
    }
    T get(int32_t index) const {
        T value;
        std::memcpy(&value, roxy_list_get(m_data, index), sizeof(T));
        return value;
    }
    void set(int32_t index, const T& value) {
        roxy_list_set(m_data, index, &value);
    }

    int32_t len() const { return roxy_list_len(m_data); }
    int32_t cap() const { return roxy_list_cap(m_data); }

    bool is_valid() const { return m_data != nullptr; }
    void* data() const { return m_data; }

private:
    void* m_data;
};

template <typename K, typename V>
class Map {
public:
    static constexpr int32_t key_slot_count =
        static_cast<int32_t>((sizeof(K) + sizeof(uint32_t) - 1) / sizeof(uint32_t));
    static constexpr int32_t value_slot_count =
        static_cast<int32_t>((sizeof(V) + sizeof(uint32_t) - 1) / sizeof(uint32_t));

    static Map<K, V> alloc(int32_t key_kind,
                           int32_t capacity = 0,
                           roxy_map_hash_fn hash_fn = nullptr,
                           roxy_map_eq_fn eq_fn = nullptr) {
        void* data = roxy_map_alloc(key_slot_count, /*key_is_inline=*/1,
                                    value_slot_count, /*value_is_inline=*/1,
                                    hash_fn, eq_fn);
        if (data) roxy_map_init(data, key_kind, capacity);
        return Map<K, V>(data);
    }

    Map() : m_data(nullptr) {}
    explicit Map(void* data) : m_data(data) {}

    void insert(const K& key, const V& value) {
        roxy_map_insert(m_data, &key, &value);
    }
    V get(const K& key) const {
        V value;
        std::memcpy(&value, roxy_map_get(m_data, &key), sizeof(V));
        return value;
    }
    bool contains(const K& key) const {
        return roxy_map_contains(m_data, &key);
    }
    bool remove(const K& key) {
        return roxy_map_remove(m_data, &key);
    }
    void clear() { roxy_map_clear(m_data); }

    List<K> keys() const { return List<K>(roxy_map_keys(m_data)); }
    List<V> values() const { return List<V>(roxy_map_values(m_data)); }

    int32_t len() const { return roxy_map_len(m_data); }

    bool is_valid() const { return m_data != nullptr; }
    void* data() const { return m_data; }

private:
    void* m_data;
};

} // namespace roxy

#endif // __cplusplus
