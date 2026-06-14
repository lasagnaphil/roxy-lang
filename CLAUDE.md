# Claude Code Project Guide - Roxy

## Project Overview

Roxy is an embeddable scripting language for game engines with:
- Static typing
- Value semantics by default
- Memory management via `uniq`/`ref`/`weak` references (no GC)
- Fast C++ interop
- Future AOT compilation to C

## Example Code

```roxy
// ── Enums ──
enum Element { Fire, Ice, Lightning }

// ── Structs with fields and default values ──
struct Vec2 {
    x: f32 = 0.0f;
    y: f32 = 0.0f;
}

// ── Methods ──
fun Vec2.length_sq(): f32 {
    return self.x * self.x + self.y * self.y;
}

// ── Traits and operator overloading ──
fun Vec2.add(other: Vec2): Vec2 for Add {
    return Vec2 { x = self.x + other.x, y = self.y + other.y };
}

fun Vec2.mul(scalar: f32): Vec2 for Mul<f32> {
    return Vec2 { x = self.x * scalar, y = self.y * scalar };
}

fun Vec2.eq(other: Vec2): bool for Eq {
    return self.x == other.x && self.y == other.y;
}

// ── Generics ──
struct Pair<T, U> {
    first: T;
    second: U;
}

fun identity<T>(value: T): T {
    return value;
}

// ── Inheritance ──
struct Entity {
    pos: Vec2;
    hp: i32;
}

fun Entity.is_alive(): bool {
    return self.hp > 0;
}

struct Player : Entity {
    name: string;
    mana: i32;
}

// ── Named constructors and destructors ──
fun new Player(name: string, x: f32, y: f32) {
    self.pos = Vec2 { x = x, y = y };
    self.hp = 100;
    self.name = name;
    self.mana = 50;
}

fun delete Player() {
    print(f"Player {self.name} removed");
}

// ── Tagged unions ──
struct Skill {
    name: string;
    when element: Element {
        case Fire:
            burn_duration: i32;
        case Ice:
            slow_factor: f32;
        case Lightning:
            chain_count: i32;
    }
}

// ── When statement (pattern matching) ──
fun describe_skill(s: Skill): string {
    when s.element {
        case Fire:
            return f"Fire skill: burns for {s.burn_duration} turns";
        case Ice:
            return f"Ice skill: slows by {s.slow_factor}";
        case Lightning:
            return f"Lightning skill: chains to {s.chain_count} targets";
    }
}

// ── Exception handling ──
struct OutOfMana {
    required: i32;
    available: i32;
}

fun OutOfMana.message(): string for Exception {
    return f"Need {self.required} mana, have {self.available}";
}

fun cast_spell(player: ref Player, cost: i32) {
    if (player.mana < cost) {
        throw OutOfMana { required = cost, available = player.mana };
    }
    player.mana = player.mana - cost;
}

// ── Coroutines ──
fun countdown(n: i32): Coro<i32> {
    var i: i32 = n;
    while (i > 0) {
        yield i;
        i = i - 1;
    }
}

// ── Lists, Maps, control flow, references ──
fun main() {
    // Unique ownership and RAII
    var player: uniq Player = uniq Player("Arwen", 10.0f, 20.0f);

    // Inherited method
    print(f"Alive: {player.is_alive()}");

    // Operator overloading
    var a: Vec2 = Vec2 { x = 1.0f, y = 2.0f };
    var b: Vec2 = Vec2 { x = 3.0f, y = 4.0f };
    var c: Vec2 = (a + b) * 2.0f;

    // Generic inference
    var pair = Pair { first = 42, second = "hello" };
    var x: i32 = identity(10);

    // Lists
    var scores: List<i32> = List<i32>();
    for (var i: i32 = 0; i < 5; i = i + 1) {
        scores.push(i * 10);
    }
    print(f"Scores: len={scores.len()}, first={scores[0]}");

    // Maps
    var inventory: Map<string, i32> = Map<string, i32>();
    inventory.insert("potion", 3);
    inventory.insert("elixir", 1);
    inventory["potion"] = inventory["potion"] + 1;
    print(f"Potions: {inventory["potion"]}");

    // Tagged union + when
    var skill: Skill = Skill {
        name = "Fireball",
        element = Element::Fire,
        burn_duration = 3
    };
    print(describe_skill(skill));

    // Exception handling
    try {
        cast_spell(player, 999);
    } catch (e: OutOfMana) {
        print(f"Failed: {e.message()}");
    } finally {
        print("Spell attempt complete");
    }

    // Coroutine
    var coro = countdown(3);
    while (!coro.done()) {
        print(f"Countdown: {coro.resume()}");
    }

    // Type casting and numeric literals
    var big: i64 = 1000000l;
    var small: i32 = i32(big);
    var flag: bool = bool(small);

    // Out parameters
    var ox: i32 = 0;
    var oy: i32 = 0;
    init_pair(out ox, out oy);

    // player is automatically deleted here (RAII)
}

fun init_pair(x: out i32, y: out i32) {
    x = 10;
    y = 20;
}
```

