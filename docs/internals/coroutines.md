# Coroutines

Roxy has generator-style stackless coroutines via the built-in `Coro<T>` type. A coroutine function is transformed at compile time into a state machine — three ordinary functions (init / resume / done) — so no special bytecode opcodes are needed.

## Syntax

```roxy
fun fibonacci(): Coro<i32> {
    var a: i32 = 0;
    var b: i32 = 1;
    yield a;
    yield b;
    var temp: i32 = a + b;
    a = b;
    b = temp;
    yield b;
}

fun main(): i32 {
    var gen = fibonacci();
    var x: i32 = gen.resume();        // 0
    var y: i32 = gen.resume();        // 1
    var z: i32 = gen.resume();        // 1
    var finished: bool = gen.done();  // false
    gen.resume();                     // runs past last yield, now done
    finished = gen.done();            // true
    return x + y + z;                 // 2
}
```

Grammar: `yield_stmt = "yield" expression ";"`.

## Language Rules

- `yield` can only appear inside a coroutine function (return type `Coro<T>`); the yielded expression must be assignable to `T`.
- A coroutine function cannot return a value — only bare `return;` is allowed (use `yield` to produce values).
- `Coro<T>` is a built-in parameterized type taking exactly one type argument; it cannot be user-defined.
- Calling a coroutine function returns a `Coro<T>` object without executing the body.
- `.resume()` executes the body up to the next `yield`, returning the yielded value.
- `.done()` returns `true` once execution has passed the last yield point.

## `Coro<T>` Type

`Coro<T>` is `TypeKind::Coroutine`, carrying `CoroutineTypeInfo` (yield type `T`, the generated state struct, the `resume`/`done` methods, and the function name used for method mangling — see `compiler/types.hpp`). Two methods are registered on each `Coro<T>`:

| Method | Signature | Description |
|--------|-----------|-------------|
| `resume()` | `() → T` | Execute to next yield, return yielded value |
| `done()` | `() → bool` | True once the coroutine has finished |

## Pipeline

**Lexer & parser.** One keyword (`yield` / `KwYield`) and one AST node (`YieldStmt` holding the yielded expression). The parser matches `KwYield`, parses the expression, and requires a trailing semicolon.

**Semantic analysis.** When `Coro` appears as a type name with one type argument, the analyzer interns the type via `TypeCache::coroutine_type(yield_type)` and registers its `resume()` / `done()` methods. While analyzing a function whose return type `is_coroutine()`, it sets `m_in_coroutine` / `m_coro_yield_type` (restored after the body), validates that each `yield` is inside a coroutine and assignable to the yield type, and rejects `return <value>`. `populate_coro_methods()` builds a per-function `Coro<T>` (storing the function name for mangling) and registers `resume` → `__coro_<func>$$resume` and `done` → `__coro_<func>$$done`.

**IR generation.** A function returning `Coro<T>` gets coroutine metadata on its `IRFunction` (yield type, state struct type, `Coro<T>` type). `IROp::Yield` is a block terminator (like `Return`/`Throw`) whose operand is the yielded value. Immediately after emitting `Yield`, the builder captures the live locals across the yield using the existing SSA block-argument mechanism: it collects all live locals, creates a `coro.resume` block with one block parameter per live variable, terminates the current block with a `Goto` passing the current values, then switches to the resume block and rebinds the locals to its parameters.

## Coroutine Lowering Pass

`coroutine_lower()` runs after IR generation and before bytecode lowering, transforming each coroutine into three ordinary functions.

It first finds every `Yield` (each block ends in a `Goto` to its resume block) and derives the **promoted variables** from the resume blocks' parameters — the locals that must survive across yields, deduplicated. It then synthesizes a struct `__coro_<func>` holding the state machine:

| Field | Type | Purpose |
|-------|------|---------|
| `__state` | `i32` | Current state-machine state |
| `__yield_val` | `T` | Cached yield value |
| `<param>` | varies | One field per function parameter |
| `<local>` | varies | One field per promoted local |

**Init function** replaces the original (same name/params, returns `Coro<T>`): allocate the struct with `New`, set `__state = 0`, copy each parameter into its field, return the struct.

**Resume function** (`__coro_<func>$$resume`, takes `ref<struct> self`) is built by transforming the function in place:

