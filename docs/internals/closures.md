# Closures and First-Class Functions

Roxy has first-class functions and closures: `fun(...) -> R` function types, lambda expressions, and capture of enclosing variables. Closures are heap-allocated env structs that reuse the existing struct machinery — there is no closure-specific runtime.

The full surface is implemented: function types, lambdas, captures (implicit copy / `[move]` / `[copy self]` / `[weak self]`) with use-after-move enforcement, function references (script / native / imported / generic, including cross-module), nested closures with transitive captures, and `self` capture in methods. `[move self]` is intentionally unsupported — refactor to a free function taking `uniq Self`.

## Syntax

### Function types

`fun` with parenthesized parameter types and `->` for the return type. Void-returning types omit the arrow.

```roxy
var callback: fun(i32, i32) -> i32;
var action: fun();                 // no params, void return

fun make_adder(n: i32): fun(i32) -> i32 {
    return fun(x: i32): i32 { return x + n; };
}
```

The `->` distinguishes function *types* from the `:` used in variable/parameter annotations and in lambda/function declarations.

### Lambdas

Block body, expression body (`=>`), and void:

```roxy
var add   = fun(x: i32, y: i32): i32 { return x + y; };
var add2  = fun(x: i32, y: i32): i32 => x + y;
var greet = fun(name: string) { print(f"hello {name}"); };
```

Function values are called like any function (`f(21)`). A bare named function used as a value is a **function reference**: `var f: fun(i32) -> i32 = double;`.

## Capture Semantics

Roxy captures **by value (copy)** by default, consistent with its value semantics. Noncopyable variables cannot be captured implicitly — they require an explicit `[move x]` capture list, which consumes the outer variable (use-after-move is enforced).

| Variable type | Capture |
|---|---|
| Copyable (primitives, copyable structs) | implicit copy |
| `ref T` / `weak T` | implicit copy (the reference is copied; shares the object) |
| `uniq T`, `List<T>`, `Map<K,V>`, noncopyable struct | `[move x]` required, else compile error |

```roxy
var items: List<i32> = List<i32>();
var bad  = fun(): i32 { return items.len(); };             // ERROR: implicit capture of noncopyable
var good = fun[move items](): i32 { return items.len(); }; // OK; `items` is consumed here
```

Copyable variables are still captured implicitly when a `[move]` list is present — only noncopyables need listing.

### `self` capture in methods

A lambda inside a method that references `self` captures it in one of three modes:

- **Implicit `ref self`** (default) — ref-counted; safe for heap receivers, cycle-prone. For *copyable* structs whose receiver might be stack-allocated, an `AssertHeap` runtime check traps a stack receiver (the trap message points at `[copy self]`).
- **`[copy self]`** — stores a value snapshot of the struct (copyable structs without `when` clauses only).
- **`[weak self]`** — stores a `weak Self` cycle-breaker; same heap check as ref-self on copyable receivers.

## Runtime Representation

A function value is a `uniq` pointer to a heap-allocated **env struct** (2 slots, pointer width). The layout reuses ordinary struct machinery:

```
[ObjectHeader][__call_idx: u32][capture_0]...[capture_N]
```

`__call_idx` (slot 0) is the target function's index; `CALL_INDIRECT` reads it to dispatch. Captures follow in declaration order with their natural slot counts. Function values are `uniq`-flavored — owned by one variable, moved when passed, shared via `ref fun(...)` — so the existing move tracker, cleanup records, and typed delete handle destruction, including a synthesized destructor for noncopyable captures.

`TypeKind::Function` is the user-facing signature type; at codegen, function values are 2-slot pointers, type-erased from their concrete `__lambda_<id>_env` type. Closures reuse the `Ptr` value variant — no new runtime type.

### Calling a borrowed function (`ref fun`)