## Build System

- **Build tool:** CMake with Ninja
- **Compiler:** clang-cl (Windows), clang/gcc (macOS/Linux)
- **C++ Standard:** C++17

### Build Commands

**Windows (clang-cl):**
```bash
cd build
cmake .. -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_MT="C:/Program Files/LLVM/bin/llvm-mt.exe"
ninja
```

**macOS/Linux:**
```bash
cd build
cmake .. -G Ninja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
ninja
```

**With AddressSanitizer:**
```bash
cmake .. -G Ninja -DENABLE_ASAN=ON
```

> **Note:** ASAN is currently **disabled** on the maintainer's machine — the ASAN
> runtime deadlocks at process startup (inside `AsanInitInternal`, before `main`)
> on macOS Tahoe. Use `-DENABLE_ASAN=OFF` (the default) until the toolchain/OS
> issue is resolved. A binary that spins at ~100% CPU before running any test is
> this deadlock, not a code bug.

### CMake Libraries

The project is organized into 6 libraries:
- `roxy_core` - File utilities, rx::String, rx::format_to, JSON parser/writer
- `roxy_shared` - Lexer and tokens
- `roxy_compiler` - Parser, AST, semantic analysis, SSA IR, IR builder
- `roxy_rt` - Unified runtime (allocation, slab allocator, vmem, strings, lists, maps, intern). Used by both `roxy_vm` and AOT-compiled programs.
- `roxy_vm` - Bytecode, value, object, VM, interpreter, lowering
- `roxy_lsp` - Error-recovering parser, LSP transport, LSP server

## Code Conventions

- **Namespace:** `rx::`
- **Naming:**
  - Functions/methods: `snake_case`
  - Types/classes: `PascalCase`
  - Member variables: `m_` prefix (e.g., `m_source`, `m_current`)
  - Local variables: Use descriptive names, avoid excessive abbreviations (e.g., `method_info` not `mi`, `struct_type` not `st`)
- **Types:** Use aliases from `types.hpp`: `u8`, `u16`, `u32`, `u64`, `i8`, `i16`, `i32`, `i64`, `f32`, `f64`
- **Headers:** Use `#pragma once`
- **Assertions:** Use `assert()` for invariants

## Project Structure

