# Coroutines

Roxy has generator-style stackless coroutines via the built-in `Coro<T>` type. A coroutine function is transformed at compile time into a state machine — ordinary functions (init / resume / destructor) — so no special bytecode opcodes are needed. `Coro<T>` is a first-class value: `resume()` dispatches through the existing closure `CALL_INDIRECT` machinery and `done()` is an inlined `__state` check, so a coroutine can be passed, returned, and stored even when its source function is erased.

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
| `resume()` | `() → T` | Execute to next yield, return yielded value (dispatched via `CALL_INDIRECT`) |
| `done()` | `() → bool` | True once the coroutine has finished (inlined `__state` compare) |

Both dispatch dynamically, so a coroutine value is **first-class** — passable, returnable, and storable as an erased `Coro<T>` (see below).

## Pipeline

**Lexer & parser.** One keyword (`yield` / `KwYield`) and one AST node (`YieldStmt` holding the yielded expression). The parser matches `KwYield`, parses the expression, and requires a trailing semicolon.

**Semantic analysis.** When `Coro` appears as a type name with one type argument, the analyzer interns the type via `TypeCache::coroutine_type(yield_type)` and registers its `resume()` / `done()` methods. A function is classified as a coroutine (`FunDecl::is_coroutine`, set in `register_fun_signature`) iff its return type is `Coro<T>` **and its body contains a `yield`** — a non-yielding `Coro<T>`-returning function merely produces/forwards a first-class coroutine value and stays ordinary (may `return <coro value>`). While analyzing a real coroutine's body it sets `in_coroutine` / `coro_yield_type` (restored after the body), validates that each `yield` is inside a coroutine and assignable to the yield type, and rejects `return <value>`. A real coroutine's return type becomes a per-function `Coro<T>` (via `coroutine_type_for_func`, storing the function name for the state-struct wiring in coroutine lowering); `populate_coro_methods()` registers `resume` / `done`.

**IR generation.** A function returning `Coro<T>` gets coroutine metadata on its `IRFunction` (yield type, state struct type, `Coro<T>` type). `IROp::Yield` is a block terminator (like `Return`/`Throw`) whose operand is the yielded value. Immediately after emitting `Yield`, the builder captures the live locals across the yield using the existing SSA block-argument mechanism: it collects all live locals, creates a `coro.resume` block with one block parameter per live variable, terminates the current block with a `Goto` passing the current values, then switches to the resume block and rebinds the locals to its parameters.

## Coroutine Lowering Pass

`coroutine_lower()` runs after IR generation and before bytecode lowering, transforming each coroutine into three ordinary functions.

It first finds every `Yield` (each block ends in a `Goto` to its resume block) and derives the **promoted variables** from the resume blocks' parameters — the locals that must survive across yields, deduplicated. It then synthesizes a struct `__coro_<func>` holding the state machine:

| Field | Type | Slot | Purpose |
|-------|------|------|---------|
| `__resume_idx` | `u32` | 0 | Resume function's dispatch index (see "First-Class Coroutine Values") |
| `__state` | `i32` | 1 | Current state-machine state |
| `__yield_val` | `T` | 2 | Cached yield value |
| `<param>` | varies | 3… | One field per function parameter |
| `<local>` | varies | … | One field per promoted local |