- *Promote variables.* Replace the original params with a single `self`. In every block, prepend `GetField` loads for all promoted vars and remap uses of their original ValueIds to those loads; on each jump edge that carried promoted arguments, insert `SetField` stores before the terminator; then drop the promoted block parameters and arguments. Per-block loads (rather than a single global remap) are required because the IR builder rebinds local names to resume-block param ValueIds after a yield, so later blocks not dominated by the resume block would otherwise see non-dominating references. Catch blocks are special-cased: their exception block parameter is never treated as promoted, and if a catch variable name collides with a promoted var, a `SetField` at the top of the catch writes the exception value into the struct.
- *Split at yields, add dispatch.* Each `Yield` becomes `SetField(__yield_val)`, `SetField(__state, next_state)`, `Return(GetField(__yield_val))`. Each `Return` (coroutine end) becomes `SetField(__state, CORO_STATE_DONE)` + `Return(default)`. A dispatch if-else chain at entry branches on `__state` to the original entry (state 0) or the resume blocks (states 1..N), trapping on invalid states. Exception-handler BlockIds are remapped alongside terminator targets so handlers stay valid.

**Done function** (`__coro_<func>$$done`, takes `ref<struct> self`) loads `__state`, compares it to `CORO_STATE_DONE` (`0x7FFFFFFF`, a positive sentinel above all yield-point states), and returns the boolean.

**Bytecode.** No new opcodes. All `Yield` instructions are lowered to field stores and returns before bytecode lowering; a `Yield` reaching lowering is an assertion failure. The generated functions use only standard opcodes (`New`, `GetField`, `SetField`, `EqI`, `Branch`, `Goto`, `Return`, …).

**C backend.** Because lowering happens before either backend, coroutines also work through the AOT C backend with no dedicated emitter logic: `Coro<T>` emits as a pointer to the synthesized state struct, the four generated functions emit like any other, and deleting a `Coro<T>` runs the generated `__coro_<func>$$delete` destructor. See `docs/internals/c-backend.md` ("Coroutines").

## Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Detection mechanism | Return type `Coro<T>` | No special keyword; fits the type system |
| Execution model | Stackless state machine | Simple, no stack copying; suits generators |
| Lowering level | IR-level transformation | Reuses SSA infrastructure; no bytecode changes |
| Done sentinel | `__state == 0x7FFFFFFF` | Positive sentinel above states 0..N |
| Two-phase in-place lowering | Promote then split | No block cloning or cross-function value remapping |
| Per-block `GetField` loads | Load all promoted vars per block | Handles non-dominating uses from name-based local rebinding |
| Live variable capture | Block arguments | Natural fit with SSA block parameters |

## Supported Yield Placements

- Straight-line code (sequential yields)
- `if`/`else` branches (including deeply nested)
- `while`/`for` loops, including nested loops and loops with `break`/`continue`
- `when` statements (enum pattern matching)
- `try` blocks and `catch` blocks (handlers are preserved; the exception parameter stays a non-promoted block param)

## Restrictions

- `yield` inside `finally` is a compile-time error — `finally` runs in multiple contexts (normal and exception exit), making coroutine state management infeasible.
- `return <value>` inside a coroutine is a compile-time error — use `yield` to produce values; bare `return;` ends the coroutine early.

## Files

| File | Purpose |
|------|---------|
| `include/roxy/shared/token_kinds.hpp` | `KwYield` token |
| `include/roxy/compiler/ast.hpp` | `YieldStmt` AST node |
| `src/roxy/compiler/parser.cpp` | `yield_statement()` parser |
| `include/roxy/compiler/types.hpp` | `TypeKind::Coroutine`, `CoroutineTypeInfo` |
| `src/roxy/compiler/types.cpp` | `coroutine_type()`, `coroutine_type_for_func()`, `lookup_coro_method()` |
| `src/roxy/compiler/semantic.cpp` | Coro\<T\> detection, `populate_coro_methods()`, yield/return validation |
| `include/roxy/compiler/ssa_ir.hpp` | `IROp::Yield`, coroutine metadata on `IRFunction` |
| `src/roxy/compiler/ir_builder.cpp` | `gen_yield_stmt()`, live-variable capture, resume blocks |
| `src/roxy/compiler/coroutine_lowering.cpp` | State machine transformation: init/resume/done |
| `src/roxy/compiler/lowering.cpp` | Yield assertion (must not reach bytecode) |
| `src/roxy/compiler/ir_validator.cpp` | Post-lowering Yield validation |
| `tests/e2e/test_coroutines.cpp` | E2E test suite |