```
roxy-v2/
├── include/roxy/
│   ├── core/           # Core utilities (types.hpp, span.hpp, vector.hpp, allocators)
│   ├── shared/         # Lexer and tokens
│   ├── compiler/       # Parser, AST, types, semantic, SSA IR, IR builder, lowering
│   ├── lsp/            # LSP server (syntax_tree, lsp_parser, protocol, transport, server)
│   ├── rt/             # Unified runtime (roxy_rt.h, slab_allocator, vmem, string_intern) — used by both VM and AOT-compiled programs
│   └── vm/             # Bytecode, value, object, VM, interpreter, binding/, map_dispatch
├── src/roxy/           # Implementation files matching include/ structure
├── tests/
│   ├── test_main.cpp   # Single doctest entry point
│   ├── unit/           # Unit tests (lexer, parser, semantic, IR, bytecode, VM)
│   └── e2e/            # End-to-end tests (basics, structs, lists, strings, modules, etc.)
├── docs/
│   ├── overview.md     # Language features and design
│   ├── grammar.md      # Grammar specification, numeric literals, type casting
│   ├── libraries.md    # Vendored library documentation
│   └── internals/      # Detailed implementation documentation
└── CMakeLists.txt
```

## Compiler Pipeline

```
Source → Lexer → Parser → AST → Semantic Analysis → IR Builder → SSA IR → Lowering → Bytecode → VM
```

## Key Language Features

### Reference Types

| Type | Owns? | Nullable? | On dangling | Move semantics |
|------|-------|-----------|-------------|----------------|
| `uniq` | Yes | Yes | N/A (is owner) | Passed to `uniq` param = move (caller consumed) |
| `ref` | No | No | Assert/crash | Borrows from owner |
| `weak` | No | Yes | Returns null or asserts | N/A |

`uniq` variables are implicitly deleted at scope exit (RAII). Passing `uniq` to a function moves ownership; using the variable after move is a compile error.

### Keywords

- Types/modifiers: `true false nil var fun struct enum pub native`
- Control flow: `if else for while break continue return when case try catch throw finally yield`
- OOP: `self super new delete`
- References: `uniq ref weak out inout`
- Imports: `import from`

### Numeric Types and Casting

See `docs/grammar.md` for numeric literal suffixes and type casting rules.

## Implemented Components

### Frontend
**Lexer** - Tokenizes source code with number bases, suffixes, escape sequences, nested comments.
**Details:** `docs/internals/frontend.md` | **Files:** `shared/lexer.hpp`, `shared/lexer.cpp`

**Parser** - Recursive descent with Pratt parsing for expressions. Fail-fast design.
**Details:** `docs/internals/frontend.md` | **Files:** `compiler/parser.hpp`, `compiler/parser.cpp`

**AST** - Expression, statement, and declaration node kinds (literals, operators, calls, control flow, structs, enums, traits, etc.).
**Files:** `compiler/ast.hpp`

**Semantic Analysis** - Multi-pass analyzer with symbol resolution, type inference, type checking, and move-state tracking for `uniq` variables (use-after-move detection).
**Details:** `docs/internals/frontend.md` | **Files:** `compiler/semantic.hpp`, `compiler/semantic.cpp`

### Type System
**Types** - Primitives (`void`, `bool`, `i32`, `i64`, `f32`, `f64`, `string`), structs, enums, references.
**Files:** `compiler/types.hpp`, `compiler/types.cpp`

**Enums** - C-style enumerations with integer underlying type. Access via `Type::Variant`.
**Tests:** `tests/e2e/test_enums.cpp`

**Structs** - Stack-allocated value types with slot-based layout, inheritance, methods, constructors/destructors.
**Details:** `docs/internals/structs.md`, `docs/internals/methods.md`, `docs/internals/inheritance.md`, `docs/internals/constructors.md`

**Tagged Unions** - Discriminated unions with `when` clause in struct definitions.
**Details:** `docs/internals/tagged-unions.md` | **Tests:** `tests/e2e/test_tagged_unions.cpp`

**Recursive Types** - Self-referential structs via `uniq` indirection (linked lists, trees, tagged-union ASTs) and mutually recursive structs. Direct value-type cycles (`struct Node { next: Node; }`) are rejected at compile time with an "infinite size" error. Recursive destruction is descriptor-driven — a `BCDeleteDesc` walks owned fields directly in C++ rather than re-entering the interpreter per node — so deep ownership chains destroy without overflowing the native stack.
**Details:** `docs/internals/recursive-types.md` | **Tests:** `tests/e2e/test_recursive_types.cpp`

