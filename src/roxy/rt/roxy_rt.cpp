#include "roxy/rt/roxy_rt.h"

#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cassert>

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

// ===== Allocation =====

void* roxy_alloc(uint32_t data_size, uint32_t type_id) {
    uint8_t* raw = static_cast<uint8_t*>(malloc(sizeof(roxy_object_header) + data_size));
    if (!raw) return nullptr;

    auto* header = reinterpret_cast<roxy_object_header*>(raw);
    header->weak_generation = roxy_random_generation();
    header->ref_count = 0;
    header->type_id = type_id;

    void* data = raw + sizeof(roxy_object_header);
    memset(data, 0, data_size);
    return data;
}

void roxy_free(void* data) {
    if (!data) return;
    auto* header = roxy_get_header(data);
    header->weak_generation = 0;  // Tombstone
    uint8_t* raw = reinterpret_cast<uint8_t*>(data) - sizeof(roxy_object_header);
    free(raw);
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

// ===== String Operations =====

static roxy_string_header* string_hdr(void* s) {
    return static_cast<roxy_string_header*>(s);
}

void* roxy_string_from_literal(const char* data, uint32_t length) {
    uint32_t data_size = static_cast<uint32_t>(sizeof(roxy_string_header)) + length + 1;
    void* s = roxy_alloc(data_size, ROXY_TYPEID_STRING);
    if (!s) return nullptr;

    auto* hdr = string_hdr(s);
    hdr->length = length;
    hdr->capacity = length + 1;

    char* chars = reinterpret_cast<char*>(reinterpret_cast<uint8_t*>(s) + sizeof(roxy_string_header));
    if (length > 0) {
        memcpy(chars, data, length);
    }
    chars[length] = '\0';
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

    uint32_t data_size = static_cast<uint32_t>(sizeof(roxy_string_header)) + total + 1;
    void* s = roxy_alloc(data_size, ROXY_TYPEID_STRING);
    if (!s) return nullptr;

    auto* hdr = string_hdr(s);
    hdr->length = total;
    hdr->capacity = total + 1;

    char* chars = roxy_string_chars(s);
    memcpy(chars, roxy_string_chars(a), len_a);
    memcpy(chars + len_a, roxy_string_chars(b), len_b);
    chars[total] = '\0';
    return s;
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

void* roxy_list_alloc(int32_t element_slot_count, int32_t element_is_inline) {
    (void)element_slot_count;  // C backend uses uint64_t elements; multi-slot deferred
    (void)element_is_inline;
    void* data = roxy_alloc(sizeof(roxy_list_header), ROXY_TYPEID_LIST);
    if (!data) return nullptr;
    // Already zero-initialized by roxy_alloc
    return data;
}

void roxy_list_init(void* self, int32_t capacity) {
    auto* hdr = static_cast<roxy_list_header*>(self);
    hdr->length = 0;
    if (capacity > 0) {
        hdr->capacity = static_cast<uint32_t>(capacity);
        hdr->elements = static_cast<uint64_t*>(calloc(capacity, sizeof(uint64_t)));
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

void roxy_list_push(void* self, uint64_t value) {
    auto* hdr = static_cast<roxy_list_header*>(self);
    if (hdr->length >= hdr->capacity) {
        uint32_t new_cap = hdr->capacity == 0 ? 8 : hdr->capacity * 2;
        auto* new_elements = static_cast<uint64_t*>(realloc(hdr->elements, new_cap * sizeof(uint64_t)));
        if (!new_elements) return;
        hdr->elements = new_elements;
        hdr->capacity = new_cap;
    }
    hdr->elements[hdr->length++] = value;
}

uint64_t roxy_list_pop(void* self) {
    auto* hdr = static_cast<roxy_list_header*>(self);
    assert(hdr->length > 0);
    return hdr->elements[--hdr->length];
}

uint64_t roxy_list_get(void* self, int32_t index) {
    auto* hdr = static_cast<roxy_list_header*>(self);
    assert(index >= 0 && static_cast<uint32_t>(index) < hdr->length);
    return hdr->elements[index];
}

void roxy_list_set(void* self, int32_t index, uint64_t value) {
    auto* hdr = static_cast<roxy_list_header*>(self);
    assert(index >= 0 && static_cast<uint32_t>(index) < hdr->length);
    hdr->elements[index] = value;
}

void* roxy_list_copy(void* src) {
    if (!src) return nullptr;
    auto* src_hdr = static_cast<roxy_list_header*>(src);

    void* dst = roxy_list_alloc(2, 1);
    if (!dst) return nullptr;

    auto* dst_hdr = static_cast<roxy_list_header*>(dst);
    if (src_hdr->capacity > 0) {
        dst_hdr->length = src_hdr->length;
        dst_hdr->capacity = src_hdr->capacity;
        dst_hdr->elements = static_cast<uint64_t*>(malloc(src_hdr->capacity * sizeof(uint64_t)));
        memcpy(dst_hdr->elements, src_hdr->elements, src_hdr->length * sizeof(uint64_t));
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

int64_t roxy_bool_hash(bool val) {
    return static_cast<int64_t>(hash_splitmix64(val ? 1 : 0));
}

int64_t roxy_i8_hash(int8_t val) {
    return static_cast<int64_t>(hash_splitmix64(static_cast<uint64_t>(val)));
}

int64_t roxy_i16_hash(int16_t val) {
    return static_cast<int64_t>(hash_splitmix64(static_cast<uint64_t>(val)));
}

int64_t roxy_i32_hash(int32_t val) {
    return static_cast<int64_t>(hash_splitmix64(static_cast<uint64_t>(val)));
}

int64_t roxy_i64_hash(int64_t val) {
    return static_cast<int64_t>(hash_splitmix64(static_cast<uint64_t>(val)));
}

int64_t roxy_u8_hash(uint8_t val) {
    return static_cast<int64_t>(hash_splitmix64(static_cast<uint64_t>(val)));
}

int64_t roxy_u16_hash(uint16_t val) {
    return static_cast<int64_t>(hash_splitmix64(static_cast<uint64_t>(val)));
}

int64_t roxy_u32_hash(uint32_t val) {
    return static_cast<int64_t>(hash_splitmix64(static_cast<uint64_t>(val)));
}

int64_t roxy_u64_hash(uint64_t val) {
    return static_cast<int64_t>(hash_splitmix64(val));
}

int64_t roxy_f32_hash(float val) {
    if (val == 0.0f) val = 0.0f;  // Normalize -0
    uint32_t bits;
    memcpy(&bits, &val, sizeof(uint32_t));
    return static_cast<int64_t>(hash_splitmix64(static_cast<uint64_t>(bits)));
}

int64_t roxy_f64_hash(double val) {
    if (val == 0.0) val = 0.0;  // Normalize -0
    uint64_t bits;
    memcpy(&bits, &val, sizeof(uint64_t));
    return static_cast<int64_t>(hash_splitmix64(bits));
}

int64_t roxy_string_hash(void* val) {
    if (!val) return 0;
    return static_cast<int64_t>(hash_fnv1a(roxy_string_chars(val),
                                            static_cast<uint32_t>(roxy_string_len(val))));
}

// ===== Map Operations =====

static roxy_map_header* map_hdr(void* self) {
    return static_cast<roxy_map_header*>(self);
}

// Internal: hash a key based on key_kind
static uint64_t map_hash_key(uint64_t key, uint8_t key_kind) {
    switch (key_kind) {
        case ROXY_MAP_KEY_INTEGER:
            return hash_splitmix64(key);
        case ROXY_MAP_KEY_FLOAT32: {
            float val;
            memcpy(&val, &key, sizeof(float));
            if (val == 0.0f) val = 0.0f;
            uint32_t bits;
            memcpy(&bits, &val, sizeof(uint32_t));
            return hash_splitmix64(static_cast<uint64_t>(bits));
        }
        case ROXY_MAP_KEY_FLOAT64: {
            double val;
            memcpy(&val, &key, sizeof(double));
            if (val == 0.0) val = 0.0;
            uint64_t bits;
            memcpy(&bits, &val, sizeof(uint64_t));
            return hash_splitmix64(bits);
        }
        case ROXY_MAP_KEY_STRING: {
            void* str = reinterpret_cast<void*>(key);
            if (!str) return 0;
            return hash_fnv1a(roxy_string_chars(str),
                              static_cast<uint32_t>(roxy_string_len(str)));
        }
    }
    return hash_splitmix64(key);
}

// Internal: compare two keys for equality
static bool map_keys_equal(uint64_t a, uint64_t b, uint8_t key_kind) {
    switch (key_kind) {
        case ROXY_MAP_KEY_INTEGER:
            return a == b;
        case ROXY_MAP_KEY_FLOAT32: {
            float fa, fb;
            memcpy(&fa, &a, sizeof(float));
            memcpy(&fb, &b, sizeof(float));
            return fa == fb;
        }
        case ROXY_MAP_KEY_FLOAT64: {
            double fa, fb;
            memcpy(&fa, &a, sizeof(double));
            memcpy(&fb, &b, sizeof(double));
            return fa == fb;
        }
        case ROXY_MAP_KEY_STRING: {
            void* str_a = reinterpret_cast<void*>(a);
            void* str_b = reinterpret_cast<void*>(b);
            return roxy_string_eq(str_a, str_b);
        }
    }
    return a == b;
}

static void map_alloc_buckets(roxy_map_header* hdr, uint32_t capacity) {
    assert(capacity > 0 && (capacity & (capacity - 1)) == 0);
    hdr->capacity = capacity;
    hdr->distances = static_cast<uint8_t*>(calloc(capacity, sizeof(uint8_t)));
    hdr->keys = static_cast<uint64_t*>(calloc(capacity, sizeof(uint64_t)));
    hdr->values = static_cast<uint64_t*>(calloc(capacity, sizeof(uint64_t)));
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

static void map_insert_internal(roxy_map_header* hdr, uint64_t key, uint64_t value) {
    uint32_t mask = hdr->capacity - 1;
    uint64_t hash = map_hash_key(key, hdr->key_kind);
    uint32_t pos = static_cast<uint32_t>(hash) & mask;
    uint8_t dist = 1;

    while (true) {
        if (hdr->distances[pos] == 0) {
            hdr->distances[pos] = dist;
            hdr->keys[pos] = key;
            hdr->values[pos] = value;
            return;
        }
        if (hdr->distances[pos] < dist) {
            uint8_t tmp_dist = hdr->distances[pos];
            uint64_t tmp_key = hdr->keys[pos];
            uint64_t tmp_val = hdr->values[pos];
            hdr->distances[pos] = dist;
            hdr->keys[pos] = key;
            hdr->values[pos] = value;
            dist = tmp_dist;
            key = tmp_key;
            value = tmp_val;
        }
        pos = (pos + 1) & mask;
        dist++;
        assert(dist < 255);
    }
}

static void map_grow(roxy_map_header* hdr) {
    uint32_t old_capacity = hdr->capacity;
    uint8_t* old_distances = hdr->distances;
    uint64_t* old_keys = hdr->keys;
    uint64_t* old_values = hdr->values;

    uint32_t new_capacity = old_capacity == 0 ? 8 : old_capacity * 2;
    map_alloc_buckets(hdr, new_capacity);
    hdr->length = 0;

    for (uint32_t i = 0; i < old_capacity; i++) {
        if (old_distances[i] != 0) {
            map_insert_internal(hdr, old_keys[i], old_values[i]);
            hdr->length++;
        }
    }

    free(old_distances);
    free(old_keys);
    free(old_values);
}

void* roxy_map_alloc() {
    void* data = roxy_alloc(sizeof(roxy_map_header), ROXY_TYPEID_MAP);
    if (!data) return nullptr;
    // Already zero-initialized by roxy_alloc
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

bool roxy_map_contains(void* self, uint64_t key) {
    auto* hdr = map_hdr(self);
    if (hdr->capacity == 0 || hdr->length == 0) return false;

    uint32_t mask = hdr->capacity - 1;
    uint64_t hash = map_hash_key(key, hdr->key_kind);
    uint32_t pos = static_cast<uint32_t>(hash) & mask;
    uint8_t dist = 1;

    while (true) {
        if (hdr->distances[pos] == 0) return false;
        if (hdr->distances[pos] < dist) return false;
        if (hdr->distances[pos] == dist &&
            map_keys_equal(hdr->keys[pos], key, hdr->key_kind)) {
            return true;
        }
        pos = (pos + 1) & mask;
        dist++;
    }
}

uint64_t roxy_map_get(void* self, uint64_t key) {
    auto* hdr = map_hdr(self);
    if (hdr->capacity == 0 || hdr->length == 0) {
        assert(false && "Map key not found");
        return 0;
    }

    uint32_t mask = hdr->capacity - 1;
    uint64_t hash = map_hash_key(key, hdr->key_kind);
    uint32_t pos = static_cast<uint32_t>(hash) & mask;
    uint8_t dist = 1;

    while (true) {
        if (hdr->distances[pos] == 0 || hdr->distances[pos] < dist) {
            assert(false && "Map key not found");
            return 0;
        }
        if (hdr->distances[pos] == dist &&
            map_keys_equal(hdr->keys[pos], key, hdr->key_kind)) {
            return hdr->values[pos];
        }
        pos = (pos + 1) & mask;
        dist++;
    }
}

void roxy_map_insert(void* self, uint64_t key, uint64_t value) {
    auto* hdr = map_hdr(self);

    // Check if key already exists — update in place
    if (hdr->capacity > 0 && hdr->length > 0) {
        uint32_t mask = hdr->capacity - 1;
        uint64_t hash = map_hash_key(key, hdr->key_kind);
        uint32_t pos = static_cast<uint32_t>(hash) & mask;
        uint8_t dist = 1;

        while (true) {
            if (hdr->distances[pos] == 0) break;
            if (hdr->distances[pos] < dist) break;
            if (hdr->distances[pos] == dist &&
                map_keys_equal(hdr->keys[pos], key, hdr->key_kind)) {
                hdr->values[pos] = value;
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

    map_insert_internal(hdr, key, value);
    hdr->length++;
}

bool roxy_map_remove(void* self, uint64_t key) {
    auto* hdr = map_hdr(self);
    if (hdr->capacity == 0 || hdr->length == 0) return false;

    uint32_t mask = hdr->capacity - 1;
    uint64_t hash = map_hash_key(key, hdr->key_kind);
    uint32_t pos = static_cast<uint32_t>(hash) & mask;
    uint8_t dist = 1;

    while (true) {
        if (hdr->distances[pos] == 0) return false;
        if (hdr->distances[pos] < dist) return false;
        if (hdr->distances[pos] == dist &&
            map_keys_equal(hdr->keys[pos], key, hdr->key_kind)) {
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
            hdr->keys[pos] = 0;
            hdr->values[pos] = 0;
            return true;
        }
        hdr->distances[pos] = hdr->distances[next] - 1;
        hdr->keys[pos] = hdr->keys[next];
        hdr->values[pos] = hdr->values[next];
        pos = next;
    }
}

void roxy_map_clear(void* self) {
    auto* hdr = map_hdr(self);
    hdr->length = 0;
    if (hdr->capacity > 0) {
        memset(hdr->distances, 0, sizeof(uint8_t) * hdr->capacity);
        memset(hdr->keys, 0, sizeof(uint64_t) * hdr->capacity);
        memset(hdr->values, 0, sizeof(uint64_t) * hdr->capacity);
    }
}

void* roxy_map_keys(void* self) {
    auto* hdr = map_hdr(self);
    void* lst = roxy_list_alloc(2, 1);
    if (!lst) return nullptr;
    roxy_list_init(lst, static_cast<int32_t>(hdr->length));

    for (uint32_t i = 0; i < hdr->capacity; i++) {
        if (hdr->distances[i] != 0) {
            roxy_list_push(lst, hdr->keys[i]);
        }
    }
    return lst;
}

void* roxy_map_values(void* self) {
    auto* hdr = map_hdr(self);
    void* lst = roxy_list_alloc(2, 1);
    if (!lst) return nullptr;
    roxy_list_init(lst, static_cast<int32_t>(hdr->length));

    for (uint32_t i = 0; i < hdr->capacity; i++) {
        if (hdr->distances[i] != 0) {
            roxy_list_push(lst, hdr->values[i]);
        }
    }
    return lst;
}

void* roxy_map_copy(void* src) {
    if (!src) return nullptr;
    auto* src_hdr = map_hdr(src);

    void* dst = roxy_map_alloc();
    if (!dst) return nullptr;

    auto* dst_hdr = map_hdr(dst);
    dst_hdr->key_kind = src_hdr->key_kind;
    if (src_hdr->capacity > 0) {
        map_alloc_buckets(dst_hdr, src_hdr->capacity);
        dst_hdr->length = src_hdr->length;
        memcpy(dst_hdr->distances, src_hdr->distances, sizeof(uint8_t) * src_hdr->capacity);
        memcpy(dst_hdr->keys, src_hdr->keys, sizeof(uint64_t) * src_hdr->capacity);
        memcpy(dst_hdr->values, src_hdr->values, sizeof(uint64_t) * src_hdr->capacity);
    }
    return dst;
}

uint64_t roxy_map_index(void* self, uint64_t key) {
    return roxy_map_get(self, key);
}

void roxy_map_index_mut(void* self, uint64_t key, uint64_t value) {
    roxy_map_insert(self, key, value);
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
    return map_hdr(self)->keys[idx];
}

uint64_t roxy_map_iter_value_at(void* self, int32_t idx) {
    return map_hdr(self)->values[idx];
}
