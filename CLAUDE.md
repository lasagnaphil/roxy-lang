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
// Add/Mul are ordinary traits, not builtins — they must be declared.
trait Add<Rhs>;
fun Add.add(other: Rhs): Self;

trait Mul<Rhs>;
fun Mul.mul(other: Rhs): Self;

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
- **C++ Standard:** C++20 (compiler internals; the AOT-emitted C/C++ runtime stays C++17-clean)

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
- `roxy_compiler` - Parser, AST, semantic analysis, SSA IR, IR builder, and **both** codegen backends (bytecode lowering + C emission)
- `roxy_rt` - Unified runtime (allocation, slab allocator, vmem, strings, lists, maps, intern). Used by both `roxy_vm` and AOT-compiled programs.
- `roxy_vm` - Bytecode, value, object, VM, interpreter
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

### Warnings, clang-format, clang-tidy

`-Wall -Wextra` is on by default (`ENABLE_WARNINGS`, `/W4` on real MSVC), with
`-Wunused-parameter` and `-Wmissing-field-initializers` disabled project-wide —
see the comment in `CMakeLists.txt` for why. **The build is warning-clean; keep
it that way.**

`.clang-format` is LLVM defaults overridden only where the house style differs
(4-space indent, 100 columns, `Type* ptr`, un-indented namespaces, indented case
labels). **The whole tree has been formatted with it**, so keep it clean:

```bash
clang-format -i <file>       # on files you touch
git-clang-format             # or just what you staged
```

Vendored code (`doctest/`, `tsl/`, `xxhash.h`, `third_party/`) is excluded via
`.clang-format-ignore`. Every file is at its formatting fixed point, so a
`--dry-run` check over the tree passes clean and is safe to wire into CI.

If you re-sweep the tree, note that **clang-format is not always idempotent in
a single pass** — wrapped trailing comments inside an aligned block can settle
only on the second pass, and `clang-format -i` runs exactly one. Re-run until
nothing changes. (Also: `--assume-filename` is ignored when clang-format is
given a file argument instead of stdin, which will silently fall back to LLVM
defaults and make an idempotency check lie to you.)

`.clang-tidy` is a narrow, high-signal check set (the broad `misc-*` style
checks are off — they produce ~3.4k mechanical findings that bury the useful
ones). Each exclusion carries its measured finding count and rationale in the
config itself — read those before re-enabling one.

**The tree is down to one finding** (`interpreter.cpp`'s C99 array designators,
a portability question for a real-MSVC build, not a defect). Keep it there.

Two things worth knowing when a new finding shows up:

- **Prefer `assert()` over `NOLINT` for null-path warnings.** clang-analyzer
  treats an assert as a path constraint, so asserting an invariant makes the
  path genuinely infeasible rather than merely silenced — and documents the
  contract. Most of the original backlog was a *vestigial null-guard*
  (`if (p && ...)` earlier in a function, an unguarded `p->` later); the guard
  is what created the null path, since `check_expr` never returns null (it
  yields the `error_type` sentinel — `docs/internals/error-handling.md`).
- **`clang-analyzer-security.ArrayBound` is taint-based** and flags any
  externally-derived index even when the bounds are provably correct. The two
  `NOLINT`s for it (`file.cpp`, `roxy_rt.cpp`) are that, not real gaps.

```bash
run-clang-tidy -p build 'src/roxy/.*\.cpp'    # needs Homebrew/upstream LLVM on PATH
```

Both tools need a full LLVM install (Apple clang ships neither); on macOS that's
`/opt/homebrew/opt/llvm/bin`.

## Project Structure

```
roxy-v2/
├── include/roxy/        # Headers; src/roxy/ mirrors this layout
│   ├── core/            # Utilities (types.hpp, span, vector, allocators, doctest)
│   ├── shared/          # Lexer and tokens
│   ├── compiler/        # Grouped by pipeline phase:
│   │                    #   types/ support/ parse/ sema/ ir/ codegen/ driver/
│   ├── lsp/             # Error-recovering parser, indexer, LSP server
│   ├── rt/              # Unified runtime (roxy_rt.h) — used by the VM and AOT programs
│   └── vm/              # Bytecode, VM, interpreter, natives, binding/
├── benchmarks/          # lox, mandelbrot, nbody, quicksort, struct_copy
├── examples/            # Runnable programs (incl. lox/ — a Lox interpreter in Roxy)
├── tests/               # unit/, e2e/, fuzz/ — one doctest binary, roxy_tests
└── docs/                # overview, grammar, libraries, internals/
```

`src/roxy/compiler/ir/` splits the IR builder across `ir_builder{,_expr,_stmt,_lifetime}.cpp`
with shared helpers in `ir_builder_internal.hpp`.

## Compiler Pipeline