### IR and Bytecode
**SSA IR** - Block arguments (not phi nodes); operations spanning arithmetic, comparisons, memory, calls, control flow, object lifecycle, and closures.
**Details:** `docs/internals/ssa-ir.md` | **Files:** `compiler/ssa_ir.hpp`, `compiler/ir_builder.hpp`

**IR Optimizations** - Phase 1 (constant folding, algebraic simplifications, cast folding) eagerly applied during IR building. Phases 2 (DCE, copy propagation), 3 (branch folding, block merging, trivial block-argument elimination), and 4 (block-local Common Subexpression Elimination) as standalone passes between coroutine lowering and IR validation, iterated to a fixed point with a final RPO sweep.
**Details:** `docs/internals/optimization.md` | **Files:** `compiler/ir_optimize.hpp`, `compiler/ir_optimize.cpp`

**Bytecode** - 32-bit fixed-width register-based, three instruction formats (ABC, ABI, AOFF). Liveness-based register allocation with free-list reuse; register spilling via furthest-first eviction when pressure exceeds 255 registers.
**Details:** `docs/internals/bytecode.md`, `docs/internals/ssa-ir.md` | **Files:** `vm/bytecode.hpp`, `compiler/lowering.hpp`

### Runtime
**VM** - Shared register file with windowing, call frame stack, module loading.
**Details:** `docs/internals/vm.md` | **Files:** `vm/vm.hpp`, `vm/interpreter.hpp`

**Lists** - Dynamic lists (`List<T>`) with bounds checking, push/pop/len/cap methods. Noncopyable when `T` is noncopyable (move semantics, element cleanup at scope exit).
**Details:** `docs/internals/list.md` | **Files:** `vm/list.hpp`

**Maps** - Hash tables (`Map<K, V>`) with Robin Hood open addressing, backward-shift deletion, insert/get/remove/contains/clear/keys/values methods, index operator support. Builtin `Hash` trait for primitives. Noncopyable when `K` or `V` is noncopyable.
**Details:** `docs/internals/maps.md` | **Files:** `vm/map.hpp`, `vm/map.cpp`

**Strings** - Heap-allocated string objects. Operations via native functions (`str_concat`, `str_eq`, `str_len`). F-string interpolation (`f"hello {expr}"`) with automatic `to_string` conversion via builtin `Printable` trait.
**Details:** `docs/internals/strings.md` | **Files:** `vm/string.hpp`

**Slab Allocator** - Custom allocator with Vale-style random generational references, tombstoning.
**Details:** `docs/internals/memory.md` | **Files:** `vm/slab_allocator.hpp`, `vm/vmem.hpp`

### Interop and Modules
**C++ Interop** - Type-safe function binding with automatic wrapper generation via `NativeRegistry`.
**Details:** `docs/internals/interop.md` | **Files:** `vm/binding/`

**Module System** - Multi-file compilation with `import`/`from` syntax, topological sorting, static linking.
**Details:** `docs/internals/modules.md` | **Files:** `compiler/module_registry.hpp`, `compiler/compiler.hpp`

**Module Globals** - Top-level `var` declarations with persistent storage, a synthesized `__module_init` running initializers/constructors before `main`, and `__module_shutdown` running destructors for noncopyable globals at teardown (RAII). VM accesses via the `GLOBAL_ADDR` opcode; the C backend emits real C globals (`g_<name>`) with init/teardown driven from the generated `main()`. Both backends supported (single-module; multi-module init is a documented limitation). The C-backend `Delete` op gained typed-delete (runs destructors) as part of this.
**Details:** `docs/internals/globals.md` | **Files:** `compiler/ir_builder.cpp` (`collect_globals`/`build_module_init`/`build_module_shutdown`), `compiler/c_emitter.cpp`, `vm/vm.cpp`