A `ref fun(...)` / `weak fun(...)` borrows a function value. Since a borrow shares the env-pointer representation, it is **callable**: the call paths (`analyze_regular_fun_call`, and the IR builder's local-var / struct-field / general indirect-call dispatch) unwrap the borrow with `base_type()` before reading `__call_idx`, then emit the same `CALL_INDIRECT`. This is what lets `List<fun>` indexing return a `borrowed fun` (= `ref fun`, see [memory.md](memory.md#the-borrowed-type-modifier)) that callers can both store and invoke without moving the closure out of the list.

A bare `fun` value also **converts** to `ref fun` / `weak fun` (`can_convert_ref`, mirroring `uniq → ref` / `uniq → weak` — a closure value is a heap env pointer), so passing a function to a borrowed-function parameter works and the caller keeps ownership. `fun → weak fun` runs through `WeakCreate` (`maybe_wrap_weak`) to capture the env's generation.

## How It Works

Each lambda is **lifted** to a top-level function; captured variables are read from the env through a hidden `__env` parameter:

```roxy
// Source
fun make_adder(n: i32): fun(i32) -> i32 {
    return fun(x: i32): i32 => x + n;
}

// After lifting (conceptually)
fun __lambda_0(__env: ref __lambda_0_env, x: i32): i32 { return x + __env.n; }
fun make_adder(n: i32): fun(i32) -> i32 {
    return Closure(__lambda_0_env, __lambda_0, n);   // allocate env, capture n
}
```

The analyzer synthesizes the env struct type and the lifted `__lambda_<id>_call` function per lambda. The IR builder emits `IROp::Closure` (allocate env, store `__call_idx` + captures) and `IROp::CallIndirect` for calls through a function-typed value. Lowering expands `Closure` into `NEW_OBJ` + `SET_FIELD`s and emits `CALL_INDIRECT` (0xDD); the interpreter reads `__call_idx`, places the env pointer in the callee's first register, and copies the explicit args after it.

### Function references

A bare function name used as a value lowers to a per-target **trampoline** closure (zero captures) whose body forwards to the real target. The trampoline's body op depends on the source:

| Source | Body op |
|---|---|
| Script / generic instantiation | `Call` |
| Native / imported native | `CallNative` |
| Imported script (cross-module) | `CallExternal` |

Trampolines are cached per target name. Generic templates are monomorphized at the reference site — either explicit (`identity<i32>`, via the parser's trial parse) or inferred from the expected function type at the assignment site. Cross-module instantiation resolves in the template's defining module.

### C backend

Closures also work through the AOT C backend. Lifted call functions and env
structs emit like ordinary functions/structs; `CallIndirect` dispatches through a
per-module `g_closure_fns[]` table indexed by `__call_idx` (the AOT analogue of the
VM's function table), and a type-erased `__closure_delete` runs env destructors.
`AssertHeap` maps to a `roxy_heap_owns` trap. See `docs/internals/c-backend.md`
("Closures").

### Nested closures

Captures flow through every enclosing lambda boundary: an inner lambda capturing an outer-scope variable records a capture in each lambda in between (the outermost reads the variable directly; inner ones read from the enclosing env). `[move x]` propagates the same way — you write `[move x]` only on the level that consumes it.

## Interaction with Other Features

- **Move semantics** — `[move]` captures reuse the existing move-state tracking (equivalent to passing the variable to a function parameter).
- **Generics** — generic functions accept function-typed parameters, and inference works (`map(list, fun(x: i32): i32 => x * 2)`).
- **Exceptions / coroutines** — closures are ordinary heap objects, cleaned up by the caller's cleanup records; a closure captured in coroutine state lives in the coroutine's state struct.
- **Traits** — closures have no trait dispatch; their type is a signature, not a named type.

## Files

| File | Purpose |
|---|---|
| `src/roxy/compiler/parser.cpp` | `fun(T)->R` types, lambda + capture-list parsing |
| `src/roxy/compiler/semantic.cpp` | capture analysis, lambda lifting, env-struct synthesis, self-capture modes |
| `src/roxy/compiler/ir_builder.cpp` | `IROp::Closure` / `CallIndirect`, function-reference trampolines |
| `src/roxy/compiler/lowering.cpp` | `Closure` → `NEW_OBJ` + `SET_FIELD`; `CALL_INDIRECT` |
| `include/roxy/vm/bytecode.hpp` | `CALL_INDIRECT` (0xDD), `ASSERT_HEAP` (0xDE) |
| `src/roxy/vm/interpreter.cpp` | `CALL_INDIRECT` / `ASSERT_HEAP` handlers |
| `tests/e2e/test_closures.cpp` | E2E tests |
