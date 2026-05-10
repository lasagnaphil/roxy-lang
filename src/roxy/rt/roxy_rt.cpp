#include "roxy/rt/roxy_rt.h"
#include "roxy/rt/slab_allocator.hpp"

#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cassert>
#include <new>

#define XXH_INLINE_ALL
#include "roxy/core/xxhash.h"

// ===== Runtime Context =====

// Thread-local pointer to the currently-active context. Each native VM thread
// or AOT entry point sets this before calling into Roxy code. Defaulted to
// nullptr — `roxy_get_ctx()` returns nullptr if nothing has been set.
static thread_local roxy_ctx* tls_current_ctx = nullptr;

void roxy_ctx_init(roxy_ctx* ctx) {
    if (!ctx) return;
    // Pick up the runtime's default allocator at init time. Before the first
    // `roxy_rt_init` this is `&roxy_malloc_allocator`; after, it's the global
    // slab vtable. Embedders may overwrite `ctx->allocator` afterwards (e.g.
    // VM mode points it at a per-VM slab in `vm_init`).
    ctx->allocator = roxy_rt_default_allocator();
    ctx->string_intern = nullptr;
    ctx->exception_state = nullptr;
    ctx->user_data = nullptr;
}

void roxy_ctx_destroy(roxy_ctx* ctx) {
    // No owned state to release yet. Defined so the lifecycle pairs with
    // `roxy_ctx_init` and embedder/host code can call it unconditionally.
    (void)ctx;
}

void roxy_set_ctx(roxy_ctx* ctx) {
    tls_current_ctx = ctx;
}

roxy_ctx* roxy_get_ctx(void) {
    return tls_current_ctx;
}

// ===== Random generation for weak references =====

static uint64_t roxy_random_generation() {
    // Simple xorshift64 PRNG for generating unique weak_generation values
    static uint64_t state = 0x12345678deadbeefULL;
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    // Ensure non-zero (0 means dead/tombstoned)
    return state ? state : 1;
}

// ===== Default malloc-based allocator =====
//
// Vtable-shaped wrappers around the existing malloc/free path. These keep
// behaviour identical to the pre-vtable `roxy_alloc`/`roxy_free` so Phase 1
// can land without changing any allocation outcomes (Phase 2 wires the
// vtable into `roxy_alloc` and starts dispatching through it).

static void* roxy_malloc_alloc_fn(void* userdata, uint32_t total_size,
                                  uint64_t* out_generation) {
    (void)userdata;
    void* raw = malloc(total_size);
    if (!raw) {
        if (out_generation) *out_generation = 0;
        return nullptr;
    }
    if (out_generation) *out_generation = roxy_random_generation();
    return raw;
}

static void roxy_malloc_free_fn(void* userdata, void* header_ptr) {
    (void)userdata;
    if (!header_ptr) return;
    auto* hdr = static_cast<roxy_object_header*>(header_ptr);
    hdr->weak_generation = 0;  // Tombstone before freeing.
    free(header_ptr);
}

static bool roxy_malloc_owns_fn(void* userdata, void* ptr) {
    (void)userdata;
    (void)ptr;
    // No fast way to query libc's heap. Return true so callers (e.g. the
    // VM's ASSERT_HEAP) treat any pointer as owned in malloc mode. This
    // degrades the assertion to a tautology — acceptable because
    // closure-capture validation has no analogue in AOT-compiled programs.
    return true;
}

roxy_allocator roxy_malloc_allocator = {
    /*alloc=*/    roxy_malloc_alloc_fn,
    /*free=*/     roxy_malloc_free_fn,
    /*owns=*/     roxy_malloc_owns_fn,
    /*userdata=*/ nullptr,
};

// ===== Process-wide slab (lazy, ref-counted) =====
//
// Used by AOT-compiled programs when `roxy_rt_init` is called. Single-
// threaded; a multi-threaded embedder must manage allocator lifetime per
// ctx instead of relying on this global.

static rx::SlabAllocator* g_global_slab = nullptr;
static roxy_allocator g_global_slab_vtable = {nullptr, nullptr, nullptr, nullptr};
static int g_rt_init_refcount = 0;

void roxy_rt_init(void) {
    if (g_rt_init_refcount++ == 0) {
        g_global_slab = new (std::nothrow) rx::SlabAllocator();
        if (g_global_slab && g_global_slab->init()) {
            g_global_slab_vtable = rx::make_slab_allocator_vtable(g_global_slab);
        } else {
            // Init failed — fall back to malloc by leaving the vtable empty;
            // `roxy_rt_default_allocator` will return the malloc one when
            // refcount > 0 but the slab pointer is null.
            delete g_global_slab;
            g_global_slab = nullptr;
        }
    }
}

void roxy_rt_shutdown(void) {
    if (g_rt_init_refcount == 0) return;
    if (--g_rt_init_refcount == 0) {
        if (g_global_slab) {
            g_global_slab->shutdown();
            delete g_global_slab;
            g_global_slab = nullptr;
        }
        g_global_slab_vtable = roxy_allocator{nullptr, nullptr, nullptr, nullptr};
    }
}