```
Source → Lexer → Parser → AST → Semantic Analysis → IR Builder → SSA IR
       → coroutine_lower → optimize_module → IRValidator → Lowering → Bytecode → VM
                                                         ↘ CEmitter → C/C++ (AOT)
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

- Types/modifiers: `true false nil var fun struct enum trait pub native`
- Control flow: `if else for while break continue return when case try catch throw finally yield`
- OOP: `self super new delete`
- References: `uniq ref weak out inout`
- Imports: `import from`

See `docs/grammar.md` for numeric literal suffixes and type casting rules.

### Semantics worth knowing before writing Roxy

- **Containers are move-only.** `List<T>` / `Map<K, V>` move on binding and passing; `.copy()` makes an independent duplicate. A `ref List<T>` / `ref Map<K, V>` parameter borrows instead (no call-site marker; mutation through it is allowed, rebinding the caller's slot is not).
- **Index reads throw.** An out-of-bounds `list[i]` throws `IndexError`, a missing `m[k]` throws `KeyError` (both catchable); `.get()` / `.pop()` abort. `m.get_or(k, fallback)` never throws or inserts.
- **Move-only is structural.** A struct is move-only iff it has a user-written destructor or a move-only field; a `string` or `ref` field keeps it copyable (copies retain).
- **Printing.** `print` is an overload set over primitives; structs/enums/containers print through `Printable` (`f"{items}"` renders `[1, 2, 3]`). `weak` is not printable (it can dangle).
- **Methods are one-per-name** (no method overloading, including across trait impls); free functions and natives may be overloaded.
- **Variant fields of a tagged union are checked at runtime**, not compile time — reading the wrong variant traps.
- **`when` exhaustiveness is detected, not required.** Covering every variant (with no `else`) counts as all-paths-return and sharpens `uniq` move-state merges; the impossible fall-through is compiled to a trap.
- **Statements are only valid inside functions**; module scope holds declarations and `var` globals (initialized before `main`, torn down after).
- **Coroutines:** a function is a coroutine iff it returns `Coro<T>` and its body yields. Not yet supported as methods of generic structs or traits.

## Where things are documented

Each feature has an internals doc under `docs/internals/`; read it before changing that area.

| Area | Doc |
|------|-----|
| Lexer, parser, semantic analysis (and its collaborators) | `frontend.md`, `error-handling.md` (never-null `error_type` sentinels) |
| **Ownership, borrows, drop/retain/move-only, RAII, runtime heap** | **`lifetimes.md`** — the single memory/lifecycle reference |
| Structs, methods, constructors, inheritance, tagged unions, recursive types | `structs.md`, `methods.md`, `constructors.md`, `inheritance.md`, `tagged-unions.md`, `recursive-types.md` |
| Traits, operators, overloading, generics | `traits.md`, `operator-overloading.md`, `overloading.md`, `generics.md` |
| Lists, maps, strings | `list.md`, `maps.md`, `strings.md` |
| Exceptions, coroutines, closures | `exceptions.md`, `coroutines.md`, `closures.md` |
| Modules, globals, C++ interop | `modules.md`, `globals.md`, `interop.md` |
| SSA IR, optimizer, bytecode, VM | `ssa-ir.md`, `optimization.md`, `bytecode.md`, `vm.md`, `vm-optimization.md` |
| C backend (incl. the live "Known C-backend gaps" list) | `c-backend.md` |
| LSP server | `lsp-server.md` |
| Fuzzing, profiling | `fuzzer.md`, `profiling.md`, `identifier-interning.md` (a rejected optimization's post-mortem) |

Language-level: `docs/overview.md` (design and philosophy), `docs/grammar.md`, `docs/libraries.md`
(vendored libraries). At the repo root: `TODO.md` (known bugs and technical debt — check it before
assuming a feature works) and `OPTIMIZATION.md` (the compiler's compile-time performance program:
baseline, measurement rules, negative results).

### C backend: points worth knowing before touching it

- **Feature-complete ≠ bug-free.** Every language feature has a codegen path, but narrow gaps remain, each pinned by a `// VM-only: C backend:` test case. `c-backend.md` → "Known C-backend gaps" is the live list.
- **Lowering order does the work.** `coroutine_lower()` runs before codegen, so `Coro<T>` is just a pointer to its state struct. Exceptions use a checked-return model (thread-local in-flight exception + per-try dispatch labels) since there is no runtime handler table. Closures dispatch through a per-module `g_closure_fns[]`.
- **The runtime is unified, not duplicated.** `roxy_rt` owns the allocator, object/string/list/map headers and the intern table; `vm/string.cpp` / `list.cpp` / `map.cpp` are thin shims, and `RoxyVM` embeds `roxy_ctx` as its first member.
- **Natives take no `RoxyVM*`.** `bind<>`'d functions are plain `Ret(Args...)` and call `roxy_get_ctx()` for runtime state; AOT emits a typed direct call via the entry's `aot_symbol_name`.
- There is no production driver: the C backend is reached through the test harness (`compile_to_cpp` / `compile_and_run_cpp`). Codegen quality (DCE, `switch` lowering, readable names) is deliberately left to the C compiler.

