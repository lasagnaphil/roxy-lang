# C Backend (AOT Compilation)

> **Status:** Phases 1–4 fully implemented; Phase 5 partially implemented (function- and statement-level `#line` directives). Other Phase 5 items (DCE, Relooper, `switch` lowering, readable variable names) are deliberately not pursued — the C compiler's optimizer covers them and they don't affect debugger UX.
>
> **Language feature coverage:** every feature has a codegen path — primitives, structs (inheritance, methods, ctors/dtors, copy, nesting), enums, tagged unions, generics, traits/operators, strings, lists, maps, module globals, coroutines, exceptions, and **closures** (lambdas, captures, function references, self-capture). Roxy identifiers that are C++ keywords (e.g. a function named `double`, a field named `class`) are escaped with a reserved `roxy_kw_` prefix in `emit_mangled_name`, so they compile.
>
> **Correctness is *not* complete.** Running the full `tests/e2e/` suite through the C backend (see Testing below) surfaced real divergences from the VM that the prior hand-written tests never exercised. The core uniq/RAII destruction bugs (null-guarded delete, owned struct-value locals) are now fixed; remaining gaps include struct-by-value copy semantics, operator dispatch on structs, uniq move-state across control flow, weak fields, and closures. See **Known C-backend gaps** under Testing for the current list.

The C backend (`CEmitter`) translates Roxy's SSA IR into a `.cpp` file that any C++ compiler can build. The body is C-style (structs, gotos, typed `vN` locals); native bindings and the public header use C++ to interface directly with the embedder. It operates on the same `IRModule` the bytecode lowering uses, so all frontend work (type checking, method/operator resolution, monomorphization, struct layout) is already done.

## Pipeline

```
Source → ... → SSA IR
                 ├→ Lowering → Bytecode → VM          (interpreter)
                 └→ CEmitter → .cpp file → g++/clang++ (AOT)
```

SSA IR is the chosen translation source: every op is typed, structs are laid out, generics are monomorphized, and operators are desugared into method calls, so the mapping to C is mechanical. (AST would force re-implementing all of that; bytecode loses types and produces unreadable register-shuffling C.)

## Type Mapping

### Primitives

| Roxy | C | Header |
|------|---|--------|
| `void` | `void` | — |
| `bool` | `bool` | `<stdbool.h>` |
| `i8`–`i64` | `int8_t`–`int64_t` | `<stdint.h>` |
| `u8`–`u64` | `uint8_t`–`uint64_t` | `<stdint.h>` |
| `f32` / `f64` | `float` / `double` | — |
| `string` | `roxy_string*` | `roxy_rt.h` |

### Compound Types

| Roxy | C |
|------|---|
| `struct Point { x: i32; y: i32; }` | `typedef struct { int32_t x; int32_t y; } Point;` |
| `enum Color { Red, Green, Blue }` | `typedef enum { Color_Red, Color_Green, Color_Blue } Color;` |
| `List<T>` / `Map<K,V>` | `roxy_list*` / `roxy_map*` |
| `uniq T` | `T*` (owns the allocation) |
| `ref T` | `T*` (borrowing, ref-counted) |
| `weak T` | `roxy_weak` (`{void* ptr; uint64_t generation;}`) |

All reference types point to data preceded by a `roxy_object_header` (`{uint64_t weak_generation; uint32_t ref_count; uint32_t type_id;}`), matching the VM's layout. `weak_generation == 0` means dead/tombstoned.

## IR to C Mapping

### Constants

```
v0 = const_int 42          →  int32_t v0 = 42;
v3 = const_string "hello"  →  roxy_string* v3 = roxy_string_from_literal("hello", 5);
v4 = const_null            →  void* v4 = NULL;
v5 = const_int 1 (Color)   →  Color v5 = (Color)1;   // enum-typed constants need a cast
```

### Arithmetic, Comparisons, Conversions

Binary/unary ops map directly to C operators; conversions map to C-style casts:

```
v2 = add_i v0, v1    →  int64_t v2 = v0 + v1;
v2 = lt_f v0, v1     →  bool v2 = (v0 < v1);
v1 = not v0          →  bool v1 = !v0;
v1 = i_to_f64 v0     →  double v1 = (double)v0;
v1 = i_to_b v0       →  bool v1 = (v0 != 0);
```

### Structs

Roxy structs map directly to C structs — slot-based layout is field-order compatible. Types are forward-declared (`typedef struct Name Name;`) then defined in dependency order (Kahn topological sort over field types).

`StackAlloc` becomes a local declaration. Because the rest of the IR refers to structs through pointers, the emitter emits both the backing value and a pointer to it, and zero-initializes with `memset` (not `= {0}`, which fails in C++ when the first field is an enum):

```
v0 = stack_alloc 2          →  Point v0_struct; memset(&v0_struct, 0, sizeof(v0_struct));
                                Point* v0;          // v0 = &v0_struct; at block entry
v2 = set_field v0.x <- v1   →  v0->x = v1;
v3 = get_field v0.y         →  int32_t v3 = v0->y;
```

For a scalar `StackAlloc` (out/inout on a primitive) the value is dereferenced instead: `set_field v1.n <- v0` → `*v1 = v0;`.

`struct_copy v1, v0, 2` emits `*v1 = *v0;` when the concrete type is known (always, from `IRInst.type`), falling back to `memcpy(v1, v0, ...)` otherwise.

### Functions

IR functions become C functions with IR-typed parameters; no `roxy_ctx*` is threaded through (the context is thread-local — see below). Struct parameters are always pointers, since the IR uses pointer semantics for struct values.

```
fn add(v0, v1) -> i32 { v2 = add_i v0, v1; return v2 }
→
int32_t add(int32_t v0, int32_t v1) { int32_t v2 = v0 + v1; return v2; }
```

- **Inheritance:** passing a child pointer where a parent is expected emits an explicit cast — `Animal__new((Animal*)v0);`.
- **Out/inout:** params flagged in `IRFunction::param_is_ptr` become pointer params; `load_ptr`/`store_ptr` lower to `*v0` reads/writes.
- **Large struct returns (slot_count > 4):** the IR builder adds a hidden `__ret_ptr` last parameter and rewrites the return into a `struct_copy` + void return. The emitter detects `returns_large_struct()`, emits a `void` return type, and skips the call-site assignment — the caller passes its `StackAlloc` pointer as the output argument.

### Control Flow — Labels + Gotos

Each `IRBlock` becomes a labeled section; block arguments become local variables assigned before the `goto`. Gotos are trivially correct for any CFG shape, and C compilers reconstruct structured control flow internally, so there is no performance penalty.

```
loop(sum, i):
    v4 = le_i i, v2
    if v4 goto body else exit(sum)
body:
    v5 = add_i sum, i
    goto loop(v5, ...)
```
```cpp
loop:;
    int64_t sum = loop_arg0;
    int64_t i = loop_arg1;
    v4 = (i <= v2);
    if (v4) goto body; else { exit_arg0 = sum; goto exit; }
body:
    v5 = sum + i;
    loop_arg0 = v5; ...
    goto loop;
```

### Function Calls

Direct calls, no context parameter. `$$` in mangled names becomes `__` (`$` is not valid in C):

```
v2 = call "add" [v0, v1]               →  int32_t v2 = add(v0, v1);
v2 = call "Point$$sum" [v1]            →  int32_t v2 = Point__sum(v1);
v2 = call_external "math", "sin" [v0]  →  double v2 = math__sin(v0);   // cross-module
v2 = call_native "print" [v1]          →  roxy_print(v1);               // built-in
v2 = call_native "my_add" [v0, v1]     →  int32_t v2 = my_add(v0, v1);  // user native
```

### Object Lifecycle, Ref Counting, Pointers

```
v1 = new "Point" [args]   →  Point* v1 = (Point*)roxy_alloc(sizeof(Point), TYPEID_Point);
                             Point__new(v1, args);            // if a constructor exists
delete v0                 →  Point__delete(v0); roxy_free(v0);
ref_inc v0                →  roxy_ref_inc(v0);
weak_check v0             →  bool v1 = roxy_weak_valid(v0.ptr, v0.generation);
v1 = load_ptr v0          →  int32_t v1 = *v0;
nullify v0                →  memset(&v0_struct, 0, sizeof(v0_struct));   // or v0 = 0 for scalars
```

