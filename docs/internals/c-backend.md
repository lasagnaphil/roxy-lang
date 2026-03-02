# C Backend (AOT Compilation)

> **Status:** Not yet implemented. This document is a design plan.

The C backend translates Roxy's SSA IR into a `.cpp` file. The core logic is C-style (structs, gotos, typed variables), while native function bindings use C++ to interface directly with the embedder's C++ code. The output can be compiled by any C++ compiler (g++, clang++, MSVC).

## Pipeline

```
Source → Lexer → Parser → AST → Semantic Analysis → IR Builder → SSA IR
                                                                    ├→ Lowering → Bytecode → VM          (interpreter)
                                                                    └→ CEmitter → .cpp file → g++/clang++ (AOT)
```

The C emitter operates on the same `IRModule` that the bytecode lowering pass uses. All frontend work (type checking, method resolution, operator rewriting, monomorphization, struct layout) is already done at the IR level.

## Why SSA IR → C

| Source Level | Pros | Cons |
|--------------|------|------|
| AST | Readable output | Must re-implement lowering, method dispatch, operator dispatch, monomorphization |
| **SSA IR** | **All semantic work done, typed, structured** | **Need control flow emission strategy** |
| Bytecode | Simplest transform | Loses types, produces unreadable register-shuffling C |

SSA IR is the sweet spot: every operation is typed, structs are laid out, generics are monomorphized, and operators are desugared into method calls. The mapping to C is mechanical.

## Type Mapping

### Primitives

| Roxy Type | C Type | Header |
|-----------|--------|--------|
| `void` | `void` | — |
| `bool` | `bool` | `<stdbool.h>` |
| `i8` | `int8_t` | `<stdint.h>` |
| `i16` | `int16_t` | `<stdint.h>` |
| `i32` | `int32_t` | `<stdint.h>` |
| `i64` | `int64_t` | `<stdint.h>` |
| `u8` | `uint8_t` | `<stdint.h>` |
| `u16` | `uint16_t` | `<stdint.h>` |
| `u32` | `uint32_t` | `<stdint.h>` |
| `u64` | `uint64_t` | `<stdint.h>` |
| `f32` | `float` | — |
| `f64` | `double` | — |
| `string` | `roxy_string*` | `roxy_rt.h` |

### Compound Types

| Roxy Type | C Type |
|-----------|--------|
| `struct Point { x: i32; y: i32; }` | `typedef struct { int32_t x; int32_t y; } Point;` |
| `enum Color { Red, Green, Blue }` | `typedef enum { Color_Red, Color_Green, Color_Blue } Color;` |
| `List<T>` | `roxy_list*` |
| `uniq T` | `T*` (owns the allocation) |
| `ref T` | `T*` (borrowing, ref-counted) |
| `weak T` | `roxy_weak` (pointer + generation) |

### Reference Types in C

```c
// Weak reference carries a generation for validation
typedef struct {
    void* ptr;
    uint64_t generation;
} roxy_weak;
```

All reference types (`uniq`, `ref`, `weak`) point to data preceded by an `ObjectHeader`, consistent with the VM's existing layout:

```c
typedef struct {
    uint64_t weak_generation;   // 0 = dead/tombstoned
    uint32_t ref_count;
    uint32_t type_id;
} roxy_object_header;
```

## IR to C Mapping

### Constants

```
v0 = const_int 42          →  int64_t v0 = 42;
v1 = const_bool true       →  bool v1 = true;
v2 = const_f64 3.14        →  double v2 = 3.14;
v3 = const_string "hello"  →  roxy_string* v3 = roxy_string_from_literal("hello", 5);
v4 = const_null             →  void* v4 = NULL;
```

### Arithmetic and Comparisons

Binary operations map directly to C operators:

```
v2 = add_i v0, v1    →  int64_t v2 = v0 + v1;
v2 = sub_d v0, v1    →  double v2 = v0 - v1;
v2 = eq_i v0, v1     →  bool v2 = (v0 == v1);
v2 = lt_f v0, v1     →  bool v2 = (v0 < v1);
v1 = neg_i v0        →  int64_t v1 = -v0;
v1 = not v0          →  bool v1 = !v0;
v2 = bit_and v0, v1  →  int64_t v2 = (v0 & v1);
```

### Type Conversions

```
v1 = i_to_f64 v0     →  double v1 = (double)v0;
v1 = f64_to_i v0     →  int64_t v1 = (int64_t)v0;
v1 = i_to_b v0       →  bool v1 = (v0 != 0);
v1 = b_to_i v0       →  int64_t v1 = (int64_t)v0;
v1 = cast v0         →  // Depends on source/target types (CastData)
```

### Structs

Roxy structs map directly to C structs. The slot-based layout is compatible since field ordering is identical.

```
// Struct type emission
typedef struct {
    int32_t x;
    int32_t y;
} Point;

// Stack allocation + field access
v0 = stack_alloc 2          →  Point v0_struct = {0}; Point* v0 = &v0_struct;
v1 = const_int 10
v2 = set_field v0.x <- v1   →  v0->x = v1;
v3 = get_field v0.y         →  int32_t v3 = v0->y;
```

The `StackAlloc` IR instruction becomes a local variable declaration. We emit both the struct value and a pointer to it, since the rest of the IR refers to structs through pointers.

### Struct Copy

```
struct_copy v1, v0, 2       →  memcpy(v1, v0, 2 * sizeof(uint32_t));
                                // Or: *v1 = *v0;  (if types are known)
```

When the concrete struct type is known (which it always is from `IRInst.type`), prefer `*dst = *src` over `memcpy` for clarity and potentially better optimization.

### Functions

