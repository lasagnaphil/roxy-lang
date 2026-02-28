# Coroutines

Roxy supports generator-style stackless coroutines via the built-in `Coro<T>` type. Coroutine functions are transformed at compile time into state machines, producing init, resume, and done functions with no special bytecode opcodes required.

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
    var x: i32 = gen.resume();   // 0
    var y: i32 = gen.resume();   // 1
    var z: i32 = gen.resume();   // 1
    var finished: bool = gen.done();  // false
    gen.resume();                // runs past last yield, now done
    finished = gen.done();       // true
    return x + y + z;           // 2
}
```

## Grammar

```
yield_stmt = "yield" expression ";" ;
```

## Language Rules

- `yield` can only appear inside a coroutine function (one whose return type is `Coro<T>`)
- The yielded expression must be assignable to `T`
- Coroutine functions cannot return a value; only bare `return;` is allowed (use `yield` instead)
- `Coro<T>` is a built-in parameterized type — it cannot be user-defined
- `Coro<T>` requires exactly one type argument
- Calling a coroutine function returns a `Coro<T>` object; it does not execute the body
- `.resume()` executes the body up to the next `yield`, returning the yielded value
- `.done()` returns `true` after execution has passed the last yield point

## Coro\<T\> Type

`Coro<T>` is represented by `TypeKind::Coroutine` with `CoroutineTypeInfo`:

```cpp
struct CoroutineTypeInfo {
    Type* yield_type;              // T in Coro<T>
    Type* generated_struct_type;   // Synthetic struct holding coroutine state
    Span<MethodInfo> methods;      // resume() and done()
    StringView func_name;          // Name of the coroutine function (for method mangling)
};
```

Two methods are registered on each `Coro<T>` type:

| Method | Signature | Description |
|--------|-----------|-------------|
| `resume()` | `() → T` | Execute to next yield, return yielded value |
| `done()` | `() → bool` | Check if coroutine has finished |

## Pipeline

### Lexer & Parser

One keyword: `yield` (`KwYield`).

One AST node:

```cpp
struct YieldStmt {
    Expr* value;  // Expression to yield
};
```

The parser matches `TokenKind::KwYield`, parses the yield expression, and requires a trailing semicolon.

### Semantic Analysis

**Coro\<T\> detection:** When the parser encounters `Coro` as a type name with one type argument, the semantic analyzer calls `TypeCache::coroutine_type(yield_type)` to create the interned type. It then calls `populate_coro_methods()` to register `resume()` and `done()` methods.

**Function-level tracking:** When analyzing a function declaration, if the return type is a coroutine (`return_type->is_coroutine()`), the analyzer sets `m_in_coroutine = true` and `m_coro_yield_type` to the yield type. These are restored after the function body is analyzed.

**Yield validation:** Checks that (1) we are inside a coroutine function, and (2) the yielded expression is assignable to the coroutine's yield type.

**Return validation:** Inside coroutine functions, `return` with a value produces an error: `"coroutine functions cannot return a value; use 'yield' instead"`. Bare `return;` is allowed.

**Method population:** `populate_coro_methods()` creates a per-function `Coro<T>` type via `coroutine_type_for_func()` (not interned, stores the function name for method name mangling) and registers two methods:

- `resume()` → native name `__coro_<func_name>$$resume`
- `done()` → native name `__coro_<func_name>$$done`

### IR Generation

**Coroutine metadata:** When the IR builder encounters a function returning `Coro<T>`, it sets metadata on `IRFunction`:

```cpp
bool is_coroutine = false;
Type* coro_yield_type = nullptr;      // T in Coro<T>
Type* coro_struct_type = nullptr;     // Synthetic struct holding state
Type* coro_type = nullptr;            // The Coro<T> type itself
```

**Yield instruction:** `IROp::Yield` is a block terminator (like `Return` and `Throw`). Its `unary` operand holds the yielded value.

**Live variable capture:** After emitting the `Yield` instruction, the IR builder:

1. Collects all currently live local variables (names, values, types) from the scope stack
2. Creates a `coro.resume` block with one block parameter per live variable
3. Finishes the current block with a `Goto` to the resume block, passing current values as block arguments
4. Switches to the resume block and updates locals to point to the new block parameters

This captures the exact set of variables that need to survive across yield points, using the existing SSA block argument mechanism.

### Coroutine Lowering Pass

The coroutine lowering pass (`coroutine_lower()`) runs after IR generation and before bytecode lowering. It transforms each coroutine function into three ordinary functions.

**Yield point discovery:** Scans all blocks for `IROp::Yield` instructions. Each yield block must be terminated by a `Goto` to its resume block (set up by the IR builder).

```cpp
struct YieldPoint {
    u32 block_index;          // Block containing the Yield
    u32 inst_index;           // Instruction index within the block
    ValueId yielded_value;    // The value being yielded
    BlockId resume_block_id;  // Target resume block
};
```

**Promoted variable identification:** Extracts variable names and types from resume block parameters. These are the locals that must be saved/restored across yield points. Deduplication ensures each variable appears once in the struct.

```cpp
struct PromotedVar {
    StringView name;
    Type* type;
    u32 field_slot_offset;    // Slot offset in the coroutine struct
    u32 field_slot_count;     // Slot count of this field
};
```

**Synthetic struct generation:** Creates a struct named `__coro_<func_name>` with fields:

| Field | Type | Purpose |
|-------|------|---------|
| `__state` | `i32` | Current state machine state |
| `__yield_val` | `T` | Cached yield value |
| `<param>` | (varies) | One field per function parameter |
| `<local>` | (varies) | One field per promoted local variable |

**Init function:** Replaces the original function. Same name and parameters, returns `Coro<T>`. Body:

1. Allocate the synthetic struct with `New`
2. Set `__state = 0`
3. Copy each parameter to its struct field
4. Return the struct as a `Coro<T>` object

**Resume function:** Named `__coro_<func_name>$$resume`. Takes `ref<struct>` as `self`, returns `T`. Body:

1. Load `__state` from struct
2. Dispatch chain: `if state == 0 goto state_0; elif state == 1 goto state_1; ... else trap`
3. Each state entry block loads parameters and promoted locals from struct fields
4. At yield points: save yielded value to `__yield_val`, set `__state = next_state`, save promoted locals, return yielded value
5. At return points: set `__state = CORO_STATE_DONE` (0x7FFFFFFF), return default value

**Done function:** Named `__coro_<func_name>$$done`. Takes `ref<struct>` as `self`, returns `bool`. Loads `__state`, compares with `CORO_STATE_DONE`, returns the boolean result.

**Graph-preserving block cloning:** The resume function must contain the control flow graph for each state. For each state entry point, BFS discovers reachable blocks — stopping at blocks containing yield instructions (they belong to the current state but their successors start the next state). Each reachable block is cloned with fresh value IDs, and all operand references and terminator targets are remapped to the cloned copies. This preserves the original control flow structure (branches, merges) within each state.

### Bytecode

No new opcodes are required. The coroutine lowering pass transforms all `Yield` instructions into ordinary field stores and returns before bytecode lowering runs. If a `Yield` instruction reaches bytecode lowering, it triggers an assertion failure:

```cpp
case IROp::Yield:
    assert(false && "IROp::Yield should have been lowered by coroutine_lower()");