`delete` is **recursive typed delete** (`emit_typed_delete` / `emit_delete_slot`), the C analogue of the VM's descriptor-driven `delete_value`: a struct runs its `Type__delete` (which chains to its parent and walks owned fields); a noncopyable `List`/`Map` iterates its elements/keys/values, recurses into each (uniq pointers are loaded from the slot; inline value structs are cleaned in place), then frees the backing buffers via `roxy_list_delete` / `roxy_map_delete` before `roxy_free`. Nested containers (`List<List<uniq T>>`, `Map<_, uniq T>`) are handled.

Address-of (for out/inout) is handled by `StackAlloc` (`&v0_struct`) and `GetFieldAddr` — there is no dedicated `var_addr` op.

### Tagged Unions

A tagged union emits a struct with a discriminant field plus an anonymous `union` of anonymous per-variant structs (so variant fields are directly accessible on the parent). The `when` statement lowers to the same comparison chain as bytecode, mapping to `if`/`else if`:

```c
struct Skill {
    void* name;
    SkillType type;        // discriminant
    union {
        struct { int32_t damage; float crit_chance; }; /* Attack */
        struct { float damage_reduce; };               /* Defend */
    };
};
```

### Coroutines

Coroutines need no dedicated emitter support beyond type mapping. `coroutine_lower()`
runs *before* the C backend and rewrites every `Coro<T>` function into ordinary
functions — `init` (the original name, returns the coro), `__coro_<func>$$resume`,
`__coro_<func>$$done`, and `__coro_<func>$$delete` — built entirely from ops the
backend already handles (`New`, `GetField`, `SetField`, `EqI`, `Branch`, `Return`).
`IROp::Yield` never reaches the emitter (the IR validator rejects any survivor).

Two things make this work:

- **`Coro<T>` is a pointer to its state struct.** `TypeKind::Coroutine` emits as
  `__coro_<func>*` (`coro_info.generated_struct_type` + `*`), matching the
  `uniq __coro_<func>` the init function allocates and the `ref __coro_<func>`
  the resume/done/delete functions take. The synthesized state struct is appended
  to `IRModule::struct_types` *in the lowering pass* (it's created after
  `collect_backend_types` runs), so it gets a typedef, a dependency-sorted
  definition, and a `TYPEID_` define like any other struct.