roxy_allocator* roxy_rt_default_allocator(void) {
    if (g_rt_init_refcount > 0 && g_global_slab) {
        return &g_global_slab_vtable;
    }
    return &roxy_malloc_allocator;
}

// ===== Allocation =====

static inline roxy_allocator* current_allocator() {
    roxy_ctx* ctx = roxy_get_ctx();
    if (ctx && ctx->allocator) return ctx->allocator;
    return &roxy_malloc_allocator;
}

void* roxy_alloc(uint32_t data_size, uint32_t type_id) {
    roxy_allocator* alloc = current_allocator();
    uint64_t generation = 0;
    void* raw = alloc->alloc(alloc->userdata,
                             sizeof(roxy_object_header) + data_size,
                             &generation);
    if (!raw) return nullptr;

    auto* header = static_cast<roxy_object_header*>(raw);
    header->weak_generation = generation;
    header->ref_count = 0;
    header->type_id = type_id;

    void* data = static_cast<uint8_t*>(raw) + sizeof(roxy_object_header);
    memset(data, 0, data_size);
    return data;
}

void roxy_free(void* data) {
    if (!data) return;
    roxy_allocator* alloc = current_allocator();
    auto* header = roxy_get_header(data);
    // Allocator's free contract: tombstone weak_generation before freeing.
    // Both the slab and malloc impls do this.
    alloc->free(alloc->userdata, header);
}

roxy_object_header* roxy_get_header(void* data) {
    return reinterpret_cast<roxy_object_header*>(
        reinterpret_cast<uint8_t*>(data) - sizeof(roxy_object_header));
}

// ===== Reference Counting =====

void roxy_ref_inc(void* data) {
    if (!data) return;
    roxy_get_header(data)->ref_count++;
}

void roxy_ref_dec(void* data) {
    if (!data) return;
    auto* header = roxy_get_header(data);
    if (header->ref_count > 0) {
        header->ref_count--;
    }
}

// ===== Weak References =====

roxy_weak roxy_weak_create(void* data) {
    roxy_weak w;
    w.ptr = data;
    w.generation = 0;
    if (data) {
        w.generation = roxy_get_header(data)->weak_generation;
    }
    return w;
}

bool roxy_weak_valid(void* ptr, uint64_t generation) {
    if (!ptr || generation == 0) return false;
    return roxy_get_header(ptr)->weak_generation == generation;
}

uint64_t roxy_weak_generation(void* data) {
    if (!data) return 0;
    return roxy_get_header(data)->weak_generation;
}

// ===== String Operations =====

static roxy_string_header* string_hdr(void* s) {
    return static_cast<roxy_string_header*>(s);
}

void* roxy_string_from_literal(const char* data, uint32_t length) {
    // Probe the active context's intern table. Hits dedup the hot cases —
    // LOAD_CONST of the same literal across many call sites, f-string
    // numeric conversions, slices that recur. Strings are immutable
    // copyable values in Roxy, so sharing a pointer is safe.
    roxy_ctx* ctx = roxy_get_ctx();
    void* intern = ctx ? ctx->string_intern : nullptr;
    if (intern && data && length > 0) {
        if (void* existing = roxy_string_intern_lookup(intern, data, length)) {
            return existing;
        }
    }

    uint32_t data_size = static_cast<uint32_t>(sizeof(roxy_string_header)) + length + 1;
    void* s = roxy_alloc(data_size, ROXY_TYPEID_STRING);
    if (!s) return nullptr;

    auto* hdr = string_hdr(s);
    hdr->length = length;

    char* chars = reinterpret_cast<char*>(reinterpret_cast<uint8_t*>(s) + sizeof(roxy_string_header));
    if (length > 0) {
        memcpy(chars, data, length);
    }
    chars[length] = '\0';

    // Cache a 32-bit hash over the character bytes so `Map<string, V>`
    // lookups (and `roxy_string_hash`) don't walk the string on every op.
    // Low 32 bits of XXH3_64 — matches the VM's vm/string.cpp behaviour.
    hdr->hash = static_cast<uint32_t>(XXH3_64bits(chars, length));

    // Register the new string in the intern table. The key's char range
    // is the object's own chars (stable for the object's lifetime).
    if (intern && data && length > 0) {
        roxy_string_intern_insert(intern, chars, length, s);
    }
    return s;
}

char* roxy_string_chars(void* s) {
    return reinterpret_cast<char*>(
        reinterpret_cast<uint8_t*>(s) + sizeof(roxy_string_header));
}

int32_t roxy_string_len(void* s) {
    return static_cast<int32_t>(string_hdr(s)->length);
}

void roxy_print(void* s) {
    if (!s) {
        printf("nil\n");
        return;
    }
    printf("%s\n", roxy_string_chars(s));
}

void* roxy_string_concat(void* a, void* b) {
    uint32_t len_a = string_hdr(a)->length;
    uint32_t len_b = string_hdr(b)->length;
    uint32_t total = len_a + len_b;

    // Build the concatenated bytes in a temp buffer so `roxy_string_from_literal`
    // can dedup against the intern table. Short cases use the stack; longer
    // ones take a one-shot malloc.
    char stack_buf[256];
    char* buf = (total < sizeof(stack_buf))
        ? stack_buf
        : static_cast<char*>(malloc(total + 1));
    if (!buf) return nullptr;

    memcpy(buf, roxy_string_chars(a), len_a);
    memcpy(buf + len_a, roxy_string_chars(b), len_b);
    buf[total] = '\0';

    void* result = roxy_string_from_literal(buf, total);

    if (buf != stack_buf) free(buf);
    return result;
}