### Control Flow
**When Statement** - Pattern matching on enum values with phi node support for variable modifications.
**Tests:** `tests/e2e/test_when.cpp`

### Traits
**Traits** - Ad-hoc polymorphism with trait declarations, required/default methods, `for Trait` implementations, trait inheritance, `Self` type, operator dispatch (arithmetic, comparison, bitwise, unary, indexing) for structs, primitives, and lists via unified `TypeCache::lookup_method()`, and generic traits with type parameters (`trait Add<Rhs>`, `for Mul<i32>`).
**Details:** `docs/internals/traits.md`, `docs/internals/operator-overloading.md` | **Tests:** `tests/e2e/test_traits.cpp`

### Generics
**Generics** - Parametric polymorphism with monomorphization. Generic functions (`fun identity<T>(v: T): T`) and generic structs (`struct Box<T> { value: T; }`). Supports local type inference from function arguments and struct field values (`identity(42)` infers T=i32, `Box { value = 42 }` infers T=i32). Explicit type arguments also supported. Angle bracket syntax with trial-parse disambiguation. Trait bounds on type parameters (`<T: Printable>`, `<T: Add<i32> + Hash>`) with Phase A instantiation-site checking and Phase B definition-site checking (bounded generic bodies are validated against declared trait bounds). User-defined external methods on generic structs (`fun Box<T>.get(): T`) with monomorphization. User-defined constructors/destructors on generic structs (`fun new Box<T>(v: T)`, `fun delete Box<T>()`).
**Details:** `docs/internals/generics.md` | **Tests:** `tests/e2e/test_generics.cpp`

### Exception Handling
**Exceptions** - Structured error recovery via `try`/`catch`/`throw`/`finally`. Built-in `Exception` trait with required `message(): string` method. Concrete-type catch matching via `type_id` comparison, catch-all with opaque `ExceptionRef` type. Handler tables for zero-overhead on non-exception path. Stack unwinding with frame cleanup. Supported in both backends: the C backend uses a checked-return model (thread-local in-flight exception + per-try dispatch labels + null-guarded cleanup) since it has no runtime PC-range handler table.
**Details:** `docs/internals/exceptions.md` | **Tests:** `tests/e2e/test_exceptions.cpp`

### Coroutines
**Coroutines** - Generator-style stackless coroutines via `Coro<T>` built-in type. Compile-time state machine transformation producing init/resume/done functions. Yield in straight-line code and if/else branches. Graph-preserving block cloning for correct control flow. `Coro<T>` is noncopyable (RAII cleanup of heap-allocated state struct). Promoted `uniq`/noncopyable fields are cleaned up by a generated `__coro_*$$delete` destructor; null-ification on done path prevents double-free. Supported in both backends — since lowering precedes codegen, the C backend emits `Coro<T>` as a state-struct pointer with no dedicated logic.
**Details:** `docs/internals/coroutines.md` | **Tests:** `tests/e2e/test_coroutines.cpp`