## Planned Components (Not Yet Implemented)

- LSP Phase 8: route every feature through `LspAnalysisContext` (several still answer from the string-typed `GlobalIndex`)
- LSP Phase 9: Polish (signature help, code actions, workspace symbols, semantic tokens)
- Optimization future phases: global CSE / GVN, loop-invariant code motion, function inlining, tail-call optimization, escape analysis (see `docs/internals/optimization.md`)

## Testing

- **Framework:** doctest (vendored in `include/roxy/core/doctest/`); one executable, `roxy_tests`.
- **Helpers:** `tests/e2e/test_helpers.hpp` — `compile()`, `compile_and_run()`, `run_and_capture()`, `compile_to_cpp()`, `compile_and_run_cpp()`.
- **Every VM program run asserts the teardown leak invariant.** `run_and_capture` checks that nothing is still alive after `main()` returns (immortal string literals aside), so every VM E2E test is also a leak test. A test pinning a *known* leak opts out with a scoped `ExpectedLeak` naming its `TODO.md` entry. VM only; the C backend runs no census (`lifetimes.md` → "The teardown invariant").
- **The E2E harness mirrors the real pipeline** (IRBuilder → `coroutine_lower` → `optimize_module` → validate → bytecode/C, the order of `Compiler::link_modules()`). Keep it that way — when the harness skipped the optimizer, a crash on *every* coroutine program hid behind a green suite.

### Running Tests

Tests are grouped into doctest `TEST_SUITE`s (one per file). E2E suites are named
`E2E <Category>` (e.g. `E2E Structs`, `E2E C Backend`); unit suites are bare
(e.g. `Lexer`, `IR Optimize`, `LSP Hover`).

```bash
cd build
./roxy_tests                                # Run all tests
./roxy_tests --test-suite-exclude="E2E*"    # Run only unit tests
./roxy_tests --test-suite="E2E Structs"     # Run a specific suite
./roxy_tests --test-case="*field access*"   # Run cases matching a name
./roxy_tests --list-test-suites             # List all suites
```

On Windows, use `.exe` extension.

**Backend-parametric E2E tests (`<VM>` / `<C>`):** most `tests/e2e/` suites run
each test on *both* backends via `TEST_CASE_TEMPLATE` (harness:
`tests/e2e/test_e2e_backend.hpp`), named `<TestName><VM>` / `<TestName><C>`:

```bash
./roxy_tests --test-case="*<VM>*"                                  # VM only (fast, sandbox-safe)
./roxy_tests --test-case="*<C>*"                                   # C backend only (needs system compiler)
./roxy_tests --test-case-exclude="*<C>*" --test-suite-exclude="E2E C Backend"  # everything compiler-free
```

Cases the C backend can't run are plain `TEST_CASE` with a `// VM-only: <reason>`
annotation. When a C-backend gap is fixed, convert its tests to `TEST_CASE_TEMPLATE`.

**`E2E CLI` (`tests/e2e/test_cli.cpp`)** runs the `roxy` binary itself (CMake passes
its path as `ROXY_CLI_PATH`), since the driver isn't linked into `roxy_tests`. It
asserts on *how the process exited*, not just stdout. Sandbox-safe.

**Note for Claude Code:** anything that exercises the C backend — the `*<C>*`
cases and the `E2E C Backend` suite — invokes the system C++ compiler, so it
must run outside the sandbox (`dangerouslyDisableSandbox: true`). Everything else
runs inside the sandbox. (When ASAN is re-enabled, ASAN builds also need to run
outside the sandbox for the symbolizer.)

### Fuzzing

libFuzzer targets live in `tests/fuzz/` (build with `-DENABLE_FUZZERS=ON` and a Clang
that ships libFuzzer — **not** Apple clang). The always-on `Fuzz Regression` and
`Structured Gen` suites replay the seed corpus, `examples/`, and fixed generator
seeds on every `roxy_tests` run. **Never** add an OOM/very-slow reproducer to
`tests/fuzz/corpus/` — the replay has no resource cap. Details: `docs/internals/fuzzer.md`,
quickstart: `tests/fuzz/README.md`.

### Profiling

Profile an **optimized** build, never the default `-O0` `build/`:

```bash
cmake -B build-profile -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_FLAGS="-fno-omit-frame-pointer"
ninja -C build-profile roxy
./build-profile/roxy --time program.roxy        # per-phase compile timing + compile-vs-execute split
./build-profile/roxy --repeat=200 program.roxy  # avg over 200 in-process compiles (profiler loop)
./build/roxy --check-leaks program.roxy         # heap objects still alive after main() (exit 70 if any)
./build/roxy_gen --seed=7 --modules=400 --out=/tmp/corpus_400   # ~257 KLOC seeded compile benchmark
```

The compiler and interpreter are separate regimes — isolate them. Opcode profiler:
`-DENABLE_BC_PROFILE=ON`; Tracy: `-DENABLE_TRACY=ON`. Full workflow: `docs/internals/profiling.md`.
