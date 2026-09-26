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

Captures follow `__call_idx` in declaration order. Function values are `uniq`-flavored — owned by one variable, moved when passed, shared via `ref fun(...)` — so the existing move tracker, cleanup records, and typed delete handle destruction, including a synthesized destructor for noncopyable captures. `TypeKind::Function` is the user-facing signature type; at codegen, function values are type-erased from their concrete `__lambda_<id>_env` type.

### Calling a borrowed function (`ref fun`)

A `ref fun(...)` / `weak fun(...)` borrows a function value. Since a borrow shares the env-pointer representation, it is **callable**: every call path unwraps the borrow with `base_type()` before reading `__call_idx`. This is what lets `List<fun>` indexing return a `borrowed fun` (= `ref fun`, see [lifetimes.md → The `borrowed` type modifier](lifetimes.md#the-borrowed-type-modifier)) that callers can both store and invoke without moving the closure out of the list.

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

The analyzer (`LambdaLifter`) synthesizes the env struct type and the lifted `__lambda_<id>_call` function per lambda. The IR builder emits `IROp::Closure` (allocate env, store `__call_idx` + captures) and `IROp::CallIndirect` for calls through a function-typed value; at runtime `CALL_INDIRECT` reads `__call_idx` and passes the env pointer as the callee's first argument.

### Function references

A bare function name used as a value lowers to a per-target **trampoline** closure (zero captures) whose body forwards to the real target. The trampoline's body op depends on the source:

| Source | Body op |
|---|---|
| Script / generic instantiation | `Call` |
| Native / imported native | `CallNative` |
| Imported script (cross-module) | `CallExternal` |

Trampolines are cached per target name. Generic templates are monomorphized at the reference site — either explicit (`identity<i32>`, via the parser's trial parse) or inferred from the expected function type at the assignment site. Cross-module instantiation resolves in the template's defining module.

### C backend

The C backend dispatches through a per-module `g_closure_fns[]` table indexed by
`__call_idx`; see [c-backend.md → Closures](c-backend.md#closures).

### Nested closures

Captures flow through every enclosing lambda boundary: an inner lambda capturing an outer-scope variable records a capture in each lambda in between (the outermost reads the variable directly; inner ones read from the enclosing env). `[move x]` propagates the same way — you write `[move x]` only on the level that consumes it.

## Interaction with Other Features

- **Move semantics** — `[move]` captures reuse the existing move-state tracking (equivalent to passing the variable to a function parameter).
- **Generics** — generic functions accept function-typed parameters, and inference works (`map(list, fun(x: i32): i32 => x * 2)`).
- **Exceptions / coroutines** — closures are ordinary heap objects, cleaned up by the caller's cleanup records; a closure captured in coroutine state lives in the coroutine's state struct.
- **Traits** — closures have no trait dispatch; their type is a signature, not a named type.

## Files

- `compiler/sema/lambda_lifter.cpp` — capture analysis, lifting, env-struct synthesis, self-capture modes
- `compiler/ir/ir_builder_expr.cpp` — `Closure` / `CallIndirect`, function-reference trampolines
- `tests/e2e/test_closures.cpp`