bool roxy_string_eq(void* a, void* b) {
    if (a == b) return true;
    uint32_t len_a = string_hdr(a)->length;
    uint32_t len_b = string_hdr(b)->length;
    if (len_a != len_b) return false;
    return memcmp(roxy_string_chars(a), roxy_string_chars(b), len_a) == 0;
}

bool roxy_string_ne(void* a, void* b) {
    return !roxy_string_eq(a, b);
}

// ===== to_string conversions =====

void* roxy_bool_to_string(bool val) {
    return val ? roxy_string_from_literal("true", 4) : roxy_string_from_literal("false", 5);
}

void* roxy_i32_to_string(int32_t val) {
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%d", val);
    return roxy_string_from_literal(buf, static_cast<uint32_t>(len));
}

void* roxy_i64_to_string(int64_t val) {
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(val));
    return roxy_string_from_literal(buf, static_cast<uint32_t>(len));
}

void* roxy_f32_to_string(float val) {
    char buf[48];
    int len = snprintf(buf, sizeof(buf), "%g", static_cast<double>(val));
    return roxy_string_from_literal(buf, static_cast<uint32_t>(len));
}

void* roxy_f64_to_string(double val) {
    char buf[48];
    int len = snprintf(buf, sizeof(buf), "%g", val);
    return roxy_string_from_literal(buf, static_cast<uint32_t>(len));
}

void* roxy_string_to_string(void* val) {
    return val;  // Identity — strings are already strings
}

// ===== List Operations =====

static inline uint32_t* list_element_ptr(roxy_list_header* hdr, uint32_t index) {
    return hdr->elements + static_cast<size_t>(index) * hdr->element_slot_count;
}

void* roxy_list_alloc(int32_t element_slot_count, int32_t element_is_inline) {
    void* data = roxy_alloc(sizeof(roxy_list_header), ROXY_TYPEID_LIST);
    if (!data) return nullptr;
    auto* hdr = static_cast<roxy_list_header*>(data);
    hdr->element_slot_count = element_slot_count > 0
        ? static_cast<uint32_t>(element_slot_count) : 2u;
    hdr->element_is_inline = element_is_inline != 0 ? 1 : 0;
    // length/capacity/elements already zero-initialised by roxy_alloc.
    return data;
}

void roxy_list_init(void* self, int32_t capacity) {
    auto* hdr = static_cast<roxy_list_header*>(self);
    hdr->length = 0;
    // Preserve element_slot_count / element_is_inline set by roxy_list_alloc.
    if (capacity > 0) {
        hdr->capacity = static_cast<uint32_t>(capacity);
        hdr->elements = static_cast<uint32_t*>(calloc(
            static_cast<size_t>(capacity) * hdr->element_slot_count,
            sizeof(uint32_t)));
    } else {
        hdr->capacity = 0;
        hdr->elements = nullptr;
    }
}

void roxy_list_delete(void* self) {
    auto* hdr = static_cast<roxy_list_header*>(self);
    free(hdr->elements);
    hdr->elements = nullptr;
    hdr->length = 0;
    hdr->capacity = 0;
}

int32_t roxy_list_len(void* self) {
    return static_cast<int32_t>(static_cast<roxy_list_header*>(self)->length);
}

int32_t roxy_list_cap(void* self) {
    return static_cast<int32_t>(static_cast<roxy_list_header*>(self)->capacity);
}

void roxy_list_push(void* self, const void* value_src) {
    auto* hdr = static_cast<roxy_list_header*>(self);
    uint32_t esc = hdr->element_slot_count;
    if (hdr->length >= hdr->capacity) {
        uint32_t new_cap = hdr->capacity == 0 ? 8 : hdr->capacity * 2;
        // malloc + memcpy + free rather than realloc — keeps the path uniform
        // for variable element widths and matches the VM's growth strategy.
        auto* new_elements = static_cast<uint32_t*>(malloc(
            sizeof(uint32_t) * esc * new_cap));
        if (!new_elements) return;
        if (hdr->elements) {
            memcpy(new_elements, hdr->elements, sizeof(uint32_t) * esc * hdr->length);
            free(hdr->elements);
        }
        hdr->elements = new_elements;
        hdr->capacity = new_cap;
    }
    memcpy(list_element_ptr(hdr, hdr->length), value_src, sizeof(uint32_t) * esc);
    hdr->length++;
}

void* roxy_list_pop(void* self) {
    auto* hdr = static_cast<roxy_list_header*>(self);
    assert(hdr->length > 0);
    hdr->length--;
    // Return a pointer to the now-out-of-bounds slot; bytes remain valid until
    // the next push (matches the VM's list_pop_ptr contract).
    return list_element_ptr(hdr, hdr->length);
}