The first two fields are a runtime layout contract: `__resume_idx` at slot 0 (so `CALL_INDIRECT` dispatches `resume()` on an erased value exactly like a closure env's `__call_idx`), `__state` at a fixed slot (so `done()` inlines a load+compare).

**Init function** replaces the original (same name/params, returns `Coro<T>`): allocate the struct with `New`, seed `__resume_idx` with `IROp::FuncIndex("__coro_<func>$$resume")` (resolved to the runtime function index at bytecode lowering / a `g_closure_fns[]` slot at C emission), set `__state = 0`, copy each parameter into its field, return the struct.

**Resume function** (`__coro_<func>$$resume`, takes `ref<struct> self`) is built by transforming the function in place:

- *Promote variables.* Replace the original params with a single `self`. In every block, prepend `GetField` loads for all promoted vars and remap uses of their original ValueIds to those loads; on each jump edge that carried promoted arguments, insert `SetField` stores before the terminator; then drop the promoted block parameters and arguments. Per-block loads (rather than a single global remap) are required because the IR builder rebinds local names to resume-block param ValueIds after a yield, so later blocks not dominated by the resume block would otherwise see non-dominating references. Catch blocks are special-cased: their exception block parameter is never treated as promoted, and if a catch variable name collides with a promoted var, a `SetField` at the top of the catch writes the exception value into the struct.
- *Split at yields, add dispatch.* Each `Yield` becomes `SetField(__yield_val)`, `SetField(__state, next_state)`, `Return(GetField(__yield_val))`. Each `Return` (coroutine end) becomes `SetField(__state, CORO_STATE_DONE)` + `Return(default)` (the default is `ConstInt 0` for a scalar yield type, or the zeroed `__yield_val` field for a struct — the value is never observed since `done()` is true). A dispatch if-else chain at entry branches on `__state` to the original entry (state 0) or the resume blocks (states 1..N), trapping on invalid states. Exception-handler BlockIds are remapped alongside terminator targets so handlers stay valid.

**No done function.** `done()` is inlined at every call site as a load of `__state` (fixed slot) compared against `CORO_STATE_DONE` (`0x7FFFFFFF`, a positive sentinel above all yield-point states) — no dispatch, uniform across known and erased values. The generated functions are init, resume, and `$$delete` (three, not four).

**Bytecode.** No new opcodes. All `Yield` instructions are lowered to field stores and returns before bytecode lowering; a `Yield` reaching lowering is an assertion failure. `IROp::FuncIndex` lowers to a `LOAD_INT` of the resolved function index. `resume()` lowers to the existing `CALL_INDIRECT`. The generated functions use only standard opcodes (`New`, `GetField`, `SetField`, `EqI`, `Branch`, `Goto`, `Return`, …).

**C backend.** Because lowering happens before either backend, coroutines also work through the AOT C backend with no dedicated emitter logic: a known `Coro<T>` emits as a pointer to the synthesized state struct, an erased `Coro<T>` as `void*` (with a `__coro_header` cast for `done()`), the generated functions emit like any other, resume dispatches through `g_closure_fns[]`, and deleting a `Coro<T>` runs `__coro_<func>$$delete` (directly when known, via `__closure_delete` when erased). See `docs/internals/c-backend.md` ("Coroutines").

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

## First-Class Coroutine Values

A `Coro<T>` is a first-class value: it can be assigned to an annotated variable, passed to a function, returned, and stored — even when the concrete coroutine function is unknown at the use site (an "erased" `Coro<T>`).

```roxy
fun ones(): Coro<i32> { yield 1; yield 1; }
fun twos(): Coro<i32> { yield 2; yield 2; }

fun sum_two(c: Coro<i32>): i32 {        // c is erased — could be either coroutine
    return c.resume() + c.resume();
}
fun main(): i32 {
    return sum_two(ones()) + sum_two(twos());   // 6
}
```

This works because resume/done dispatch **dynamically**, mirroring the closure design:

- **Two representations unify.** An annotated `Coro<T>` resolves to the interned generic type (empty `func_name`); a coroutine value carries its per-function type (`coroutine_type_for_func`). `TypeChecker::is_assignable` / `check_assignable` treat two coroutine types as assignable when their **yield types** match — `func_name` is irrelevant to dispatch.
- **`resume()` is a closure-style indirect call.** The state struct's slot 0 is `__resume_idx` (the resume function's dispatch index, seeded by `IROp::FuncIndex` at init), so `resume()` lowers to the existing `CALL_INDIRECT` — the interpreter reads slot 0 and dispatches, exactly like a closure env's `__call_idx`.
- **`done()` is inlined.** `__state` sits at a fixed slot in every coroutine struct, so `done()` is a load + compare against `CORO_STATE_DONE` — no dispatch, no `$$done` function.
- **Erased deletion.** An owned erased `Coro<T>` drops via `DropKind::Closure`: the VM dispatches the state-struct destructor by runtime `type_id` (`closure_env_dtors`), the C backend by `__resume_idx` (`__closure_delete`, with the resume function registered in `g_closure_fns[]`). Both run `__coro_<func>$$delete`, then free.

## Restrictions

- `yield` inside `finally` is a compile-time error — `finally` runs in multiple contexts (normal and exception exit), making coroutine state management infeasible.
- `return <value>` inside a coroutine (a function that yields) is a compile-time error — use `yield` to produce values; bare `return;` ends the coroutine early. (A non-yielding `Coro<T>`-returning function is not a coroutine and may return a coroutine value.)
- **Methods cannot be coroutines** — a method returning `Coro<T>` is rejected with "coroutine methods are not yet supported" (the body is still analyzed in coroutine context so its yields type-check without cascading errors). Needs the per-function coro type for methods plus a `self`-carrying state machine; use a free function taking the struct as a parameter instead. Tracked in TODO.md under Planned Features.

## Files

| File | Purpose |
|------|---------|
| `include/roxy/shared/token_kinds.hpp` | `KwYield` token |
| `include/roxy/compiler/ast.hpp` | `YieldStmt` AST node |
| `src/roxy/compiler/parser.cpp` | `yield_statement()` parser |
| `include/roxy/compiler/types.hpp` | `TypeKind::Coroutine`, `CoroutineTypeInfo` |
| `src/roxy/compiler/types.cpp` | `coroutine_type()`, `coroutine_type_for_func()`, `lookup_coro_method()`, `compute_drop_plan` (erased → `Closure`) |
| `src/roxy/compiler/type_checker.cpp` | Coroutine assignability (yield-type match) |
| `src/roxy/compiler/semantic.cpp` | yield-based coroutine classification (`stmt_contains_yield`), `populate_coro_methods()`, yield/return validation |
| `include/roxy/compiler/ssa_ir.hpp` | `IROp::Yield`, `IROp::FuncIndex`, coroutine metadata on `IRFunction` |
| `src/roxy/compiler/ir_builder_expr.cpp` | `resume()` → `CallIndirect`, `done()` → inline `__state` compare |
| `src/roxy/compiler/ir_builder.cpp` | `gen_yield_stmt()`, live-variable capture, resume blocks |
| `src/roxy/compiler/coroutine_lowering.cpp` | State machine transformation: init/resume/destructor, `__resume_idx` seeding |
| `src/roxy/compiler/lowering.cpp` | `FuncIndex` → `LOAD_INT`; `New` records dtor for erased delete; Yield assertion |
| `src/roxy/compiler/c_emitter.cpp` | Erased `Coro<T>` (`void*`, `__coro_header`), `FuncIndex`, coro resume in `g_closure_fns[]` |
| `src/roxy/compiler/ir_validator.cpp` | Post-lowering Yield validation |
| `tests/e2e/test_coroutines.cpp` | E2E test suite |