IR functions become C functions. Parameters are typed by their IR types. The runtime context is accessed via thread-local storage (see [Thread-Local Runtime Context](#thread-local-runtime-context)), so functions do not take a `roxy_ctx*` parameter.

```
// fun add(a: i32, b: i32): i32
fn add(a, b) -> i32 {
    v2 = add_i v0, v1
    return v2
}
→
int32_t add(int32_t v0, int32_t v1) {
    int32_t v2 = v0 + v1;
    return v2;
}
```

#### Out/Inout Parameters

Parameters marked as pointers in `IRFunction::param_is_ptr` become pointer parameters:

```
// fun double_point(p: inout Point)
fn double_point(p) {       // param_is_ptr[0] = true
    v1 = load_ptr v0
    ...
    store_ptr v0, v2
}
→
void double_point(Point* v0) {
    Point v1 = *v0;
    ...
    *v0 = v2;
}
```

#### Large Struct Returns (>16 bytes)

Functions that return large structs use a hidden output pointer (same convention as the bytecode lowering):

```
// fun make_big(): BigStruct    (slot_count > 4)
→
void make_big(BigStruct* _out) {
    BigStruct v0_struct = {0};
    BigStruct* v0 = &v0_struct;
    // ... fill fields ...
    *_out = *v0;
    return;
}
```

### Control Flow

#### Strategy: Labels + Gotos

Each `IRBlock` becomes a labeled section. Block arguments become local variables assigned before `goto`.

```
// SSA IR:
entry:
    v0 = const_int 0
    v1 = const_int 1
    goto loop(v0, v1)

loop(sum, i):
    v4 = le_i v3, v2
    if v4 goto body else exit(sum)

body:
    v5 = add_i sum, i
    v6 = add_i i, const_int(1)
    goto loop(v5, v6)

exit(result):
    return result
```

```cpp
// Generated C:
int64_t sum_of_n(int64_t v2) {
    int64_t v0, v1, v3, v4, v5, v6;
    // Block argument variables
    int64_t loop_arg0, loop_arg1;
    int64_t exit_arg0;

entry:
    v0 = 0;
    v1 = 1;
    loop_arg0 = v0;
    loop_arg1 = v1;
    goto loop;

loop:;
    int64_t sum = loop_arg0;
    int64_t i = loop_arg1;
    v4 = (i <= v2);
    if (v4) goto body; else { exit_arg0 = sum; goto exit; }

body:
    v5 = sum + i;
    v6 = i + 1;
    loop_arg0 = v5;
    loop_arg1 = v6;
    goto loop;

exit:;
    int64_t result = exit_arg0;
    return result;
}
```

Gotos are trivially correct for any CFG shape. C compilers (gcc, clang) optimize gotos into structured control flow internally, so there is no performance penalty.

#### Future: Structured Control Flow Reconstruction

For readable output, a Relooper or Stackifier algorithm could reconstruct `if`/`else`/`while` from the CFG. This is an optional enhancement — gotos work correctly and produce efficient machine code.

### Function Calls

Function calls are direct — no context parameter is threaded through. The runtime context is available via thread-local storage when needed.

```
// Regular call
v2 = call "add" [v0, v1]           →  int32_t v2 = add(v0, v1);

// Method call (mangled name)
v2 = call "Point$$sum" [v1]        →  int32_t v2 = Point__sum(v1);

// External call (cross-module, resolved after linking)
v2 = call_external "math", "sin" [v0]  →  double v2 = math__sin(v0);

// Native call (built-in runtime function)
v2 = call_native "print" [v1]      →  roxy_print(v1);

// Native call (user-registered C++ function)
v2 = call_native "my_add" [v0, v1] →  int32_t v2 = my_add(v0, v1);
```

Note: `$$` in mangled names is replaced with `__` in C identifiers (since `$` is not standard C).

## Native Function Bindings

### The Problem

In the VM path, native functions have the signature `void (*NativeFunction)(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg)` — they read arguments from VM registers and write results back. This is completely VM-specific: there is no register file or `RoxyVM*` in the AOT path.

The C backend needs a different mechanism to call native functions. Since Roxy is designed for C++ embedding (game engines), the generated `.cpp` file calls the embedder's C++ functions directly with typed arguments.

### Thread-Local Runtime Context

The AOT path uses a lightweight runtime context struct, accessed via thread-local storage. This avoids threading a `roxy_ctx*` parameter through every function call.

```c
// roxy_rt.h
typedef struct roxy_ctx {
    void* allocator;        // slab allocator or malloc-based allocator state
    void* exception_state;  // current exception + handler stack for try/catch
    void* user_data;        // embedder-provided state (game engine, etc.)
} roxy_ctx;

// Thread-local context access
void roxy_set_ctx(roxy_ctx* ctx);
roxy_ctx* roxy_get_ctx(void);
```

The `roxy_ctx` is designed to be embedded inside `RoxyVM`, so that native functions written for the AOT path also work in the VM path:

```cpp
// vm.hpp
struct RoxyVM {
    roxy_ctx ctx;           // AOT-compatible subset (first member)
    CallFrame* call_stack;  // VM-only
    u64* registers;         // VM-only
    // ...
};
```

In **VM mode**, the interpreter calls `roxy_set_ctx(&vm->ctx)` before entering Roxy code. In **AOT mode**, the embedder calls `roxy_set_ctx()` at the entry point. All generated code, runtime functions, and native functions access the context via `roxy_get_ctx()` when needed (e.g., for allocation or exception handling).

The runtime header also provides a C++ RAII guard for scoped context management:

```cpp
// roxy_rt.h
namespace roxy {

class ScopedContext {
    roxy_ctx* m_prev;
public:
    explicit ScopedContext(roxy_ctx* ctx) : m_prev(roxy_get_ctx()) { roxy_set_ctx(ctx); }
    ~ScopedContext() { roxy_set_ctx(m_prev); }

    ScopedContext(const ScopedContext&) = delete;
    ScopedContext& operator=(const ScopedContext&) = delete;
};

} // namespace roxy
```

### Native Function Signatures

Since the runtime context is thread-local, native functions do not take `roxy_ctx*` as a parameter. They are plain C++ functions with only their logical parameters:

```cpp
// These work in BOTH VM mode and AOT mode
i32 my_add(i32 a, i32 b) { return a + b; }
f64 my_sqrt(f64 x) { return std::sqrt(x); }

roxy::String greet(roxy::String name) {
    return roxy::String::alloc("hello ").concat(name);
}

i32 point_sum(Point* self) { return self->x + self->y; }

// Native functions that need the context (e.g., for user_data) call roxy_get_ctx():
void spawn_enemy(f32 x, f32 y) {
    auto* engine = (GameEngine*)roxy_get_ctx()->user_data;
    engine->spawn_at(x, y);
}
```

### How Each Binding Category Maps

| Binding Style | VM Mode | AOT Mode |
|---|---|---|
| `bind<FnPtr>("name")` | `FunctionBinder` wraps FnPtr, VM sets TLS ctx | Emit direct call to FnPtr |
| `bind_native(fn, sig)` | Calls `fn(vm, dst, argc, first_arg)` | Needs AOT-compatible overload (see below) |
| `bind_method<FnPtr>(...)` | Same as bind — binder wraps | Emit direct call with `self*` |
| `bind_method(fn, sig)` | VM-style manual wrapper | Needs AOT-compatible overload |
| Built-in (print, list, etc.) | VM native functions | `roxy_rt` C functions (use TLS ctx internally) |

#### Auto-bound functions (`bind<FnPtr>`)

This is the cleanest path. The template already captures the real C++ function pointer, and the emitter references it directly:

```cpp
// Embedder code
i32 my_add(i32 a, i32 b) { return a + b; }
registry.bind<my_add>("add");
```

```cpp
// Generated .cpp — direct call, no wrapper overhead
int32_t v2 = my_add(v0, v1);
```

The `FunctionBinder` template stores both the VM wrapper (for interpreter use) and the original function pointer (for the C emitter to reference by symbol name). In VM mode, the binder sets the thread-local context before invoking the native function.

#### Signature-bound functions (`bind_native`)

For functions registered with the signature-based API, only a `NativeFunction` (VM-specific wrapper) is provided. To support AOT, a new overload accepts both:

```cpp
// New API: provide both VM wrapper and AOT-compatible function
registry.bind_native(vm_wrapper, aot_function, "fun foo(a: i32, b: i32): i32");

// Or: migrate to bind<FnPtr> which handles both automatically
registry.bind<my_foo>("foo");  // preferred
```

The signature-only `bind_native(fn, sig)` overload remains for VM-only use. Functions registered without an AOT-compatible version will produce a compile error if the C emitter encounters a `CallNative` referencing them.

#### Built-in runtime functions

Built-in natives (print, string ops, list ops, map ops) are provided by the `roxy_rt` runtime library. They access the context via `roxy_get_ctx()` internally, so their public API is context-free:

```c
// roxy_rt.h
void roxy_print(roxy_string* s);
roxy_string* roxy_string_concat(roxy_string* a, roxy_string* b);
roxy_list* roxy_list_new(int32_t capacity);
void roxy_list_push(roxy_list* list, uint64_t value);
```

The C emitter maps built-in native names to their `roxy_rt` equivalents:

| Native Name | C Identifier |
|---|---|
| `print` | `roxy_print` |
| `str_concat` | `roxy_string_concat` |
| `str_eq` | `roxy_string_eq` |
| `List$$push` (monomorphized) | `roxy_list_push` |
| `Map$$insert` (monomorphized) | `roxy_map_insert` |

#### Native struct methods

Native methods on structs are called with the `self` pointer as a typed argument:

```cpp
// Embedder code
i32 point_product(Point* self) { return self->x * self->y; }
registry.bind_method<point_product>("Point", "product");
```

```cpp
// Generated .cpp
int32_t v2 = point_product(v0);  // v0 is Point*
```

### Generic Native Types (List, Map) in AOT

Generic native types like `List<T>` and `Map<K, V>` are type-erased at runtime — all Roxy values are 64 bits, so a single implementation handles all element types. The same approach works in AOT:

```cpp
// Generated .cpp for List<i32> operations
roxy_list* v0 = roxy_list_new(16);              // capacity 16
roxy_list_push(v0, (uint64_t)42);               // push i32 (widened to u64)
int32_t v1 = (int32_t)roxy_list_get(v0, 0);    // get and narrow back to i32

// Generated .cpp for Map<string, i32> operations
roxy_map* v2 = roxy_map_new(ROXY_KEY_STRING, 16);
roxy_map_insert(v2, (uint64_t)key_str, (uint64_t)42);
int32_t v3 = (int32_t)roxy_map_get(v2, (uint64_t)key_str);
```

The `uint64_t` casts at the call boundary match the VM's type-erased value representation. The C++ compiler optimizes these to no-ops on 64-bit platforms for integer types.

### AOT Entry Point

The generated `.cpp` file includes a `main` function that initializes the runtime context, sets the thread-local, and calls the Roxy `main` entry point:

```cpp
// Generated at the bottom of the .cpp file
int main(int argc, char** argv) {
    roxy_ctx ctx;
    roxy_ctx_init(&ctx);
    roxy_set_ctx(&ctx);
    int32_t result = main_entry();
    roxy_ctx_destroy(&ctx);
    return result;
}
```

For library use (embedding without a standalone binary), the embedder creates the context and uses the scoped guard:

```cpp
// Embedder code
roxy_ctx ctx;
roxy_ctx_init(&ctx);
ctx.user_data = &my_game_engine;

{
    roxy::ScopedContext guard(&ctx);

    // Call exported Roxy functions directly — no ctx parameter needed
    int32_t score = calculate_score(player_ptr);
    update_world(dt);
}

roxy_ctx_destroy(&ctx);
```

### Object Lifecycle

```
// Heap allocation
v1 = new "Point" [args...]
→
Point* v1 = (Point*)roxy_alloc(sizeof(Point), TYPEID_Point);
Point__new(v1, args...);  // constructor call (if any)

// Deallocation
delete v0
→
Point__delete(v0);   // destructor call (if any)
roxy_free(v0);
```

### Reference Counting

```
ref_inc v0    →  roxy_ref_inc(v0);
ref_dec v0    →  roxy_ref_dec(v0);
weak_check v0 →  bool v1 = roxy_weak_valid(v0.ptr, v0.generation);
```

### Pointer Operations

```
v1 = load_ptr v0          →  int32_t v1 = *v0;    // type from IRInst.type
store_ptr v0, v1           →  *v0 = v1;
v1 = var_addr "x"          →  int32_t* v1 = &x;
```

## Tagged Unions

Tagged unions emit a C struct with a discriminant field and a union:

```roxy
enum SkillType { Attack, Defend }
struct AttackData { damage: i32; crit_chance: f32; }
struct DefendData { damage_reduce: f32; }

struct Skill {
    name: string;
    when type: SkillType {
        case Attack: attack: AttackData;
        case Defend: defend: DefendData;
    }
}
```

```c
typedef struct {
    roxy_string* name;
    SkillType type;
    union {
        AttackData attack;
        DefendData defend;
    };
} Skill;
```

The `when` statement compiles to a chain of comparisons (same as bytecode lowering), which maps to `if`/`else if` in C:

```c
if (skill->type == SkillType_Attack) {
    // case Attack
} else if (skill->type == SkillType_Defend) {
    // case Defend
}
```

For larger enums, a `switch` statement could be emitted instead.

## Runtime Library (`roxy_rt.h`)

The runtime provides C implementations of features that require allocation or complex logic. Functions that need the runtime context (for allocation, etc.) access it internally via `roxy_get_ctx()`. The public API is context-free.

### Context Lifecycle

```c
// Initialize/destroy the AOT runtime context
void roxy_ctx_init(roxy_ctx* ctx);
void roxy_ctx_destroy(roxy_ctx* ctx);

// Thread-local context access
void roxy_set_ctx(roxy_ctx* ctx);
roxy_ctx* roxy_get_ctx(void);
```

### Core

```c
// Object allocation/deallocation (use roxy_get_ctx() internally)
void* roxy_alloc(uint32_t data_size, uint32_t type_id);
void roxy_free(void* data);

// Reference counting
void roxy_ref_inc(void* data);
void roxy_ref_dec(void* data);  // frees if count reaches 0 + no uniq owner

// Weak references
roxy_weak roxy_weak_create(void* data);
bool roxy_weak_valid(void* ptr, uint64_t generation);
uint64_t roxy_weak_generation(void* data);  // read generation from ObjectHeader
```

### Strings

```c
// String operations (allocating functions use roxy_get_ctx() internally)
roxy_string* roxy_string_from_literal(const char* data, uint32_t length);
roxy_string* roxy_string_concat(roxy_string* a, roxy_string* b);
bool roxy_string_eq(roxy_string* a, roxy_string* b);
int32_t roxy_string_len(roxy_string* s);
const char* roxy_string_chars(roxy_string* s);
void roxy_string_print(roxy_string* s);

// to_string conversions (for f-string interpolation)
roxy_string* roxy_i32_to_string(int32_t v);
roxy_string* roxy_i64_to_string(int64_t v);
roxy_string* roxy_f32_to_string(float v);
roxy_string* roxy_f64_to_string(double v);
roxy_string* roxy_bool_to_string(bool v);
```

### Lists

```c
roxy_list* roxy_list_new(int32_t capacity);
int32_t roxy_list_len(roxy_list* list);
int32_t roxy_list_cap(roxy_list* list);
void roxy_list_push(roxy_list* list, uint64_t value);
uint64_t roxy_list_pop(roxy_list* list);
uint64_t roxy_list_get(roxy_list* list, int32_t index);
void roxy_list_set(roxy_list* list, int32_t index, uint64_t value);
```

### Maps

```c
roxy_map* roxy_map_new(int32_t key_kind, int32_t capacity);
int32_t roxy_map_len(roxy_map* map);
void roxy_map_insert(roxy_map* map, uint64_t key, uint64_t value);
uint64_t roxy_map_get(roxy_map* map, uint64_t key);
bool roxy_map_contains(roxy_map* map, uint64_t key);
bool roxy_map_remove(roxy_map* map, uint64_t key);
void roxy_map_clear(roxy_map* map);
roxy_list* roxy_map_keys(roxy_map* map);
roxy_list* roxy_map_values(roxy_map* map);
```

### I/O

```c
void roxy_print(roxy_string* s);
```

### Runtime RAII Templates

The runtime header provides C++ class templates for Roxy's reference types. These are used by the generated header's factory functions to give the embedder idiomatic C++ ownership semantics.

#### `roxy::uniq<T>` — Unique Ownership

Maps to Roxy's `uniq T`. Owns the allocation, calls the destructor and frees on scope exit. Move-only (no copy). Uses `roxy_get_ctx()` internally — no stored context pointer.

```cpp
// roxy_rt.h
namespace roxy {

typedef void (*destructor_fn)(void*);

template<typename T>
class uniq {
    T* m_ptr;
    destructor_fn m_destructor;

public:
    uniq(T* ptr, void (*dtor)(T*))
        : m_ptr(ptr)
        , m_destructor(reinterpret_cast<destructor_fn>(dtor)) {}

    ~uniq() {
        if (m_ptr) {
            if (m_destructor) m_destructor(m_ptr);
            roxy_free(m_ptr);
        }
    }

    // Move semantics
    uniq(uniq&& other) noexcept
        : m_ptr(other.m_ptr), m_destructor(other.m_destructor) {
        other.m_ptr = nullptr;
    }
    uniq& operator=(uniq&& other) noexcept {
        if (this != &other) {
            if (m_ptr) { if (m_destructor) m_destructor(m_ptr); roxy_free(m_ptr); }
            m_ptr = other.m_ptr; m_destructor = other.m_destructor;
            other.m_ptr = nullptr;
        }
        return *this;
    }

    // No copy
    uniq(const uniq&) = delete;
    uniq& operator=(const uniq&) = delete;

    // Access
    T* get() const { return m_ptr; }
    T* operator->() const { return m_ptr; }
    T& operator*() const { return *m_ptr; }
    explicit operator bool() const { return m_ptr != nullptr; }

    // Release ownership (caller takes responsibility)
    T* release() { T* p = m_ptr; m_ptr = nullptr; return p; }
};

} // namespace roxy
```

#### `roxy::ref<T>` — Shared Reference

Maps to Roxy's `ref T`. Ref-counted, copyable. The last copy to be destroyed frees the object. Uses `roxy_get_ctx()` internally.

```cpp
// roxy_rt.h
namespace roxy {

template<typename T>
class ref {
    T* m_ptr;

public:
    explicit ref(T* ptr) : m_ptr(ptr) {
        if (m_ptr) roxy_ref_inc(m_ptr);
    }

    ~ref() {
        if (m_ptr) roxy_ref_dec(m_ptr);
    }

    // Copy (increments ref count)
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

    // Move (no ref count change)
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

    // Access
    T* get() const { return m_ptr; }
    T* operator->() const { return m_ptr; }
    T& operator*() const { return *m_ptr; }
};

} // namespace roxy
```

#### `roxy::weak<T>` — Weak Reference

Maps to Roxy's `weak T`. Non-owning, nullable. Stores a pointer and a generation counter for dangling detection. Does not affect ref count or ownership.

```cpp
// roxy_rt.h
namespace roxy {

template<typename T>
class weak {
    T* m_ptr;
    uint64_t m_generation;

public:
    weak() : m_ptr(nullptr), m_generation(0) {}

    // Create from a live uniq or ref
    explicit weak(T* ptr)
        : m_ptr(ptr)
        , m_generation(roxy_weak_generation(ptr)) {}

    // Check if the referenced object is still alive
    bool valid() const {
        return m_ptr && roxy_weak_valid(m_ptr, m_generation);
    }

    // Access (asserts if dangling)
    T* lock() const {
        assert(valid() && "weak reference is dangling");
        return m_ptr;
    }

    T* lock_or_null() const {
        return valid() ? m_ptr : nullptr;
    }

    // Copyable (no ownership, no ref count)
    weak(const weak&) = default;
    weak& operator=(const weak&) = default;
    weak(weak&&) = default;
    weak& operator=(weak&&) = default;
};

} // namespace roxy
```

The generated header uses these templates in factory functions — see [Generated Header](#generated-header-scriptshpp) for examples.

### Container and String Wrappers

The runtime header also provides typed C++ wrappers for Roxy's built-in container and string types. These are thin non-owning facades over the type-erased C runtime functions (`roxy_string_*`, `roxy_list_*`, `roxy_map_*`).

These are refactored from the existing VM binding wrappers (`rx::RoxyString`, `rx::RoxyList<T>`, `rx::RoxyMap<K, V>` in `include/roxy/vm/binding/`). The key change is replacing `RoxyVM*` with `roxy_ctx*` and calling the `roxy_rt` C functions instead of the VM-internal free functions. After the refactor, the VM binding wrappers become thin aliases to the `roxy::` versions.

#### `roxy::String`

```cpp
// roxy_rt.h
namespace roxy {

class String {
    void* m_data;

public:
    explicit String(void* data) : m_data(data) {}

    // Factory (uses roxy_get_ctx() internally for allocation)
    static String alloc(const char* data, uint32_t length) {
        return String(roxy_string_from_literal(data, length));
    }
    static String alloc(const char* data) {
        return alloc(data, (uint32_t)strlen(data));
    }

    // Access
    uint32_t length() const { return roxy_string_len((roxy_string*)m_data); }
    const char* c_str() const { return roxy_string_chars((roxy_string*)m_data); }

    // Operations
    bool equals(const String& other) const {
        return roxy_string_eq((roxy_string*)m_data, (roxy_string*)other.m_data);
    }
    String concat(String other) const {
        return String(roxy_string_concat((roxy_string*)m_data, (roxy_string*)other.m_data));
    }

    bool is_valid() const { return m_data != nullptr; }
    void* data() const { return m_data; }
};

} // namespace roxy
```

#### `roxy::List<T>`

```cpp
// roxy_rt.h
namespace roxy {

template<typename T>
class List {
    void* m_data;

public:
    explicit List(void* data) : m_data(data) {}

    // Factory
    static List<T> alloc(int32_t capacity) {
        return List<T>(roxy_list_new(capacity));
    }

    // Element access (bounds-checked)
    T get(int32_t index) const {
        return (T)roxy_list_get((roxy_list*)m_data, index);
    }
    void set(int32_t index, T value) {
        roxy_list_set((roxy_list*)m_data, index, (uint64_t)value);
    }

    // Mutation
    void push(T value) {
        roxy_list_push((roxy_list*)m_data, (uint64_t)value);
    }
    T pop() {
        return (T)roxy_list_pop((roxy_list*)m_data);
    }

    // Info
    int32_t len() const { return roxy_list_len((roxy_list*)m_data); }
    int32_t cap() const { return roxy_list_cap((roxy_list*)m_data); }

    bool is_valid() const { return m_data != nullptr; }
    void* data() const { return m_data; }
};

} // namespace roxy
```

#### `roxy::Map<K, V>`

```cpp
// roxy_rt.h
namespace roxy {

template<typename K, typename V>
class Map {
    void* m_data;

public:
    explicit Map(void* data) : m_data(data) {}

    // Factory
    static Map<K, V> alloc(int32_t key_kind, int32_t capacity = 0) {
        return Map<K, V>(roxy_map_new(key_kind, capacity));
    }

    // Lookup
    V get(K key) const {
        return (V)roxy_map_get((roxy_map*)m_data, (uint64_t)key);
    }
    bool contains(K key) const {
        return roxy_map_contains((roxy_map*)m_data, (uint64_t)key);
    }

    // Mutation
    void insert(K key, V value) {
        roxy_map_insert((roxy_map*)m_data, (uint64_t)key, (uint64_t)value);
    }
    bool remove(K key) {
        return roxy_map_remove((roxy_map*)m_data, (uint64_t)key);
    }
    void clear() {
        roxy_map_clear((roxy_map*)m_data);
    }

    // Bulk access
    List<K> keys() const {
        return List<K>(roxy_map_keys((roxy_map*)m_data));
    }
    List<V> values() const {
        return List<V>(roxy_map_values((roxy_map*)m_data));
    }

    // Info
    int32_t len() const { return roxy_map_len((roxy_map*)m_data); }

    bool is_valid() const { return m_data != nullptr; }
    void* data() const { return m_data; }
};

} // namespace roxy
```

#### Refactoring Plan

The existing VM binding wrappers (`rx::RoxyString`, `rx::RoxyList<T>`, `rx::RoxyMap<K, V>`) are refactored in two steps:

1. **Move to `roxy_rt.h`**: The wrapper logic moves to `roxy::String`, `roxy::List<T>`, `roxy::Map<K, V>` in the runtime header. These call the `roxy_rt` C functions and take `roxy_ctx*`.

2. **VM binding aliases**: The existing `rx::RoxyString`, `rx::RoxyList<T>`, `rx::RoxyMap<K, V>` become thin aliases that forward to the `roxy::` versions. The `RoxyType<T>` specializations remain in the VM binding layer since they depend on `TypeCache`.

```cpp
// include/roxy/vm/binding/roxy_string.hpp (after refactor)
namespace rx {
    using RoxyString = roxy::String;
    // RoxyType<RoxyString> specialization stays here
}
```

This way existing embedder code using `rx::RoxyString` continues to work, while new code can use `roxy::String` directly. Both VM and AOT paths share the same wrapper implementations.

### Memory Management Strategy

Two options for the runtime allocator:

1. **Simplified `malloc`-based**: Use `malloc`/`free` with `ObjectHeader` prepended. Simple, portable, good starting point.
2. **Full slab allocator**: Port the existing slab allocator from `vm/slab_allocator.hpp` to C. Better performance and tombstoning semantics for weak references.

**Recommendation:** Start with `malloc`-based. The slab allocator can be ported later for production use.

With `malloc`-based allocation, tombstoning for weak references requires keeping freed objects mapped. A simple approach: on `roxy_free()`, zero the `weak_generation` but don't call `free()` immediately — defer to a sweep phase or accept the leak for short-lived programs.

## Emitter Architecture

### CEmitter Class

```cpp
struct CEmitterConfig {
    // Headers to #include in the generated .cpp for native function access.
    // These are the embedder's headers that declare native functions.
    // Example: {"engine/native_bindings.hpp", "engine/types.hpp"}
    Vector<String> native_include_paths;

    // Whether to emit a standalone main() entry point.
    // Set to false for library mode (embedder calls exported functions directly).
    bool emit_main_entry = true;
};

class CEmitter {
public:
    CEmitter(BumpAllocator& alloc, const NativeRegistry* registry,
             const CEmitterConfig& config);

    // Emit the .cpp implementation file
    void emit_source(const IRModule* module, Vector<char>& output);

    // Emit the .hpp public header (pub structs, enums, and pub function declarations)
    void emit_header(const IRModule* module, Vector<char>& output);

private:
    const NativeRegistry* m_registry;
    CEmitterConfig m_config;

    // Type emission
    void emit_forward_declarations(const IRModule* module);
    void emit_struct_typedefs(const IRModule* module);
    void emit_enum_typedefs(const IRModule* module);
    void emit_function_prototypes(const IRModule* module);
    void emit_native_declarations(const IRModule* module);

    // Function emission
    void emit_function(const IRFunction* func);
    void emit_block(const IRBlock* block, const IRFunction* func);
    void emit_instruction(const IRInst* inst);
    void emit_terminator(const IRBlock* block);

    // Helpers
    void emit_type(Type* type);        // Emit C type name
    void emit_value(ValueId id);       // Emit value reference (e.g., "v42")
    void emit_mangled_name(StringView name);  // $$ → __
    void emit_native_call(const IRInst* inst);  // Emit native function call
};
```

### Output Structure

The CEmitter produces two files: a `.hpp` public header and a `.cpp` implementation.

#### Generated Header (`scripts.hpp`)

The header contains `pub` struct/enum types and `pub` function declarations. This is what the embedder `#include`s to call Roxy functions from C++.

For `pub` structs with methods, the header emits inline C++ method wrappers so the embedder can use natural `obj.method(ctx, ...)` syntax instead of calling mangled free functions. For structs that can be heap-allocated via `uniq`/`ref`, the header also emits typed factory functions that return `roxy::uniq<T>` / `roxy::ref<T>` RAII wrappers (provided by `roxy_rt.h` — see [Runtime RAII Templates](#runtime-raii-templates)).

```cpp
/* Generated by Roxy C Backend */
#pragma once
#include "roxy_rt.h"

/* Mangled function forward declarations (called by inline wrappers) */
int32_t Point__sum(Point* self);
int32_t Point__scaled(Point* self, int32_t factor);

void Player__take_damage(Player* self, int32_t amount);
bool Player__is_alive(Player* self);
void Player__new(Player* self, roxy_string* name, int32_t health);
void Player__delete(Player* self);

/* Enum definitions */
typedef enum { Color_Red = 0, Color_Green = 1, Color_Blue = 2 } Color;

/* Struct definitions with C++ method wrappers */

struct Point {
    int32_t x;
    int32_t y;

    // Inline method wrappers (zero overhead — same codegen as calling free functions)
    int32_t sum() { return Point__sum(this); }
    int32_t scaled(int32_t factor) { return Point__scaled(this, factor); }
};

struct Player {
    roxy_string* name;
    int32_t health;

    void take_damage(int32_t amount) { Player__take_damage(this, amount); }
    bool is_alive() { return Player__is_alive(this); }
};

/* RAII factory functions for heap-allocated types */

// For: var p: uniq Player = new Player("hero", 100);
inline roxy::uniq<Player> make_Player(roxy_string* name, int32_t health) {
    Player* ptr = (Player*)roxy_alloc(sizeof(Player), TYPEID_Player);
    Player__new(ptr, name, health);
    return roxy::uniq<Player>(ptr, Player__delete);
}

/* Public free function declarations */
int32_t calculate_score(Point* v0);
void update_player(Player* v0, float v1);
int32_t main_entry();
```

The embedder uses this naturally from C++:

```cpp
#include "scripts.hpp"

void game_tick() {
    // Value-type struct — stack-allocated, method wrappers
    Point p = {3, 4};
    int32_t s = p.sum();
    int32_t t = p.scaled(10);

    // Heap-allocated with RAII — automatically freed at scope exit
    auto player = make_Player(roxy_string_from_literal("hero", 4), 100);
    player->take_damage(25);
    if (player->is_alive()) { /* ... */ }
    // ~roxy::uniq<Player> calls Player__delete + roxy_free automatically
}
```

Only types and functions marked `pub` in the Roxy source appear in the header. Non-`pub` items are internal to the `.cpp`.

#### Generated Source (`scripts.cpp`)

```cpp
/* Generated by Roxy C Backend */
#include "scripts.hpp"

/* Embedder native function headers (from CEmitterConfig::native_include_paths) */
#include "engine/native_bindings.hpp"

/* Native function declarations (from NativeRegistry) */
extern int32_t my_add(int32_t a, int32_t b);
extern int32_t point_product(Point* self);

/* Internal (non-pub) function prototypes */
static int32_t Point__sum(Point* self);
static int32_t helper(int32_t v0);

/* Function definitions */
static int32_t Point__sum(Point* self) {
    int32_t v1 = self->x;
    int32_t v2 = self->y;
    int32_t v3 = v1 + v2;
    return v3;
}

int32_t calculate_score(Point* v0) {
    // ... pub function — external linkage ...
}

// ... more function definitions ...

/* Entry point (standalone binary mode, optional) */
int main(int argc, char** argv) {
    roxy_ctx ctx;
    roxy_ctx_init(&ctx);
    roxy_set_ctx(&ctx);
    int32_t result = main_entry();
    roxy_ctx_destroy(&ctx);
    return result;
}
```

The `.cpp` includes its own `.hpp` header (which brings in `roxy_rt.h` and the public type/function declarations), then adds the embedder's native headers and internal definitions. Functions without `pub` are emitted as `static`. No `roxy_ctx*` parameter is passed — the context is accessed via thread-local storage.

## Build Integration

### Game Engine Example

A typical game engine project using Roxy with the C backend:

```
my_game/
├── engine/
│   ├── engine.hpp              # Game engine types
│   ├── engine.cpp
│   └── native_bindings.hpp     # Native function definitions (use roxy_get_ctx() for engine access)
├── scripts/
│   ├── player.roxy             # Roxy game scripts
│   └── enemy.roxy
├── generated/                  # Output from CEmitter (gitignored)
│   ├── scripts.hpp             # Public API: pub structs + pub function declarations
│   └── scripts.cpp             # Implementation: all function bodies
└── CMakeLists.txt
```

**engine/native_bindings.hpp** — the embedder's native functions:

```cpp
#pragma once
#include "engine.hpp"
#include <roxy/rt/roxy_rt.h>

struct Vec2 { float x; float y; };

inline Vec2 get_position(Entity* entity) {
    auto* engine = (GameEngine*)roxy_get_ctx()->user_data;
    return engine->get_position(entity);
}

inline void apply_damage(Entity* target, int32_t amount) {
    auto* engine = (GameEngine*)roxy_get_ctx()->user_data;
    engine->damage(target, amount);
}
```

**scripts/player.roxy**:

```roxy
pub struct Stats {
    health: i32;
    armor: i32;

    pub fun effective_health(): i32 {
        return self.health + self.armor * 2;
    }
}

pub fun new Stats(health: i32, armor: i32) {
    self.health = health;
    self.armor = armor;
}

pub fun update_player(entity: ref Entity, dt: f32) {
    var pos = get_position(entity);
    if pos.x > 100.0 {
        apply_damage(entity, 10);
    }
}
```

**generated/scripts.hpp** — generated by CEmitter:

```cpp
/* Generated by Roxy C Backend — do not edit */
#pragma once
#include "roxy_rt.h"

/* Mangled function forward declarations */
int32_t Stats__effective_health(Stats* self);
void Stats__new(Stats* self, int32_t health, int32_t armor);

/* Struct definitions with C++ method wrappers */
struct Stats {
    int32_t health;
    int32_t armor;

    int32_t effective_health() { return Stats__effective_health(this); }

    // Constructor wrapper
    static Stats create(int32_t health, int32_t armor) {
        Stats self = {0};
        Stats__new(&self, health, armor);
        return self;
    }
};

/* Public free function declarations */
void update_player(Entity* v0, float v1);
```

**generated/scripts.cpp** — generated by CEmitter:

```cpp
/* Generated by Roxy C Backend — do not edit */
#include "scripts.hpp"
#include "engine/native_bindings.hpp"

extern Vec2 get_position(Entity* entity);
extern void apply_damage(Entity* target, int32_t amount);

int32_t Stats__effective_health(Stats* self) {
    int32_t v1 = self->health;
    int32_t v2 = self->armor;
    int32_t v3 = v2 * 2;
    int32_t v4 = v1 + v3;
    return v4;
}

void Stats__new(Stats* self, int32_t v1, int32_t v2) {
    self->health = v1;
    self->armor = v2;
}

void update_player(Entity* v0, float v1) {
    Vec2 v2_struct = {0}; Vec2* v2 = &v2_struct;
    v2_struct = get_position(v0);
    float v3 = v2->x;
    bool v4 = (v3 > 100.0f);
    if (v4) goto then_0; else goto end_0;
then_0:
    apply_damage(v0, 10);
end_0:;
}
```

**engine/engine.cpp** — the embedder uses the generated header with natural C++ syntax:

```cpp
#include "engine.hpp"
#include "generated/scripts.hpp"  // just #include — no manual extern declarations

void GameEngine::tick(float dt) {
    // Set up the thread-local context (typically done once at engine startup)
    roxy::ScopedContext guard(&m_roxy_ctx);

    // Value-type struct with method wrappers — no ctx needed!
    auto stats = Stats::create(100, 30);
    int32_t ehp = stats.effective_health();  // natural C++ method call

    for (auto* entity : m_entities) {
        update_player(entity, dt);  // direct C++ call
    }
}
```

### CMake Integration

The Roxy compiler runs as a custom command before the main build:

```cmake
# Build step 1: Roxy source → generated .hpp/.cpp
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/generated/scripts.hpp
           ${CMAKE_BINARY_DIR}/generated/scripts.cpp
    COMMAND roxy_compiler
        --backend=c
        --output-dir=${CMAKE_BINARY_DIR}/generated
        --native-includes="engine/native_bindings.hpp"
        scripts/player.roxy scripts/enemy.roxy
    DEPENDS scripts/player.roxy scripts/enemy.roxy
    COMMENT "Compiling Roxy scripts to C++"
)

# Build step 2: compile everything together
add_executable(my_game
    engine/engine.cpp
    ${CMAKE_BINARY_DIR}/generated/scripts.cpp
)
target_link_libraries(my_game roxy_rt)
target_include_directories(my_game PRIVATE ${CMAKE_BINARY_DIR}/generated)
```

The `--native-includes` flag maps to `CEmitterConfig::native_include_paths`, telling the emitter which headers to `#include` in the generated `.cpp` for native function access.

## Implementation Phases

### Phase 1: Scaffold + Primitives

- [ ] Create `include/roxy/compiler/c_emitter.hpp` and `src/roxy/compiler/c_emitter.cpp`
- [ ] Implement `CEmitterConfig` (native include paths, emit main entry toggle)
- [ ] Implement `emit_header()` — emit `.hpp` with pub structs, enums, and pub function declarations
- [ ] Implement `emit_source()` — emit `.cpp` with includes, native declarations, all function bodies
- [ ] Add `roxy_compiler` library dependency in CMakeLists.txt
- [ ] Emit C type names for all primitive `TypeKind` variants
- [ ] Emit constants: `ConstInt`, `ConstBool`, `ConstF`, `ConstD`, `ConstNull`
- [ ] Emit arithmetic operations: `AddI`, `SubI`, `MulI`, `DivI`, `ModI`, `NegI`, and f32/f64 variants
- [ ] Emit comparison operations: `EqI`, `NeI`, `LtI`, `LeI`, `GtI`, `GeI`, and f32/f64 variants
- [ ] Emit logical/bitwise: `Not`, `And`, `Or`, `BitAnd`, `BitOr`, `BitNot`
- [ ] Emit type conversions: `I_TO_F64`, `F64_TO_I`, `I_TO_B`, `B_TO_I`, `Cast`
- [ ] Emit control flow: `Goto` → `goto`, `Branch` → `if-goto`, `Return` → `return`
- [ ] Emit block arguments as local variables with assignment before `goto`
- [ ] Emit function calls: `Call` → direct call
- [ ] Add basic E2E test: compile Roxy → C++ → g++/clang++ → run → check output

### Phase 2: Structs

- [ ] Emit `typedef struct` for all struct types (sorted by dependency order)
- [ ] Emit enum `typedef enum` for all enum types
- [ ] Handle `StackAlloc` → local struct variable + pointer
- [ ] Handle `GetField`/`SetField` → `ptr->field`
- [ ] Handle `GetFieldAddr` → `&ptr->field`
- [ ] Handle `StructCopy` → struct assignment or `memcpy`
- [ ] Handle struct function parameters and returns (small struct packing/unpacking)
- [ ] Handle large struct returns via hidden output pointer
- [ ] Handle struct inheritance (parent fields come first in C struct)
- [ ] Handle tagged unions → C struct with `union` member
- [ ] Emit inline C++ method wrappers on pub structs in `.hpp` header
- [ ] Emit static `create()` constructor wrapper for pub structs with named constructors
- [ ] E2E tests for structs, enums, tagged unions

### Phase 3: Runtime Library

- [ ] Create `roxy_rt.h` and `roxy_rt.c`
- [ ] Implement `roxy_alloc`/`roxy_free` (malloc-based with `ObjectHeader`)
- [ ] Implement `roxy_ref_inc`/`roxy_ref_dec`
- [ ] Implement `roxy_weak_create`/`roxy_weak_valid`
- [ ] Implement `roxy::uniq<T>` template (move-only, destructor + free on scope exit)
- [ ] Implement `roxy::ref<T>` template (ref-counted, copyable)
- [ ] Implement `roxy::weak<T>` template (non-owning, generation-checked validity)
- [ ] Implement `roxy::String` wrapper (refactored from `rx::RoxyString`)
- [ ] Implement `roxy::List<T>` wrapper (refactored from `rx::RoxyList<T>`)
- [ ] Implement `roxy::Map<K, V>` wrapper (refactored from `rx::RoxyMap<K, V>`)
- [ ] Refactor `rx::RoxyString` / `rx::RoxyList<T>` / `rx::RoxyMap<K, V>` to alias `roxy::` versions
- [ ] Implement string C functions (`roxy_string_from_literal`, `roxy_string_concat`, `roxy_string_eq`, etc.)
- [ ] Implement `to_string` conversions for primitives
- [ ] Implement list C functions (`roxy_list_new`, `roxy_list_push`, `roxy_list_pop`, etc.)
- [ ] Implement map C functions (`roxy_map_new`, `roxy_map_insert`, `roxy_map_get`, etc.)
- [ ] Implement print functions
- [ ] Handle `New`/`Delete` IR ops → `roxy_alloc`/`roxy_free` + constructor/destructor calls
- [ ] Handle `RefInc`/`RefDec`/`WeakCheck` IR ops
- [ ] Handle `ConstString` → `roxy_string_from_literal`
- [ ] Handle `CallNative` → dispatch to runtime functions
- [ ] Emit `roxy::uniq<T>` factory functions in `.hpp` for pub structs usable with `uniq`
- [ ] Emit `roxy::ref<T>` factory functions in `.hpp` for pub structs usable with `ref`
- [ ] E2E tests for strings, lists, heap allocation, ref counting, RAII wrappers

### Phase 4: Native Function Integration

- [ ] Define `roxy_ctx` struct in `roxy_rt.h` with allocator, exception state, user data fields
- [ ] Implement thread-local `roxy_set_ctx`/`roxy_get_ctx` in runtime library
- [ ] Implement `roxy::ScopedContext` RAII guard
- [ ] Add `roxy_ctx ctx` as first member of `RoxyVM` for compatibility
- [ ] Implement `roxy_ctx_init`/`roxy_ctx_destroy` in runtime library
- [ ] Update VM interpreter to call `roxy_set_ctx(&vm->ctx)` before entering Roxy code
- [ ] Update `FunctionBinder` to set thread-local ctx instead of passing `vm` to native functions
- [ ] Store AOT function pointer/symbol name in `NativeRegistry` entries for `bind<FnPtr>` bindings
- [ ] Add `bind_native(vm_fn, aot_fn, sig)` overload for dual VM/AOT registration
- [ ] Emit `extern` declarations for user-registered native functions in generated `.cpp`
- [ ] Handle `CallNative` for auto-bound natives → direct typed call to C++ function
- [ ] Handle `CallNative` for built-in natives → call `roxy_rt` functions
- [ ] Handle native struct methods → direct call with typed `self*` parameter
- [ ] Handle generic native type methods (List/Map) → call type-erased `roxy_rt` functions with `uint64_t` casts
- [ ] Cross-module calls (`CallExternal`) → already resolved during linking, emit as regular calls
- [ ] Emit standalone `main()` entry point (optional, for standalone binary mode)
- [ ] E2E tests for native function calls (both auto-bound and built-in)

### Phase 5: Polish

- [ ] Emit `#line` directives mapping back to Roxy source (for debugger support)
- [ ] Handle `LoadPtr`/`StorePtr`/`VarAddr` for out/inout parameters
- [ ] Dead code elimination (skip unreachable blocks)
- [ ] Optional: structured control flow reconstruction (Relooper/Stackifier)
- [ ] Optional: emit `switch` for large enum `when` statements
- [ ] Optional: emit readable variable names instead of `v0`, `v1` (use debug names from `BlockParam.name`)

## Testing Strategy

C backend tests should mirror existing E2E tests:

```cpp
// In tests/e2e/test_c_backend.cpp
TEST_CASE("C Backend - Basics") {
    const char* source = R"(
        fun main(): i32 {
            var x: i32 = 10;
            var y: i32 = 20;
            return x + y;
        }
    )";

    // Compile Roxy → SSA IR → C++ source
    auto cpp_source = compile_to_cpp(source);

    // Write to temp file, compile with C++ compiler, run, check output
    auto result = compile_and_run_cpp(cpp_source);
    CHECK(result.exit_code == 30);
}
```

Helper functions:
- `compile_to_cpp(source)` — runs Roxy frontend + CEmitter, returns C++ string
- `compile_and_run_cpp(cpp_source)` — writes `.cpp`, invokes `c++`, links with `roxy_rt`, runs binary, returns exit code + stdout

## Name Mangling in C

The `$$` separator used internally is not valid in C identifiers. The emitter translates:

| Roxy Mangled Name | C Identifier |
|-------------------|--------------|
| `Point$$sum` | `Point__sum` |
| `Point$$new` | `Point__new` |
| `Point$$new$$from_coords` | `Point__new__from_coords` |
| `identity$i32` | `identity_i32` |
| `Box$i32` | `Box_i32` |
| `Pair$i32$f64` | `Pair_i32_f64` |

Rule: `$$` → `__`, `$` → `_`.

## Comparison with Bytecode Path

| Aspect | Bytecode + VM | C Backend |
|--------|---------------|-----------|
| Startup time | Fast (no compilation) | Slow (requires C++ compiler) |
| Runtime performance | Slower (interpreter overhead) | Fast (native code) |
| Portability | Anywhere VM runs | Anywhere a C++ compiler exists |
| Debugging | VM debugger | gdb/lldb with `#line` directives |
| Binary size | Small (VM + bytecode) | Larger (full native binary) |
| Hot reload | Possible (reload bytecode) | Requires recompilation |

The two paths complement each other: interpreter for development, C backend for shipping.

## Files

| File | Purpose |
|------|---------|
| `include/roxy/compiler/c_emitter.hpp` | CEmitter + CEmitterConfig class declarations |
| `src/roxy/compiler/c_emitter.cpp` | C emission implementation (`emit_header`, `emit_source`) |
| `include/roxy/rt/roxy_rt.h` | C runtime library header (`roxy_ctx`, runtime function declarations) |
| `src/roxy/rt/roxy_rt.c` | C runtime library implementation |
| `tests/e2e/test_c_backend.cpp` | E2E tests |
| *(generated)* `<name>.hpp` | Public API header: pub structs, enums, function declarations |
| *(generated)* `<name>.cpp` | Implementation: includes, native decls, all function bodies |

## Dependencies

The C backend requires all existing frontend components:
- Lexer, Parser, Semantic Analysis, IR Builder (already implemented)
- `NativeRegistry` — the CEmitter reads registered native function entries to emit declarations and direct calls
- Changes to existing code: `RoxyVM` gains a `roxy_ctx` first member; interpreter calls `roxy_set_ctx(&vm->ctx)` on entry; `FunctionBinder` no longer passes `vm`/`ctx` to native functions (they use `roxy_get_ctx()` if needed); native function signatures drop the `RoxyVM*`/`roxy_ctx*` first parameter