void* roxy_list_get(void* self, int32_t index) {
    auto* hdr = static_cast<roxy_list_header*>(self);
    assert(index >= 0 && static_cast<uint32_t>(index) < hdr->length);
    return list_element_ptr(hdr, static_cast<uint32_t>(index));
}

void roxy_list_set(void* self, int32_t index, const void* value_src) {
    auto* hdr = static_cast<roxy_list_header*>(self);
    assert(index >= 0 && static_cast<uint32_t>(index) < hdr->length);
    memcpy(list_element_ptr(hdr, static_cast<uint32_t>(index)), value_src,
           sizeof(uint32_t) * hdr->element_slot_count);
}

void* roxy_list_copy(void* src) {
    if (!src) return nullptr;
    auto* src_hdr = static_cast<roxy_list_header*>(src);

    void* dst = roxy_list_alloc(static_cast<int32_t>(src_hdr->element_slot_count),
                                static_cast<int32_t>(src_hdr->element_is_inline));
    if (!dst) return nullptr;

    auto* dst_hdr = static_cast<roxy_list_header*>(dst);
    if (src_hdr->capacity > 0) {
        dst_hdr->length = src_hdr->length;
        dst_hdr->capacity = src_hdr->capacity;
        dst_hdr->elements = static_cast<uint32_t*>(malloc(
            sizeof(uint32_t) * src_hdr->element_slot_count * src_hdr->capacity));
        memcpy(dst_hdr->elements, src_hdr->elements,
               sizeof(uint32_t) * src_hdr->element_slot_count * src_hdr->length);
    }
    return dst;
}

// ===== Hash Functions =====