- **Deleting a `Coro<T>` runs `__coro_<func>$$delete`.** `emit_typed_delete`
  treats the coroutine's state struct as the destructor pointee, so promoted
  `uniq`/noncopyable fields are cleaned up before `roxy_free` (the C analogue of
  the VM's descriptor-driven coro cleanup).

The lowering promotes locals that survive a yield into state-struct fields and
stores into them with raw `SetField`, where the regular path would use
`StructCopy` / `Nullify`. C++'s strict typing exposes two cases the emitter
handles in `SetField`: a struct-value field assigned a struct rvalue (a pointer
in the C backend) is dereferenced, and a `uniq`/`ref` pointer field assigned a
null (`void*`) is cast to the field type. The generated `$$delete` destructor
also returns a void-typed `ConstInt` sentinel, so a void-returning function emits
`return;` and void constants emit nothing.

### Exceptions

The VM resolves exceptions with a runtime handler table + an unwinding loop that
runs PC-range-keyed cleanup. The C backend has labels + gotos, not PC ranges, so
it uses a **checked-return** model instead (no setjmp/longjmp, no C++ EH):

- **Pending state.** A thread-local in-flight exception lives in `roxy_rt`
  (`roxy_set_pending` / `roxy_exception_pending` / `roxy_exception_type_id` /
  `roxy_exception_take`). `throw` stores the heap exception object and jumps to a
  routing label; after every `Call` / `CallExternal` the emitter writes
  `if (roxy_exception_pending()) goto …`. `ExceptionRef` (catch-all) emits as
  `void*`; `roxy_exception_message` is a stub (matches the VM, untested).
- **Routing.** Each `IRExceptionHandler` set sharing a try entry becomes one
  `__dispatch_<id>` label. A throw / pending-after-call in block *B* routes to the
  innermost try whose `try_body_blocks` contains *B*, else to `__unwind`. The
  dispatch runs the try-body cleanup, then a `roxy_exception_type_id()` if/else
  chain: a match assigns the caught object to the catch block's exception param
  (`(T*)roxy_exception_take()`) and `goto`s it; the catch-all / `finally.catch`
  matches unconditionally (last); no match falls to the next-outer dispatch or
  `__unwind`. `finally` needs no special logic — the IR already duplicates it into
  the normal and catch exits, and the `finally.catch` block runs it then
  re-throws. Unhandled exceptions out of `main_entry` print and exit nonzero.
- **Cleanup.** Per-frame cleanup reuses `emit_typed_delete`, null-guarded
  (`if (v) { … v = 0; }`) and LIFO. A dispatch cleans owned locals created inside
  its try body; `__unwind` cleans the whole frame. Correctness rests on every
  cleanup-tracked owned-local pointer being **zero-initialized** at declaration,
  **nulled after** a normal scope-exit `Delete`, and nulled on move — so the guard
  skips not-yet-created, already-freed, and moved values (matching the VM's
  register-nulling + scope-narrowing at block granularity). Cross-frame unwinding
  falls out naturally: a callee's `__unwind` cleans its frame and returns, then
  the caller's post-call check propagates.
- **Move-into-call ordering.** A `nullify V` consumed by a later call is emitted
  *after* that call reads `V` (the VM treats `nullify` as a scope marker, not a
  runtime zero) but *before* the post-call exception check, so a moved argument is
  owned by the callee and skipped by the caller's unwinding cleanup.

User-`native` functions are assumed not to throw (no post-`CallNative` check).

### Closures

The frontend lifts each lambda to a top-level `__lambda_<id>_call(__env: ref env, …)`
function and an env struct (`[__call_idx: u32][captures…]`); a function value is a
type-erased `uniq` pointer to that env (`TypeKind::Function` → `void*`). Env structs
are appended to `IRModule::struct_types` (in the IR builder's post-build loop) so
they get typedefs + `TYPEID_`s.

The VM dispatches `CALL_INDIRECT` through a function-table index in `__call_idx`;
AOT has no such table, so the emitter builds its own per-module
`g_closure_fns[]` (a `roxy_closure_fn` array) indexed the same way:

- **`Closure`** allocates the concrete env, stores the call function's index in
  `__call_idx`, then the captures by field name (struct captures dereference; a
  `[weak self]` capture wraps the pointer in `roxy_weak_create`).
- **`CallIndirect`** reads `*(uint32_t*)env`, indexes `g_closure_fns`, casts to
  `Ret(*)(void*, params…)` from the callee's `Function` signature, and prepends the
  env. A `ref fun` callee is the same pointer; a `weak fun` callee uses `.ptr`.
- **`AssertHeap`** (ref/weak `self` capture on a possibly-stack receiver) →
  `if (!roxy_heap_owns(p)) abort();`, reproducing the VM's `owns` trap.
- **Delete** of a `Function` is type-erased, so `emit_typed_delete` routes to a
  generated `__closure_delete(env)` that dispatches the env destructor
  (`__lambda_<id>_env$$delete`, synthesized for envs with noncopyable/ref captures)
  by `__call_idx`, then frees. `List<fun>` / `Map<_, fun>` element cleanup loads the
  env pointer from the slot first (closures are pointer-shaped elements).

Two supporting fixes the closure work required: a method returning a closure has an
unset IR `return_type`, so the prototype/return use an **effective return type**
derived from the actual `Return` value; and field access on a `weak`-typed object
derefs `((Inner*)w.ptr)->field`. Move-`Nullify` of a `[move]`-captured value is
deferred until after the `Closure` op reads it (the same peephole used for
move-into-call).

## Thread-Local Runtime Context

The VM's native ABI (`void(RoxyVM*, u8 dst, u8 argc, u8 first_arg)` reading/writing registers) has no analogue in AOT — there is no register file or `RoxyVM*`. Instead the AOT path carries runtime state in a small `roxy_ctx` (`allocator` / `exception_state` / `user_data`) accessed through thread-local storage, so it never appears in a function signature:

```c
void roxy_set_ctx(roxy_ctx* ctx);
roxy_ctx* roxy_get_ctx(void);
```

`roxy_ctx` is the first member of `RoxyVM`, so the same native functions work in both modes. In **VM mode** the interpreter brackets every public entry with `roxy::ScopedContext(&vm->ctx)` (an RAII guard in `roxy_rt.h` that saves/restores the previous ctx). In **AOT mode** the generated `main()` (or the embedder, via `ScopedContext`) sets it. All generated code, runtime functions, and natives call `roxy_get_ctx()` when they need allocation or exception state.

Because the context is thread-local, native functions are plain C++ taking only their logical parameters — `i32 my_add(i32 a, i32 b)`, `i32 point_sum(Point* self)` — and reach engine state via `(GameEngine*)roxy_get_ctx()->user_data` when needed.

### Native Binding Categories

| Binding style | VM mode | AOT mode |
|---|---|---|
| `bind<FnPtr>("name")` | `FunctionBinder` wraps FnPtr, VM sets TLS ctx | direct call to FnPtr |
| `bind_method<FnPtr>(...)` | binder wraps | direct call with `self*` |
| `bind_native(fn, sig)` | VM-style manual wrapper | needs `bind_native(vm_fn, sig, aot_symbol)` overload |
| Built-in (print, list, …) | VM native functions | `roxy_rt` C functions (use TLS ctx internally) |

`bind<FnPtr>` is the cleanest path: the template stores both the VM wrapper and the original function pointer, which the emitter references directly (no wrapper overhead). For the signature-based API, the dual-mode `bind_native(vm_fn, sig, aot_symbol)` overload supplies an AOT-compatible function; the VM-only `bind_native(fn, sig)` remains, but a `CallNative` referencing it from the emitter is a compile error. Built-in natives map by name to `roxy_rt` equivalents (`print` → `roxy_print`, `List$$push` → `roxy_list_push`, etc.).

When `emit_native_call`'s static-table lookup misses, the emitter consults `CEmitterConfig::native_registry` and emits a typed direct call using the entry's `aot_symbol_name` (defaults to the registered Roxy name; `bind<>(roxy_name, aot_symbol)` lets them diverge). The emitter pre-scans IR for user-native `CallNative` ops and writes `extern Ret name(Args...);` declarations into the source preamble, so AOT binaries link against either inline-defined `native_include_paths` headers or separately-compiled `.cpp` translation units.

### Generic Native Types in AOT

`List<T>` / `Map<K,V>` are type-erased — every Roxy value is 64 bits, so one C implementation serves all element types. Call boundaries cast through `uint64_t` (a no-op on 64-bit platforms for integers):

```cpp
roxy_list* v0 = roxy_list_new(16);
roxy_list_push(v0, (uint64_t)42);
int32_t v1 = (int32_t)roxy_list_get(v0, 0);
```

## Runtime Library (`roxy_rt.h`)

The runtime provides C implementations of everything needing allocation or complex logic — allocation, ref-counting, weak refs, strings, lists, maps (incl. struct keys with custom hash/eq), `to_string` conversions, and `print`. Allocating functions read `roxy_get_ctx()` internally, so the public API is context-free. See `rt/roxy_rt.h` for the full function list.

### Memory Management

Allocation flows through `roxy_ctx.allocator`, a `roxy_allocator` vtable (`alloc` / `free` / `owns` / `userdata`). Two implementations live in `roxy_rt`:

1. **Slab allocator** (`rt/slab_allocator.{hpp,cpp}`, moved out of `vm/` during runtime unification) — used by both VM and AOT. Freed slots stay mapped (zeroed) so stale weak refs reliably read "dead", and recycled slots get fresh random generations. AOT brings up a process-wide slab via `roxy_rt_init` / `roxy_rt_shutdown`; VM allocates a per-VM `SlabAllocator` and installs it via `make_slab_allocator_vtable(...)`.
2. **Malloc allocator** (`roxy_malloc_allocator`) — defensive fallback when `roxy_get_ctx()` or `ctx->allocator` is null (e.g. static initializers before `roxy_rt_init`). Generations come from a thread-local xorshift64; weak-ref soundness is best-effort since libc may reuse addresses.

`roxy_alloc(data_size, type_id)` writes the `roxy_object_header` with the allocator's generation and returns the data pointer. `roxy_free(data)` lets the active allocator tombstone `weak_generation` and reclaim the memory.

### C++ RAII Templates and Wrappers

`roxy_rt.h` provides C++ templates that the generated header's factories hand to the embedder for idiomatic ownership (all use `roxy_get_ctx()` internally, no stored ctx pointer):

- **`roxy::uniq<T>`** — maps to `uniq T`; move-only, calls destructor + `roxy_free` on scope exit.
- **`roxy::ref<T>`** — maps to `ref T`; ref-counted, copyable; last copy frees.
- **`roxy::weak<T>`** — maps to `weak T`; non-owning, nullable; stores pointer + generation, `valid()`/`lock()` check liveness.

It also provides thin typed facades over the type-erased C container functions — **`roxy::String`**, **`roxy::List<T>`**, **`roxy::Map<K,V>`**. The VM bindings `rx::RoxyString` / `rx::RoxyList<T>` / `rx::RoxyMap<K,V>` are now `using` aliases of these, so VM and AOT share one wrapper implementation; the `RoxyType<T>` specializations stay in the VM binding layer (they depend on `TypeCache`). See `rt/roxy_rt.h`.

## Emitter Architecture

`CEmitter(BumpAllocator&, const CEmitterConfig&)` exposes `emit_source()` and `emit_header()`. `CEmitterConfig` carries `native_include_paths`, `native_registry`, and an `emit_main_entry` toggle. Per-function state tracks `m_value_types`, the set of `StackAlloc` result values, and `m_pointer_values` (StackAlloc, struct/ref/uniq/out/inout params, and `GetFieldAddr` results) — the last decides whether `emit_field_access` uses `->` or `.`. See `compiler/c_emitter.hpp`.

`emit_source()` produces one `.cpp` with: standard includes, native include paths, enum typedefs, struct forward declarations, dependency-sorted struct definitions, function forward declarations, then function bodies. `emit_header()` produces a `.hpp` with `pub` enums, `pub` structs (with inline method wrappers), `make_<T>` / `make_<T>__<ctor>` factories returning `roxy::uniq<T>`, and `pub` function declarations.

### Generated Output

The `.hpp` is what the embedder `#include`s. `pub` structs get inline method wrappers (zero overhead — same codegen as the free-function call) and heap-allocatable structs get RAII factories:

```cpp
// scripts.hpp
#pragma once
#include "roxy_rt.h"

int32_t Point__sum(Point* self);            // mangled forward decl
void Player__new(Player* self, roxy_string* name, int32_t health);
void Player__delete(Player* self);

struct Point {
    int32_t x; int32_t y;
    int32_t sum() { return Point__sum(this); }   // inline wrapper
};

inline roxy::uniq<Player> make_Player(roxy_string* name, int32_t health) {
    Player* ptr = (Player*)roxy_alloc(sizeof(Player), TYPEID_Player);
    Player__new(ptr, name, health);
    return roxy::uniq<Player>(ptr, Player__delete);
}

int32_t main_entry();
```

Only `pub` types/functions appear in the header; everything else is `static` in the `.cpp`. The `.cpp` includes its own `.hpp`, then the embedder's native headers, then `extern` declarations for user natives, then `static` prototypes and all bodies. In standalone mode it ends with a `main()` that does `roxy_ctx_init` → `roxy_set_ctx` → `main_entry()` → `roxy_ctx_destroy`. The embedder uses the header naturally — `Point p = {3,4}; p.sum();`, `auto pl = make_Player(...); pl->take_damage(25);` — bracketing calls with `roxy::ScopedContext`.

## Build Integration

A CMake `add_custom_command` runs the compiler (`--backend=c --output-dir=... --native-includes=...`) to emit `scripts.{hpp,cpp}` before the main build; the generated `.cpp` is compiled and linked alongside engine code against `roxy_rt`. `--native-includes` maps to `CEmitterConfig::native_include_paths`, telling the emitter which embedder headers to `#include`.

## Phase 5: `#line` Directives

The emitter attributes generated body lines back to Roxy source at both function and statement granularity. `IRFunction::source_line` (from each AST decl's `body->loc.line`) seeds a `#line N "<source_path>"` at function entry; `IRInst::source_line` (set by `IRBuilder::emit_inst` from `m_current_source_line`, updated at each `gen_stmt`/`gen_decl` boundary) re-emits the directive at every statement-line transition, deduplicated so consecutive insts on one source line don't repeat it. This gives gdb/lldb users Roxy-source line mapping.

## Comparison with Bytecode Path

| Aspect | Bytecode + VM | C Backend |
|--------|---------------|-----------|
| Startup | Fast (no compilation) | Slow (needs C++ compiler) |
| Runtime perf | Slower (interpreter) | Fast (native) |
| Portability | Anywhere the VM runs | Anywhere a C++ compiler exists |
| Debugging | VM debugger | gdb/lldb with `#line` |
| Hot reload | Reload bytecode | Requires recompilation |

The two paths complement each other: interpreter for development, C backend for shipping.

## Testing

`compile_and_run_cpp(source)` runs the full pipeline (Roxy → IR → CEmitter → temp `.cpp` → `c++ -std=c++17` → run → check exit code + stdout). `compile_to_cpp` returns the C++ string; `compile_and_run_cpp_with_registry` links an inline native header for AOT NativeRegistry tests; pass `debug=true` to dump IR and generated source. Because these invoke the system compiler, the suite must run outside the sandbox.

### Parametric E2E coverage (`<VM>` / `<C>`)

Most `tests/e2e/` suites are **backend-parametric**: a single test body runs on
both the bytecode VM and the C backend via doctest's `TEST_CASE_TEMPLATE`, driven
by `tests/e2e/test_e2e_backend.hpp`. The backend is a type parameter
(`VMBackend` / `CBackend`), each exposing `static E2EResult run(source)` over a
unified `{success, value, stdout_output}` result (`value` is the VM return value
or the C process exit code). doctest registers each instantiation as a separate
case named `<TestName><VM>` / `<TestName><C>`, so a failure is attributed to the
backend, and the type tag drives selection:

```bash
./roxy_tests --test-case="*<VM>*"          # VM only (sandbox-safe, fast)
./roxy_tests --test-case="*<C>*"           # C only  (needs the system compiler)
./roxy_tests --test-case-exclude="*<C>*" \
             --test-suite-exclude="E2E C Backend"   # everything compiler-free (in-sandbox)
```

Cases the C backend cannot run are demoted to plain `TEST_CASE` (VM-only) with a
`// VM-only: <reason>` annotation, in three categories: results outside the
0..255 exit-code range, runtime-trap/abort tests whose behavior differs on C, and
the known C-backend gaps below. `test_c_backend.cpp` retains only C-specific
tests (generated-header emission, AOT NativeRegistry dispatch, `#line`
directives) — feature coverage proper lives in the shared parametric suites.

### Known C-backend gaps (surfaced by the parametric suite)

Converting the e2e suites to run on C exposed pre-existing gaps that the prior
hand-written `test_c_backend.cpp` never exercised. These cases are VM-only
pending fixes:

| Area | Symptom |
|------|---------|
| **uniq move-state across control flow** | a `uniq`/struct-literal moved in a terminating branch (then/else/when-case that returns), and move-out of fields / user-defined index results |
| **ref/inout uniq ownership** | `ref`/`inout` to a `uniq` field, `inout uniq` reassignment, and `ref`-local count balancing across control flow |
| **weak fields** | reading/writing a `weak` struct field |
| **struct-by-value copy semantics** | struct params/returns are passed by pointer with no copy-on-call, so a callee mutating a by-value struct param aliases the caller's value |
| **nested tagged-union value assignment / recursive cleanup** | assigning a tagged-union value into a variant field, and recursive destruction of tagged-union trees |
| **coroutine uniq-field cleanup** | `Coro<T>` promoting `uniq`/`List<uniq>`/`Map<_,uniq>` state |
| **trait/operator dispatch on structs** | arithmetic/bitwise/unary operator overloads and generic default-method injection |
| **closures** | `self` capture and function-to-`ref fun` borrow conversion |
| **weak field read**, **try-local rebinding**, **struct-valued map persistence** | smaller divergences |
| **string runtime-library natives** | `str_char_at` / `str_substr` / `str_to_f64` / `str_from_code` / `clock` / `read_file` are not implemented in `roxy_rt` |
| **rvalue `Printable.to_string`** | a `to_string()` returning an f-string from a struct rvalue emits a `void*` return type |

Not bugs, also VM-only: tests asserting a result > 255 (8-bit exit code — many
are recoverable by asserting printed stdout instead) and runtime-trap/overflow
tests where the C binary aborts rather than trapping cleanly.

**Fixed.** Three emitter bugs surfaced by the parametric suite are resolved:
- f32 whole-number literal (`0.0f32` → ill-formed `0f`; now `0.0f`) — `IROp::ConstF`.
- `Delete` of a null heap pointer crashed (the VM treats delete-on-null as a
  no-op); the struct-delete path in `emit_typed_delete` is now null-guarded, so
  reassigning a not-yet-set `uniq` field no longer segfaults.
- An owned struct **value** local (e.g. bound to a by-value call result) was
  declared/deleted/nulled as a pointer (`Owner v0 = 0;`, `Owner__delete((Owner*)v0)`);
  it is now `memset`-initialized and deleted through its address.

Together these recovered the bulk of the `uniq`/RAII destruction cluster on the
C backend.

## Files

| File | Purpose |
|------|---------|
| `include/roxy/compiler/c_emitter.hpp` | `CEmitter` + `CEmitterConfig` declarations |
| `src/roxy/compiler/c_emitter.cpp` | C/header emission (`emit_source`, `emit_header`, native-call/extern-decl logic) |
| `include/roxy/compiler/ssa_ir.hpp` | `IRModule::struct_types` / `enum_types`, `IRFunction/IRInst::source_line` |
| `src/roxy/compiler/ir_builder.cpp` | Populates `struct_types` / `enum_types`; per-inst `source_line` |
| `include/roxy/rt/roxy_rt.h` | C runtime header + C++ RAII templates / container wrappers |
| `src/roxy/rt/roxy_rt.cpp` | C runtime implementation |
| `include/roxy/rt/slab_allocator.{hpp}`, `src/roxy/rt/slab_allocator.cpp` | Slab allocator + `make_slab_allocator_vtable` (moved from `vm/`) |
| `include/roxy/rt/vmem.hpp`, `src/roxy/rt/vmem_{unix,win32}.cpp` | Virtual memory ops |
| `include/roxy/rt/string_intern.{hpp}`, `src/roxy/rt/string_intern.cpp` | Intern table + C-callable lookup/insert |
| `include/roxy/vm/map_dispatch.{hpp}`, `src/roxy/vm/map_dispatch.cpp` | VM Hash/Eq trampolines + per-VM dispatch side-table |
| `include/roxy/vm/binding/binder.hpp`, `registry.hpp` | `bind<>` / `bind_native` overloads, `aot_symbol_name` metadata |
| `tests/e2e/test_c_backend.cpp` | E2E tests |
| `tests/unit/test_runtime_ctx.cpp` | `roxy_ctx` / `ScopedContext` unit tests |
| `tests/e2e/test_helpers.hpp` | `compile_to_cpp` / `compile_and_run_cpp[_with_registry]` / `header_compiles` helpers |