### Closures
**Closures** - First-class functions and closures via `fun(...) -> R` type syntax and lambda expressions. `IROp::Closure` + `CALL_INDIRECT` opcode for indirect dispatch. Implicit copy capture for copyable values (primitives, copyable structs, `ref`/`weak`); explicit `[move x]` capture for noncopyables with use-after-move enforcement. Function references (`var f = double`) lower to per-target trampoline closures. Nested closures with transitive captures (captures flow through enclosing envs at any depth). `self` capture in methods with three modes: implicit `ref self` (default), `[copy self]` (struct-value snapshot), `[weak self]` (cycle-breaker); ref/weak self on copyable receivers emits a runtime slab-range check that traps on stack-allocated receivers. Capture-aware destructor codegen for envs holding noncopyable captures.
Supported in both backends: the C backend dispatches `CallIndirect` through a per-module `g_closure_fns[]` table indexed by `__call_idx` (the AOT analogue of the VM's function table), with a type-erased `__closure_delete` and an `AssertHeap` → `roxy_heap_owns` trap.
**Details:** `docs/internals/closures.md` | **Tests:** `tests/e2e/test_closures.cpp`, `tests/e2e/test_c_backend.cpp`

### C Backend (Phases 1–4 + Phase 5 partial)
**CEmitter** - AOT compilation via SSA IR → C/C++ transpilation. Phases 1–2 cover primitives, arithmetic, comparisons, logical/bitwise operations, type conversions, control flow (goto/branch/return with block arguments), function calls, struct/enum type definitions (dependency-sorted), struct field access (StackAlloc, GetField, SetField, GetFieldAddr, StructCopy), pointer operations (LoadPtr, StorePtr, VarAddr for out/inout params), large struct returns (hidden output pointer), struct inheritance (explicit pointer casts), tagged unions, Cast, and Nullify. Phase 3 adds the runtime library (`roxy_rt.h`/`.cpp`) with allocation, ref counting, weak refs, strings, lists, maps (incl. struct keys with custom hash/eq), `to_string` conversions, and `print`; C++ RAII templates (`roxy::uniq<T>` / `roxy::ref<T>` / `roxy::weak<T>`) and container wrappers (`roxy::String` / `roxy::List<T>` / `roxy::Map<K,V>`); IR ops `New`/`Delete`/`RefInc`/`RefDec`/`WeakCheck`/`ConstString`/`CallNative`; and `emit_header()` producing a public `.hpp` with pub enum typedefs, pub struct definitions with inline C++ method wrappers, `make_<T>` / `make_<T>__<ctor>` factories returning `roxy::uniq<T>`, and pub function declarations. Phase 4 wires `roxy_ctx` (thread-local context with `roxy_ctx_init`/`roxy_set_ctx`/`roxy_get_ctx` + `roxy::ScopedContext` RAII guard); `RoxyVM` embeds `roxy_ctx ctx` as its first member and the interpreter activates it on every public entry; AOT-generated `main()` brackets `main_entry()` with `roxy_rt_init`/`roxy_rt_shutdown`. **Runtime unification**: slab allocator + vmem moved to `roxy_rt`; `roxy_alloc` dispatches through `roxy_ctx.allocator` (slab in both VM and AOT modes — AOT gains generation-based weak-ref soundness); `ObjectHeader` / `StringHeader` (now `{length, hash}` with cached XXH3) / `ListHeader` / `MapHeader` are unified definitions in `roxy_rt.h`; string intern table moved into ctx; `vm/string.cpp` / `list.cpp` / `map.cpp` are thin shims over `roxy_rt`'s implementations; VM-side struct-key Hash/Eq dispatch routes through a thread-local trampoline (`vm/map_dispatch.cpp`) that bridges into `call_user_function`; bytecode dispatch indices (`hash_fn_idx` / `eq_fn_idx`) live in a per-VM `tsl::robin_map` side-table on `RoxyVM` rather than in `MapHeader`; `rx::RoxyString` / `rx::RoxyList<T>` / `rx::RoxyMap<K,V>` are now `using` aliases of `roxy::String` / `roxy::List<T>` / `roxy::Map<K,V>`. **Native binding**: `RoxyVM*` dropped from embedder native function signatures — `bind<>`'d functions are plain `Ret(Args...)`, calling `roxy_get_ctx()` if they need runtime state. AOT `emit_native_call` consults `CEmitterConfig::native_registry` on static-table misses and emits a typed direct call using the entry's `aot_symbol_name` (defaults to the registered Roxy name; `bind<>(roxy_name, aot_symbol)` lets them diverge). The CEmitter pre-scans IR for user-native CallNative ops and emits `extern Ret name(Args...);` declarations in the source preamble, so AOT binaries link against either inline-defined `native_include_paths` headers or separately-compiled `.cpp` translation units. **Coroutines** are supported: since `coroutine_lower()` runs before codegen, `Coro<T>` emits as a pointer to its synthesized state struct (appended to `IRModule::struct_types` in the lowering pass), and deleting one runs the generated `__coro_*$$delete`. **Exceptions** are supported via a checked-return model: a thread-local in-flight exception (`roxy_set_pending`/`roxy_exception_*` in `roxy_rt`), per-try `__dispatch_<id>` labels reached by `throw`/pending-after-call checks, and null-guarded per-frame cleanup (reusing `emit_typed_delete`). **Closures** are supported: `Closure`/`CallIndirect` dispatch through a per-module `g_closure_fns[]` table indexed by `__call_idx`, `AssertHeap` → `roxy_heap_owns` trap, type-erased `__closure_delete`. **All language features are now covered by the C backend** (one known gap: Roxy identifiers that are C++ keywords, e.g. a function named `double`, collide in the generated source).
**Details:** `docs/internals/c-backend.md` | **Files:** `compiler/c_emitter.hpp`, `compiler/c_emitter.cpp`, `rt/roxy_rt.h`, `rt/roxy_rt.cpp`, `rt/slab_allocator.{hpp,cpp}`, `rt/vmem.hpp`, `rt/vmem_{unix,win32}.cpp`, `rt/string_intern.{hpp,cpp}`, `vm/map_dispatch.{hpp,cpp}`, `vm/binding/binder.hpp`, `vm/binding/registry.hpp` | **Tests:** `tests/e2e/test_c_backend.cpp`, `tests/unit/test_runtime_ctx.cpp`

### LSP Server (Phases 1–7)
**LSP Parser** - Error-recovering parser producing a lossless CST. Three recovery strategies: synthetic token insertion, statement boundary synchronization, bracket-aware skipping. Handles all grammar productions from the compiler parser.
**Details:** `docs/internals/lsp-server.md` | **Files:** `lsp/syntax_tree.hpp`, `lsp/lsp_parser.hpp`, `lsp/lsp_parser.cpp`

**LSP Transport** - JSON-RPC over stdin/stdout with Content-Length framing.
**Files:** `lsp/transport.hpp`, `lsp/transport.cpp`

**LSP Server** - Request dispatch, document management, diagnostics, go-to-definition, completions, hover, find references, rename. Supports initialize/shutdown/exit lifecycle, full document sync.
**Files:** `lsp/server.hpp`, `lsp/server.cpp`, `lsp/protocol.hpp`

## Planned Components (Not Yet Implemented)

- C backend Phase 5 — function-level + statement-level `#line` directives landed. Other Phase 5 items (DCE, Relooper, `switch` lowering, readable variable names) deliberately not pursued — the C compiler's optimizer covers them and they don't affect debugger UX. See `docs/internals/c-backend.md`.
- LSP Phase 8: Full semantic analysis (TypeCache/TypeEnv integration)
- LSP Phase 9: Polish (signature help, code actions, workspace symbols, semantic tokens)
- Optimization future phases: global CSE / GVN, loop-invariant code motion, function inlining, tail-call optimization, escape analysis (see `docs/internals/optimization.md`)

## Testing

- **Framework:** doctest (vendored in `include/roxy/core/doctest/`)
- **Single executable:** `roxy_tests` contains all unit and E2E tests
- **Helpers:** `tests/e2e/test_helpers.hpp` provides `compile()`, `compile_and_run()`, `run_and_capture()`, `compile_to_cpp()`, `compile_and_run_cpp()`

### Running Tests

Tests are grouped into doctest `TEST_SUITE`s (one per file). E2E suites are named
`E2E <Category>` (e.g. `E2E Structs`, `E2E C Backend`); unit suites are bare
(e.g. `Lexer`, `IR Optimize`, `LSP Hover`). Filter by suite:

```bash
cd build
./roxy_tests                                # Run all tests
./roxy_tests --test-suite-exclude="E2E*"    # Run only unit tests
./roxy_tests --test-suite="E2E*"            # Run only E2E tests
./roxy_tests --test-suite="E2E Structs"     # Run a specific suite
./roxy_tests --test-case="*field access*"   # Run cases matching a name
./roxy_tests --list-test-suites             # List all suites
./roxy_tests --list-test-cases              # List all test cases
```

On Windows, use `.exe` extension.

**Note for Claude Code:** C backend tests (`--test-suite="E2E C Backend"`) invoke the system C++ compiler to compile and run generated code, so they require running outside the sandbox (`dangerouslyDisableSandbox: true`). All other tests run fine inside the sandbox. (ASAN is currently disabled — see the AddressSanitizer note above; when re-enabled, ASAN builds also need to run outside the sandbox for the symbolizer.)

## Documentation

- `CLAUDE.md` - Quick reference for Claude Code (this file)
- `docs/overview.md` - Language design philosophy and roadmap
- `docs/grammar.md` - Grammar specification, numeric literals, type casting
- `docs/libraries.md` - Vendored library documentation
- `docs/internals/` - Detailed implementation documentation:
  - `vm.md` - VM state, interpreter loop, value representation
  - `bytecode.md` - Instruction encoding, opcode reference
  - `ssa-ir.md` - Block arguments, lowering to bytecode
  - `memory.md` - Reference types, object header, slab allocator
  - `structs.md` - Stack-allocated structs, slot-based layout, struct parameters/returns
  - `list.md` - Dynamic lists (`List<T>`), bounds checking
  - `maps.md` - Hash tables (`Map<K, V>`), Robin Hood open addressing
  - `strings.md` - String objects, concatenation, comparison
  - `interop.md` - Native functions, automatic C++ binding
  - `frontend.md` - Lexer, parser, semantic analysis
  - `modules.md` - Module system, imports, multi-file compilation
  - `globals.md` - Module-level globals: storage, `__module_init`/`__module_shutdown`, GLOBAL_ADDR, RAII teardown
  - `constructors.md` - Named constructors/destructors, `self` keyword
  - `methods.md` - Struct methods, `self` parameter, name mangling
  - `inheritance.md` - Struct inheritance, subtyping, `super` keyword
  - `tagged-unions.md` - Discriminated unions with `when` clause
  - `recursive-types.md` - Self-referential / mutually recursive structs via `uniq`, value-cycle detection, descriptor-driven recursive destruction
  - `traits.md` - Traits: declarations, required/default methods, trait inheritance, operator dispatch
  - `operator-overloading.md` - Operator traits (arithmetic, comparison, bitwise, unary) with unified primitive/struct dispatch
  - `generics.md` - Generic functions and structs with monomorphization
  - `exceptions.md` - Exception handling: try/catch/throw/finally, Exception trait, handler tables
  - `coroutines.md` - Coroutines: Coro<T>, yield, state machine transformation, graph-preserving block cloning
  - `closures.md` - Closures and first-class functions: function types, lambdas, capture modes, function references, self capture
  - `c-backend.md` - C backend (AOT compilation via SSA IR → C): Phases 1–4 + Phase 5 partial (function- and statement-level `#line` directives) implemented; remaining Phase 5 items (DCE, Relooper, `switch` lowering) deliberately not pursued
  - `lsp-server.md` - LSP server architecture: map-reduce design, error-recovering parser, indexing, lazy analysis
  - `optimization.md` - SSA IR optimization passes: Phase 1 (in IRBuilder), Phase 2 (DCE, copy propagation), Phase 3 (branch folding, block merging, trivial block-arg elim), and Phase 4 (block-local CSE) all implemented; future phases (global CSE/GVN, LICM, inlining, TCO, escape analysis) design plan