```

The generated init/resume/done functions use only standard opcodes: `New`, `GetField`, `SetField`, `EqI`, `Branch`, `Goto`, `Return`, etc.

## Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Detection mechanism | Return type `Coro<T>` | No special keyword needed; fits existing type system |
| Execution model | Stackless state machine | Simple, no stack copying; suits generator pattern |
| Lowering level | IR-level transformation | Reuses existing SSA infrastructure; no bytecode changes |
| Done sentinel | `__state == 0x7FFFFFFF` | Positive sentinel; state values 0..N used for yield points |
| Block cloning | Graph-preserving BFS | Preserves control flow structure within each state |
| Live variable capture | Block arguments | Natural fit with SSA block parameter mechanism |

## Future Work

- **Yield in loops** (`for`/`while`) — requires back-edge handling in BFS reachability
- **Yield in `try`/`catch`/`finally`** — exception handler control flow not traversed
- **Yield in `when` statements** — pattern match branches untested
- **Deeply nested yield** (nested if/else beyond 1 level) — untested
- **Sending values into coroutine** via `resume(value)` — currently resume takes no arguments
- **Async/await sugar** — higher-level syntax built on coroutines
- **Coroutine composition / `yield from`** — delegate to nested coroutine
- **Pre-lowering validation pass** — replace runtime assertions with compile-time errors for unsupported yield placements

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
| `src/roxy/compiler/ir_builder.cpp` | `gen_yield_stmt()`, live variable capture, resume blocks |
| `src/roxy/compiler/coroutine_lowering.cpp` | State machine transformation: init/resume/done functions |
| `src/roxy/compiler/lowering.cpp` | Yield assertion (must not reach bytecode) |
| `src/roxy/compiler/ir_validator.cpp` | Post-lowering Yield validation |
| `tests/e2e/test_coroutines.cpp` | E2E test suite (8 tests) |