static uint64_t hash_splitmix64(uint64_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

static uint64_t hash_fnv1a(const char* data, uint32_t length) {
    uint64_t h = 14695981039346656037ULL;
    for (uint32_t i = 0; i < length; i++) {
        h ^= static_cast<uint64_t>(static_cast<uint8_t>(data[i]));
        h *= 1099511628211ULL;
    }
    return h;
}

uint64_t roxy_bool_hash(bool val) {
    return hash_splitmix64(val ? 1u : 0u);
}

uint64_t roxy_i8_hash(int8_t val) {
    return hash_splitmix64(static_cast<uint64_t>(val));
}

uint64_t roxy_i16_hash(int16_t val) {
    return hash_splitmix64(static_cast<uint64_t>(val));
}

uint64_t roxy_i32_hash(int32_t val) {
    return hash_splitmix64(static_cast<uint64_t>(val));
}

uint64_t roxy_i64_hash(int64_t val) {
    return hash_splitmix64(static_cast<uint64_t>(val));
}

uint64_t roxy_u8_hash(uint8_t val) {
    return hash_splitmix64(static_cast<uint64_t>(val));
}

uint64_t roxy_u16_hash(uint16_t val) {
    return hash_splitmix64(static_cast<uint64_t>(val));
}

uint64_t roxy_u32_hash(uint32_t val) {
    return hash_splitmix64(static_cast<uint64_t>(val));
}

uint64_t roxy_u64_hash(uint64_t val) {
    return hash_splitmix64(val);
}

uint64_t roxy_f32_hash(float val) {
    if (val == 0.0f) val = 0.0f;  // Normalize -0
    uint32_t bits;
    memcpy(&bits, &val, sizeof(uint32_t));
    return hash_splitmix64(static_cast<uint64_t>(bits));
}

uint64_t roxy_f64_hash(double val) {
    if (val == 0.0) val = 0.0;  // Normalize -0
    uint64_t bits;
    memcpy(&bits, &val, sizeof(uint64_t));
    return hash_splitmix64(bits);
}

uint64_t roxy_string_hash(void* val) {
    if (!val) return 0;
    // Read the cached low-32 hash. The high bits are filled with 0; the
    // map's probe mask only uses the low 32 bits anyway (capacity is u32).
    return static_cast<uint64_t>(string_hdr(val)->hash);
}

// ===== Map Operations =====

static roxy_map_header* map_hdr(void* self) {
    return static_cast<roxy_map_header*>(self);
}

// Read a u64 worth of bytes from a key slot array (works for primitive
// 2-slot inline keys: Integer/Float32/Float64/String).
static inline uint64_t read_packed_u64(const uint32_t* key_src) {
    uint64_t packed = 0;
    memcpy(&packed, key_src, sizeof(uint64_t));
    return packed;
}

// Internal: hash a key based on key_kind. For Struct keys, dispatches through
// the user-provided hash_fn if set; otherwise falls back to bytewise FNV-1a.
static uint64_t map_hash_key(const uint32_t* key_src, const roxy_map_header* hdr) {
    switch (hdr->key_kind) {
        case ROXY_MAP_KEY_INTEGER:
            return hash_splitmix64(read_packed_u64(key_src));
        case ROXY_MAP_KEY_FLOAT32: {
            float val;
            memcpy(&val, key_src, sizeof(float));
            if (val == 0.0f) val = 0.0f;
            uint32_t bits;
            memcpy(&bits, &val, sizeof(uint32_t));
            return hash_splitmix64(static_cast<uint64_t>(bits));
        }
        case ROXY_MAP_KEY_FLOAT64: {
            double val;
            memcpy(&val, key_src, sizeof(double));
            if (val == 0.0) val = 0.0;
            uint64_t bits;
            memcpy(&bits, &val, sizeof(uint64_t));
            return hash_splitmix64(bits);
        }
        case ROXY_MAP_KEY_STRING: {
            uint64_t ptr_bits = read_packed_u64(key_src);
            void* str = reinterpret_cast<void*>(ptr_bits);
            if (!str) return 0;
            // Read the cached hash from the string header — same field the
            // VM-side `Map<string,V>` lookup at vm/map.cpp:84 uses.
            return static_cast<uint64_t>(string_hdr(str)->hash);
        }
        case ROXY_MAP_KEY_STRUCT:
            if (hdr->hash_fn) {
                return hdr->hash_fn(key_src);
            }
            return hash_fnv1a(reinterpret_cast<const char*>(key_src),
                              static_cast<uint32_t>(hdr->key_slot_count) * 4);
    }
    return hash_splitmix64(read_packed_u64(key_src));
}

// Internal: compare two keys for equality. For Struct keys, dispatches through
// the user-provided eq_fn if set; otherwise falls back to bytewise memcmp.
// Note: in the C backend the user's `K__eq(K* self, K* other)` C signature
// already takes both args as pointers (Roxy's `other: K` lowers to `K*` in
// the C output via the existing struct-by-pointer convention) — no calling-
// convention dance needed here, unlike the VM where `other: K` arrives as
// packed-by-value bytes.
static bool map_keys_equal(const uint32_t* a, const uint32_t* b,
                           const roxy_map_header* hdr) {
    switch (hdr->key_kind) {
        case ROXY_MAP_KEY_INTEGER:
            return read_packed_u64(a) == read_packed_u64(b);
        case ROXY_MAP_KEY_FLOAT32: {
            float fa, fb;
            memcpy(&fa, a, sizeof(float));
            memcpy(&fb, b, sizeof(float));
            return fa == fb;
        }
        case ROXY_MAP_KEY_FLOAT64: {
            double fa, fb;
            memcpy(&fa, a, sizeof(double));
            memcpy(&fb, b, sizeof(double));
            return fa == fb;
        }
        case ROXY_MAP_KEY_STRING: {
            uint64_t a_bits = read_packed_u64(a);
            uint64_t b_bits = read_packed_u64(b);
            return roxy_string_eq(reinterpret_cast<void*>(a_bits),
                                  reinterpret_cast<void*>(b_bits));
        }
        case ROXY_MAP_KEY_STRUCT:
            if (hdr->eq_fn) {
                return hdr->eq_fn(a, b);
            }
            return memcmp(a, b, static_cast<size_t>(hdr->key_slot_count) * 4) == 0;
    }
    return read_packed_u64(a) == read_packed_u64(b);
}

static inline uint32_t* map_key_ptr(const roxy_map_header* hdr, uint32_t pos) {
    return hdr->keys + static_cast<size_t>(pos) * hdr->key_slot_count;
}

static inline uint32_t* map_value_ptr(const roxy_map_header* hdr, uint32_t pos) {
    return hdr->values + static_cast<size_t>(pos) * hdr->value_slot_count;
}

static void map_alloc_buckets(roxy_map_header* hdr, uint32_t capacity) {
    assert(capacity > 0 && (capacity & (capacity - 1)) == 0);
    hdr->capacity = capacity;
    hdr->distances = static_cast<uint8_t*>(calloc(capacity, sizeof(uint8_t)));
    hdr->keys = static_cast<uint32_t*>(calloc(
        static_cast<size_t>(capacity) * hdr->key_slot_count, sizeof(uint32_t)));
    hdr->values = static_cast<uint32_t*>(calloc(
        static_cast<size_t>(capacity) * hdr->value_slot_count, sizeof(uint32_t)));
}

static void map_free_buckets(roxy_map_header* hdr) {
    free(hdr->distances);
    free(hdr->keys);
    free(hdr->values);
    hdr->distances = nullptr;
    hdr->keys = nullptr;
    hdr->values = nullptr;
    hdr->capacity = 0;
}

// Insert without grow check (used during rehash).
// `key_src` and `value_src` each point to slot_count*4 bytes to copy.
//
// Robin Hood with variable-sized keys AND values needs ping-pong scratch
// buffers for both, so the entry being placed survives multiple displacements
// in the chain. The defensive memcpy of caller's bytes into buf_a defends
// against `value_src` / `key_src` aliasing the bucket array (rehash case).
static void map_insert_internal(roxy_map_header* hdr,
                                const uint32_t* key_src,
                                const uint32_t* value_src) {
    uint32_t mask = hdr->capacity - 1;
    uint8_t ksc = hdr->key_slot_count;
    uint8_t vsc = hdr->value_slot_count;
    uint64_t hash = map_hash_key(key_src, hdr);
    uint32_t pos = static_cast<uint32_t>(hash) & mask;
    uint8_t dist = 1;

    uint32_t stack_ka[16], stack_kb[16];
    uint32_t stack_va[16], stack_vb[16];
    uint32_t* kbuf_a = (ksc <= 16) ? stack_ka : static_cast<uint32_t*>(malloc(sizeof(uint32_t) * ksc));
    uint32_t* kbuf_b = (ksc <= 16) ? stack_kb : static_cast<uint32_t*>(malloc(sizeof(uint32_t) * ksc));
    uint32_t* vbuf_a = (vsc <= 16) ? stack_va : static_cast<uint32_t*>(malloc(sizeof(uint32_t) * vsc));
    uint32_t* vbuf_b = (vsc <= 16) ? stack_vb : static_cast<uint32_t*>(malloc(sizeof(uint32_t) * vsc));

    memcpy(kbuf_a, key_src, sizeof(uint32_t) * ksc);
    memcpy(vbuf_a, value_src, sizeof(uint32_t) * vsc);
    uint32_t* ksrc = kbuf_a;
    uint32_t* kscratch = kbuf_b;
    uint32_t* vsrc = vbuf_a;
    uint32_t* vscratch = vbuf_b;

    while (true) {
        if (hdr->distances[pos] == 0) {
            hdr->distances[pos] = dist;
            memcpy(map_key_ptr(hdr, pos), ksrc, sizeof(uint32_t) * ksc);
            memcpy(map_value_ptr(hdr, pos), vsrc, sizeof(uint32_t) * vsc);
            if (kbuf_a != stack_ka) free(kbuf_a);
            if (kbuf_b != stack_kb) free(kbuf_b);
            if (vbuf_a != stack_va) free(vbuf_a);
            if (vbuf_b != stack_vb) free(vbuf_b);
            return;
        }
        if (hdr->distances[pos] < dist) {
            uint8_t tmp_dist = hdr->distances[pos];
            memcpy(kscratch, map_key_ptr(hdr, pos), sizeof(uint32_t) * ksc);
            memcpy(vscratch, map_value_ptr(hdr, pos), sizeof(uint32_t) * vsc);
            hdr->distances[pos] = dist;
            memcpy(map_key_ptr(hdr, pos), ksrc, sizeof(uint32_t) * ksc);
            memcpy(map_value_ptr(hdr, pos), vsrc, sizeof(uint32_t) * vsc);
            dist = tmp_dist;
            uint32_t* tmp = ksrc; ksrc = kscratch; kscratch = tmp;
            tmp = vsrc; vsrc = vscratch; vscratch = tmp;
        }
        pos = (pos + 1) & mask;
        dist++;
        assert(dist < 255);
    }
}

static void map_grow(roxy_map_header* hdr) {
    uint32_t old_capacity = hdr->capacity;
    uint8_t*  old_distances = hdr->distances;
    uint32_t* old_keys = hdr->keys;
    uint32_t* old_values = hdr->values;
    uint8_t ksc = hdr->key_slot_count;
    uint8_t vsc = hdr->value_slot_count;

    uint32_t new_capacity = old_capacity == 0 ? 8 : old_capacity * 2;
    map_alloc_buckets(hdr, new_capacity);
    hdr->length = 0;

    for (uint32_t i = 0; i < old_capacity; i++) {
        if (old_distances[i] != 0) {
            const uint32_t* old_key_ptr = old_keys + static_cast<size_t>(i) * ksc;
            const uint32_t* old_val_ptr = old_values + static_cast<size_t>(i) * vsc;
            map_insert_internal(hdr, old_key_ptr, old_val_ptr);
            hdr->length++;
        }
    }

    free(old_distances);
    free(old_keys);
    free(old_values);
}

void* roxy_map_alloc(int32_t key_slot_count, int32_t key_is_inline,
                     int32_t value_slot_count, int32_t value_is_inline,
                     roxy_map_hash_fn hash_fn, roxy_map_eq_fn eq_fn) {
    void* data = roxy_alloc(sizeof(roxy_map_header), ROXY_TYPEID_MAP);
    if (!data) return nullptr;
    auto* hdr = map_hdr(data);
    hdr->key_slot_count = key_slot_count > 0
        ? static_cast<uint8_t>(key_slot_count) : static_cast<uint8_t>(2);
    hdr->key_is_inline = key_is_inline != 0 ? 1 : 0;
    hdr->value_slot_count = value_slot_count > 0
        ? static_cast<uint8_t>(value_slot_count) : static_cast<uint8_t>(2);
    hdr->value_is_inline = value_is_inline != 0 ? 1 : 0;
    hdr->hash_fn = hash_fn;
    hdr->eq_fn = eq_fn;
    return data;
}

void roxy_map_init(void* self, int32_t key_kind, int32_t capacity) {
    auto* hdr = map_hdr(self);
    hdr->length = 0;
    hdr->key_kind = static_cast<uint8_t>(key_kind);
    hdr->distances = nullptr;
    hdr->keys = nullptr;
    hdr->values = nullptr;
    hdr->capacity = 0;
    // key/value layout fields already set by roxy_map_alloc.

    if (capacity > 0) {
        uint32_t actual = 8;
        while (actual < static_cast<uint32_t>(capacity)) actual *= 2;
        map_alloc_buckets(hdr, actual);
    }
}

void roxy_map_delete(void* self) {
    auto* hdr = map_hdr(self);
    map_free_buckets(hdr);
    hdr->length = 0;
}

int32_t roxy_map_len(void* self) {
    return static_cast<int32_t>(map_hdr(self)->length);
}

bool roxy_map_contains(void* self, const void* key_src) {
    auto* hdr = map_hdr(self);
    if (hdr->capacity == 0 || hdr->length == 0) return false;

    uint8_t ksc = hdr->key_slot_count;
    uint32_t mask = hdr->capacity - 1;
    auto* k = static_cast<const uint32_t*>(key_src);
    uint64_t hash = map_hash_key(k, hdr);
    uint32_t pos = static_cast<uint32_t>(hash) & mask;
    uint8_t dist = 1;

    while (true) {
        if (hdr->distances[pos] == 0) return false;
        if (hdr->distances[pos] < dist) return false;
        if (hdr->distances[pos] == dist &&
            map_keys_equal(map_key_ptr(hdr, pos), k, hdr)) {
            return true;
        }
        pos = (pos + 1) & mask;
        dist++;
    }
}

void* roxy_map_get(void* self, const void* key_src) {
    auto* hdr = map_hdr(self);
    if (hdr->capacity == 0 || hdr->length == 0) {
        assert(false && "Map key not found");
        return nullptr;
    }

    uint8_t ksc = hdr->key_slot_count;
    uint32_t mask = hdr->capacity - 1;
    auto* k = static_cast<const uint32_t*>(key_src);
    uint64_t hash = map_hash_key(k, hdr);
    uint32_t pos = static_cast<uint32_t>(hash) & mask;
    uint8_t dist = 1;

    while (true) {
        // Robin Hood termination: if the bucket is empty, or holds an
        // entry with smaller probe distance, the key isn't present.
        // Returns nullptr on miss — VM-side wrappers turn this into a
        // "Map key not found" error message; AOT-generated code is
        // expected to call `roxy_map_contains` first when missing keys
        // are possible.
        if (hdr->distances[pos] == 0 || hdr->distances[pos] < dist) {
            return nullptr;
        }
        if (hdr->distances[pos] == dist &&
            map_keys_equal(map_key_ptr(hdr, pos), k, hdr)) {
            return map_value_ptr(hdr, pos);
        }
        pos = (pos + 1) & mask;
        dist++;
    }
}

void roxy_map_insert(void* self, const void* key_src, const void* value_src) {
    auto* hdr = map_hdr(self);
    uint8_t ksc = hdr->key_slot_count;
    uint8_t vsc = hdr->value_slot_count;
    auto* k = static_cast<const uint32_t*>(key_src);
    auto* v = static_cast<const uint32_t*>(value_src);

    // Check if key already exists — update in place
    if (hdr->capacity > 0 && hdr->length > 0) {
        uint32_t mask = hdr->capacity - 1;
        uint64_t hash = map_hash_key(k, hdr);
        uint32_t pos = static_cast<uint32_t>(hash) & mask;
        uint8_t dist = 1;

        while (true) {
            if (hdr->distances[pos] == 0) break;
            if (hdr->distances[pos] < dist) break;
            if (hdr->distances[pos] == dist &&
                map_keys_equal(map_key_ptr(hdr, pos), k, hdr)) {
                memcpy(map_value_ptr(hdr, pos), v, sizeof(uint32_t) * vsc);
                return;
            }
            pos = (pos + 1) & mask;
            dist++;
        }
    }

    // New key — grow if needed (80% load factor)
    if (hdr->capacity == 0 || (hdr->length + 1) > hdr->capacity * 4 / 5) {
        map_grow(hdr);
    }

    map_insert_internal(hdr, k, v);
    hdr->length++;
}

bool roxy_map_remove(void* self, const void* key_src) {
    auto* hdr = map_hdr(self);
    if (hdr->capacity == 0 || hdr->length == 0) return false;

    uint8_t ksc = hdr->key_slot_count;
    uint8_t vsc = hdr->value_slot_count;
    uint32_t mask = hdr->capacity - 1;
    auto* k = static_cast<const uint32_t*>(key_src);
    uint64_t hash = map_hash_key(k, hdr);
    uint32_t pos = static_cast<uint32_t>(hash) & mask;
    uint8_t dist = 1;

    while (true) {
        if (hdr->distances[pos] == 0) return false;
        if (hdr->distances[pos] < dist) return false;
        if (hdr->distances[pos] == dist &&
            map_keys_equal(map_key_ptr(hdr, pos), k, hdr)) {
            break;
        }
        pos = (pos + 1) & mask;
        dist++;
    }

    // Backward-shift deletion
    hdr->length--;
    while (true) {
        uint32_t next = (pos + 1) & mask;
        if (hdr->distances[next] <= 1) {
            hdr->distances[pos] = 0;
            memset(map_key_ptr(hdr, pos), 0, sizeof(uint32_t) * ksc);
            memset(map_value_ptr(hdr, pos), 0, sizeof(uint32_t) * vsc);
            return true;
        }
        hdr->distances[pos] = hdr->distances[next] - 1;
        memcpy(map_key_ptr(hdr, pos), map_key_ptr(hdr, next),
               sizeof(uint32_t) * ksc);
        memcpy(map_value_ptr(hdr, pos), map_value_ptr(hdr, next),
               sizeof(uint32_t) * vsc);
        pos = next;
    }
}

void roxy_map_clear(void* self) {
    auto* hdr = map_hdr(self);
    hdr->length = 0;
    if (hdr->capacity > 0) {
        memset(hdr->distances, 0, sizeof(uint8_t) * hdr->capacity);
        memset(hdr->keys, 0,
               sizeof(uint32_t) * static_cast<size_t>(hdr->capacity) * hdr->key_slot_count);
        memset(hdr->values, 0,
               sizeof(uint32_t) * static_cast<size_t>(hdr->capacity) * hdr->value_slot_count);
    }
}

void* roxy_map_keys(void* self) {
    auto* hdr = map_hdr(self);
    // Mirror the key layout into the produced List<K>.
    void* lst = roxy_list_alloc(static_cast<int32_t>(hdr->key_slot_count),
                                static_cast<int32_t>(hdr->key_is_inline));
    if (!lst) return nullptr;
    roxy_list_init(lst, static_cast<int32_t>(hdr->length));

    for (uint32_t i = 0; i < hdr->capacity; i++) {
        if (hdr->distances[i] != 0) {
            roxy_list_push(lst, map_key_ptr(hdr, i));
        }
    }
    return lst;
}

void* roxy_map_values(void* self) {
    auto* hdr = map_hdr(self);
    // Mirror the value layout into the produced List<V>.
    void* lst = roxy_list_alloc(static_cast<int32_t>(hdr->value_slot_count),
                                static_cast<int32_t>(hdr->value_is_inline));
    if (!lst) return nullptr;
    roxy_list_init(lst, static_cast<int32_t>(hdr->length));

    for (uint32_t i = 0; i < hdr->capacity; i++) {
        if (hdr->distances[i] != 0) {
            roxy_list_push(lst, map_value_ptr(hdr, i));
        }
    }
    return lst;
}

void* roxy_map_copy(void* src) {
    if (!src) return nullptr;
    auto* src_hdr = map_hdr(src);

    void* dst = roxy_map_alloc(static_cast<int32_t>(src_hdr->key_slot_count),
                               static_cast<int32_t>(src_hdr->key_is_inline),
                               static_cast<int32_t>(src_hdr->value_slot_count),
                               static_cast<int32_t>(src_hdr->value_is_inline),
                               src_hdr->hash_fn, src_hdr->eq_fn);
    if (!dst) return nullptr;

    auto* dst_hdr = map_hdr(dst);
    dst_hdr->key_kind = src_hdr->key_kind;
    if (src_hdr->capacity > 0) {
        map_alloc_buckets(dst_hdr, src_hdr->capacity);
        dst_hdr->length = src_hdr->length;
        memcpy(dst_hdr->distances, src_hdr->distances, sizeof(uint8_t) * src_hdr->capacity);
        memcpy(dst_hdr->keys, src_hdr->keys,
               sizeof(uint32_t) * static_cast<size_t>(src_hdr->capacity) * src_hdr->key_slot_count);
        memcpy(dst_hdr->values, src_hdr->values,
               sizeof(uint32_t) * static_cast<size_t>(src_hdr->capacity) * src_hdr->value_slot_count);
    }
    return dst;
}

void* roxy_map_index(void* self, const void* key_src) {
    return roxy_map_get(self, key_src);
}

void roxy_map_index_mut(void* self, const void* key_src, const void* value_src) {
    roxy_map_insert(self, key_src, value_src);
}

// ===== Internal map iteration =====

int32_t roxy_map_iter_capacity(void* self) {
    return static_cast<int32_t>(map_hdr(self)->capacity);
}

int32_t roxy_map_iter_next_occupied(void* self, int32_t idx) {
    auto* hdr = map_hdr(self);
    for (int32_t i = idx; i < static_cast<int32_t>(hdr->capacity); i++) {
        if (hdr->distances[i] != 0) return i;
    }
    return static_cast<int32_t>(hdr->capacity);  // Sentinel: past end
}

uint64_t roxy_map_iter_key_at(void* self, int32_t idx) {
    // Cleanup-only path: noncopyable keys are pointer-sized (≤ 2 slots).
    auto* hdr = map_hdr(self);
    uint32_t copy_slots = hdr->key_slot_count <= 2 ? hdr->key_slot_count : 2u;
    uint64_t packed = 0;
    memcpy(&packed,
           hdr->keys + static_cast<size_t>(idx) * hdr->key_slot_count,
           sizeof(uint32_t) * copy_slots);
    return packed;
}

uint64_t roxy_map_iter_value_at(void* self, int32_t idx) {
    // Cleanup-only path: noncopyable values are pointer-sized (≤ 2 slots).
    // Pack up to 2 u32 slots into a u64 — matches VM's native_map_iter_value_at.
    auto* hdr = map_hdr(self);
    uint32_t copy_slots = hdr->value_slot_count <= 2 ? hdr->value_slot_count : 2u;
    uint64_t packed = 0;
    memcpy(&packed,
           hdr->values + static_cast<size_t>(idx) * hdr->value_slot_count,
           sizeof(uint32_t) * copy_slots);
    return packed;
}
