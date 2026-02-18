# C Backend (AOT Compilation)

> **Status:** Not yet implemented. This document is a design plan.

The C backend translates Roxy's SSA IR into C source code, which can then be compiled by any C compiler (gcc, clang, MSVC) for native performance.

## Pipeline

```
Source → Lexer → Parser → AST → Semantic Analysis → IR Builder → SSA IR
                                                                    ├→ Lowering → Bytecode → VM       (interpreter)
                                                                    └→ CEmitter → .c file → gcc/clang (AOT)
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

IR functions become C functions. Parameters are typed by their IR types:

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

```c
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

```
// Regular call
v2 = call "add" [v0, v1]           →  int32_t v2 = add(v0, v1);

// Method call (mangled name)
v2 = call "Point$$sum" [v1]        →  int32_t v2 = Point__sum(v1);

// External call (cross-module, resolved after linking)
v2 = call_external "math", "sin" [v0]  →  double v2 = math__sin(v0);

// Native call
v2 = call_native "print" [v1]      →  roxy_print(v1);
```

Note: `$$` in mangled names is replaced with `__` in C identifiers (since `$` is not standard C).

### Native Functions

Native functions called via `CallNative` need C-compatible wrappers or must be linked directly. Two approaches:

1. **Direct linking**: If the native function's C++ signature is compatible, call it directly.
2. **Wrapper functions**: Generate thin C wrappers that match the native function signature.

For builtin natives (print, string ops, list ops), the runtime library provides C-callable implementations.

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

### Index Operations (Lists)

```
v2 = get_index v0, v1    →  int64_t v2 = roxy_list_get(v0, v1);
set_index v0, v1, v2     →  roxy_list_set(v0, v1, v2);
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

The runtime provides C implementations of features that require allocation or complex logic:

### Core

```c
// Object allocation/deallocation
void* roxy_alloc(uint32_t data_size, uint32_t type_id);
void roxy_free(void* data);

// Reference counting
void roxy_ref_inc(void* data);
void roxy_ref_dec(void* data);  // frees if count reaches 0 + no uniq owner

// Weak references
roxy_weak roxy_weak_create(void* data);
bool roxy_weak_valid(void* ptr, uint64_t generation);
```

### Strings

```c
// String operations
roxy_string* roxy_string_from_literal(const char* data, uint32_t length);
roxy_string* roxy_string_concat(roxy_string* a, roxy_string* b);
bool roxy_string_eq(roxy_string* a, roxy_string* b);
int32_t roxy_string_len(roxy_string* s);
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

### I/O

```c
void roxy_print(roxy_string* s);
```

### Memory Management Strategy

Two options for the runtime allocator:

1. **Simplified `malloc`-based**: Use `malloc`/`free` with `ObjectHeader` prepended. Simple, portable, good starting point.
2. **Full slab allocator**: Port the existing slab allocator from `vm/slab_allocator.hpp` to C. Better performance and tombstoning semantics for weak references.

**Recommendation:** Start with `malloc`-based. The slab allocator can be ported later for production use.

With `malloc`-based allocation, tombstoning for weak references requires keeping freed objects mapped. A simple approach: on `roxy_free()`, zero the `weak_generation` but don't call `free()` immediately — defer to a sweep phase or accept the leak for short-lived programs.

## Emitter Architecture

### CEmitter Class

```cpp
class CEmitter {
public:
    CEmitter(BumpAllocator& alloc);

    // Main entry point: emit entire module as C source
    void emit(const IRModule* module, Vector<char>& output);

private:
    // Type emission
    void emit_forward_declarations(const IRModule* module);
    void emit_struct_typedefs(const IRModule* module);
    void emit_enum_typedefs(const IRModule* module);
    void emit_function_prototypes(const IRModule* module);

    // Function emission
    void emit_function(const IRFunction* func);
    void emit_block(const IRBlock* block, const IRFunction* func);
    void emit_instruction(const IRInst* inst);
    void emit_terminator(const IRBlock* block);

    // Helpers
    void emit_type(Type* type);        // Emit C type name
    void emit_value(ValueId id);       // Emit value reference (e.g., "v42")
    void emit_mangled_name(StringView name);  // $$ → __
};
```

### Output Structure

A single generated `.c` file with this layout:

```c
/* Generated by Roxy C Backend */
#include "roxy_rt.h"

/* Forward declarations */
typedef struct Point Point;
typedef struct Config Config;

/* Enum definitions */
typedef enum { Color_Red = 0, Color_Green = 1, Color_Blue = 2 } Color;

/* Struct definitions */
struct Point { int32_t x; int32_t y; };
struct Config { int32_t width; int32_t height; int32_t fullscreen; };

/* Function prototypes */
static int32_t Point__sum(Point* self);
static int32_t add(int32_t v0, int32_t v1);
int32_t main_entry(void);

/* Function definitions */
static int32_t Point__sum(Point* self) {
    int32_t v1 = self->x;
    int32_t v2 = self->y;
    int32_t v3 = v1 + v2;
    return v3;
}

// ...
```

Functions without `pub` are emitted as `static`. Functions with `pub` get external linkage.

## Implementation Phases

### Phase 1: Scaffold + Primitives

- [ ] Create `include/roxy/compiler/c_emitter.hpp` and `src/roxy/compiler/c_emitter.cpp`
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
- [ ] Add basic E2E test: compile Roxy → C → gcc → run → check output

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
- [ ] E2E tests for structs, enums, tagged unions

### Phase 3: Runtime Library

- [ ] Create `roxy_rt.h` and `roxy_rt.c` (or `.h`-only)
- [ ] Implement `roxy_alloc`/`roxy_free` (malloc-based with `ObjectHeader`)
- [ ] Implement `roxy_ref_inc`/`roxy_ref_dec`
- [ ] Implement `roxy_weak_create`/`roxy_weak_valid`
- [ ] Implement string operations (`roxy_string_from_literal`, `roxy_string_concat`, `roxy_string_eq`, etc.)
- [ ] Implement `to_string` conversions for primitives
- [ ] Implement list operations (`roxy_list_new`, `roxy_list_push`, `roxy_list_pop`, etc.)
- [ ] Implement print functions
- [ ] Handle `New`/`Delete` IR ops → `roxy_alloc`/`roxy_free` + constructor/destructor calls
- [ ] Handle `RefInc`/`RefDec`/`WeakCheck` IR ops
- [ ] Handle `GetIndex`/`SetIndex` → `roxy_list_get`/`roxy_list_set`
- [ ] Handle `ConstString` → `roxy_string_from_literal`
- [ ] Handle `CallNative` → dispatch to runtime functions
- [ ] E2E tests for strings, lists, heap allocation, ref counting

### Phase 4: Native Function Integration

- [ ] Generate C-compatible declarations for native functions registered via `NativeRegistry`
- [ ] Handle `CallNative` for user-registered natives (link against user-provided C/C++ code)
- [ ] Handle native struct methods
- [ ] Cross-module calls (`CallExternal`) → already resolved during linking, emit as regular calls
- [ ] E2E tests for native function calls

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
// In tests/e2e/c_backend_test.cpp
TEST_CASE("C Backend - Basics") {
    const char* source = R"(
        fun main(): i32 {
            var x: i32 = 10;
            var y: i32 = 20;
            return x + y;
        }
    )";

    // Compile Roxy → SSA IR → C source
    auto c_source = compile_to_c(source);

    // Write to temp file, compile with system C compiler, run, check output
    auto result = compile_and_run_c(c_source);
    CHECK(result.exit_code == 30);
}
```

Helper functions:
- `compile_to_c(source)` — runs Roxy frontend + CEmitter, returns C string
- `compile_and_run_c(c_source)` — writes `.c`, invokes `cc`, runs binary, returns exit code + stdout

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
| Startup time | Fast (no compilation) | Slow (requires C compiler) |
| Runtime performance | Slower (interpreter overhead) | Fast (native code) |
| Portability | Anywhere VM runs | Anywhere a C compiler exists |
| Debugging | VM debugger | gdb/lldb with `#line` directives |
| Binary size | Small (VM + bytecode) | Larger (full native binary) |
| Hot reload | Possible (reload bytecode) | Requires recompilation |

The two paths complement each other: interpreter for development, C backend for shipping.

## Files

| File | Purpose |
|------|---------|
| `include/roxy/compiler/c_emitter.hpp` | CEmitter class declaration |
| `src/roxy/compiler/c_emitter.cpp` | C emission implementation |
| `include/roxy/rt/roxy_rt.h` | C runtime library header |
| `src/roxy/rt/roxy_rt.c` | C runtime library implementation |
| `tests/e2e/c_backend_test.cpp` | E2E tests |

## Dependencies

The C backend requires all existing frontend components:
- Lexer, Parser, Semantic Analysis, IR Builder (already implemented)
- No changes needed to existing code — the `CEmitter` is a new consumer of `IRModule`