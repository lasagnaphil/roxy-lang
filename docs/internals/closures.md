# Closures and First-Class Functions

This document describes the design for closures and first-class functions in Roxy.

## Status

Implementation is incremental:

- **Done** — `fun(...) -> R` type syntax, lambda expression parsing, zero-capture
  lambdas end-to-end, `IROp::Closure` and `CALL_INDIRECT` opcode, implicit copy
  capture for copyable variables (primitives, copyable structs, `ref`/`weak`),
  explicit `[move x]` capture for noncopyables (consumes outer with use-after-move
  enforcement), capture-aware destructor codegen for envs holding noncopyable
  captures (via the synthetic-default-destructor path the IR builder auto-emits),
  function references (`var f = double`) — bare named-function-as-value lowers
  to a closure with a synthesized trampoline that forwards to the named function,
  nested closures with implicit copy captures (transitive captures flow through
  each enclosing lambda's env at any depth — `make_adder`-style and full curried
  forms work), **`self` capture inside struct methods** with three modes:
  implicit `ref self` (default; ref-counted; cycle-prone), `[copy self]`
  (struct-value snapshot; copyable structs only), `[weak self]` (weak ref;
  cycle-breaker). For copyable structs, ref/weak self captures emit a runtime
  slab-range check that traps if the receiver is stack-allocated. **`[copy self]`
  and `[weak self]` work in nested lambdas** — the outer chain gets implicit
  ref-self captures so the inner can read self via the enclosing env's `__self`
  field (analogous to multi-boundary identifier capture). On copyable receivers
  the outer's heap check still fires, so nested self-capture in copyable
  structs requires a `uniq` receiver.
- **Deferred (follow-up commits)** — transitive `[move]` captures across nested
  lambdas; cross-module generic-template function references; explicit
  `identity<i32>` value-position syntax (inferred form is supported).

  **Dropped (not implementing)** — `[move self]`. Would require receiver-kind
  annotations on methods (`fun uniq T.method(...)`) plus consumption-tracking
  at the call site. The use cases are niche enough that the language complexity
  isn't worth it; users who need a closure to own a `uniq Self` can refactor
  to a free function that takes `uniq Self` and pass the value explicitly.

The user-facing surface (syntax, capture rules, error messages) below describes
the *full* design; implementation status is annotated where it differs.

## Original State (pre-closures)

Functions were compile-time entities only:
- Registered as symbols with `SymbolKind::Function` during semantic analysis
- Referenced by static index in bytecode (`CALL func_idx`)
- No `Function` variant in the runtime `Value` union
- No way to store, pass, or return functions as values

## Goals

1. **Function types** as first-class types in the type system
2. **Function values** that can be stored in variables, passed as arguments, and returned
3. **Lambda expressions** for inline anonymous functions
4. **Closures** that capture variables from their enclosing scope
5. Preserve Roxy's value semantics and static typing

## Syntax

### Function Type Annotations

Use the `fun` keyword with parenthesized parameter types and `->` for the return type:

```roxy
var callback: fun(i32, i32) -> i32;
var predicate: fun(i32) -> bool;
var action: fun();                     // no params, void return
```

The `->` arrow in function *types* distinguishes them from the `:` used in variable/parameter type annotations and in lambda/function *declarations*. Void-returning function types omit the arrow: `fun(i32)`.

Function types in parameters and return types:

```roxy
fun apply(f: fun(i32) -> i32, x: i32): i32 {
    return f(x);
}

fun make_adder(n: i32): fun(i32) -> i32 {
    return fun(x: i32): i32 { return x + n; };
}
```

### Lambda Expressions

Full form with block body:

```roxy
var add = fun(x: i32, y: i32): i32 { return x + y; };
```

Short form with expression body:

```roxy
var add = fun(x: i32, y: i32): i32 => x + y;
```

Void lambdas:

```roxy
var greet = fun(name: string) { print(f"hello {name}"); };
```

### Calling Function Values

Function values are called with the same syntax as regular functions:

```roxy
var f: fun(i32) -> i32 = fun(x: i32): i32 => x * 2;
var result: i32 = f(21);   // 42
```

### Function References

Named functions can be referenced as values by name without calling them:

```roxy
fun double(x: i32): i32 { return x * 2; }

var f: fun(i32) -> i32 = double;   // function reference, not a call
print(f"{f(5)}");                // 10
```

## Capture Semantics

Roxy uses value semantics by default, so closures capture by value (copy) by default. This is consistent with the language philosophy and avoids the complexity of shared mutable state.

Noncopyable types (`uniq`, `List`, `Map`, structs with noncopyable fields) **cannot be captured implicitly**. They require an explicit capture list with the `move` keyword. This prevents accidental consumption of variables in the outer scope.

### Implicit Capture by Value (Default)

Copyable variables referenced inside a lambda body are automatically captured by value (copy) at closure creation time. Mutations to the original variable do not affect the closure, and vice versa:

```roxy
fun make_counter_broken(): fun() -> i32 {
    var count: i32 = 0;
    // `count` is copied into the closure implicitly
    return fun(): i32 {
        count = count + 1;   // mutates the closure's own copy
        return count;
    };
}
```

This is consistent with Roxy's value semantics: assigning a struct copies it, so capturing a variable copies it too.

### Implicit Capture of `ref` and `weak`

`ref` and `weak` variables are copyable - the reference itself is copied into the closure. The closure and the outer scope share access to the same underlying object:

```roxy
fun make_counter(): fun() -> i32 {
    var state: uniq Counter = uniq Counter { count = 0 };
    var state_ref: ref Counter = state;
    return fun(): i32 {
        // state_ref is captured by value (the ref is copied),
        // but it still points to the same Counter object
        state_ref.count = state_ref.count + 1;
        return state_ref.count;
    };
    // Note: `state` (uniq) must outlive the closure.
}
```

### Explicit Capture List for Noncopyable Types

Noncopyable types (`uniq`, `List<T>`, `Map<K, V>`, noncopyable structs) produce a **compile error** if referenced inside a lambda without an explicit capture list. The capture list appears in square brackets before the parameter list:

```roxy
var items: List<i32> = List<i32>();
items.push(1);
items.push(2);

// ERROR: cannot implicitly capture 'items' of noncopyable type 'List<i32>'
var bad = fun(): i32 { return items.len(); };

// OK: explicit move capture
var good = fun[move items](): i32 { return items.len(); };
// `items` is consumed here - use-after-move error if referenced below
```

**Capture list syntax:**

```roxy
fun[capture1, capture2, ...](params): return_type { body }
```

Each capture entry is one of:
- `move var_name` - move the variable into the closure (ownership transferred)

```roxy
// Move a uniq variable into the closure
fun wrap_resource(r: uniq Resource): fun() -> i32 {
    return fun[move r](): i32 {
        return r.value;
    };
    // `r` is consumed by the closure, cannot use here
}

// Move a List into the closure
fun make_getter(items: List<i32>): fun(i32) -> i32 {
    return fun[move items](i: i32): i32 {
        return items[i];
    };
}
```

**Mixing implicit and explicit captures:**

Copyable variables are still captured implicitly. Only noncopyable variables need to appear in the capture list:

```roxy
var scale: i32 = 10;
var items: List<i32> = List<i32>();

// `scale` captured implicitly (copyable), `items` captured explicitly (noncopyable)
var f = fun[move items](i: i32): i32 {
    return items[i] * scale;
};
```

### Capture Analysis Rules

1. **Primitives and copyable structs**: captured implicitly by value (copy)
2. **`ref` variables**: captured implicitly by value (the ref itself is copied, shares underlying object)
3. **`weak` variables**: captured implicitly by value (the weak ref is copied)
4. **`uniq` variables**: **compile error** unless listed with `move` in capture list
5. **Noncopyable types** (`List<T>`, `Map<K, V>`, structs with noncopyable fields): **compile error** unless listed with `move` in capture list

**Error messages:**

```
error: cannot implicitly capture 'items' of noncopyable type 'List<i32>'
  --> main.roxy:5:20
  |
5 |     var f = fun(): i32 { return items.len(); };
  |                                 ^^^^^
  hint: use 'fun[move items](...)' to move it into the closure
```

## Type System

### FunctionTypeInfo Extension

The existing `FunctionTypeInfo` already stores parameter types and return type. It needs to become usable as a value type:

```cpp
// In types.hpp - already exists:
struct FunctionTypeInfo {
    Span<Type*> param_types;
    Type* return_type;
};
```

Currently, `Type` with `kind == TypeKind::Function` exists but is only used for compile-time type checking of direct calls. To support first-class functions:

- Function types must have a defined **size** (pointer-sized, since closures are heap-allocated)
- Function types must be comparable for assignment compatibility
- Function types must participate in type inference

### Subtyping

Function types are invariant in both parameter and return types (no covariance/contravariance). Two function types are compatible only if parameter types and return type match exactly.

### Size and Representation

A function value at runtime is a **pointer to a closure object** (or a thin wrapper for bare function references). In the register file, it occupies 1 register (8 bytes = pointer width).

## Runtime Representation

A function value is a `uniq` pointer to a heap-allocated **env struct**. It
occupies 2 u32 slots (8 bytes, pointer width) in the register file. The env
struct is synthesized per-lambda by the semantic analyzer; its layout reuses the
existing struct machinery — no new runtime mechanism.

### Env struct layout

```
[ObjectHeader]           16 bytes (weak_generation, ref_count, type_id)
[__call_idx: u32]         4 bytes (index into module's function table)
[capture_0]                       (typed field — captures, declaration order)
[capture_1]
...
[capture_N]
```

The `__call_idx` field is always first (slot offset 0). The runtime reads this
single u32 in `CALL_INDIRECT` to dispatch. Captures occupy subsequent fields
with their natural slot counts — multi-register captures (multi-slot structs,
weak refs) are handled by existing struct-field codegen.

For zero-capture lambdas, the env is just `__call_idx` (1 slot = 4 bytes data,
plus the 16-byte ObjectHeader).

### Per-lambda synthesized types

Each `LambdaExpr` produces (during semantic analysis):

- A struct type `__lambda_<id>_env` registered in the type cache, with field
  layout described above. Slot count = `1 + Σ capture.slot_count`.
- A top-level `FunDecl __lambda_<id>_call(__env: ref __lambda_<id>_env, params...)`
  whose body is the lambda body verbatim. Outer-variable references in the body
  are rewritten to `__env.<field>` (capture lowering, deferred).

The synthesized FunDecl is stashed in `m_synthetic_decls` so the IR builder picks
it up alongside regular module declarations.

### Function type (`fun(...) -> R`)

`TypeKind::Function` represents the user-facing function-type signature. At
codegen, function-typed values flow through the IR as 2-slot pointers — the IR
builder types the result of `IROp::Closure` as `Function<sig>` even though the
underlying SSA value is a `uniq __lambda_<id>_env`. This is the type-erasure
boundary: nominally distinct lambda types collapse to a single value-level
representation.

`Type::noncopyable()` returns true for `TypeKind::Function`, so the existing
move-state tracker, cleanup-record machinery, and slab-allocator typed delete
take care of destruction at scope exit. **No closure-specific runtime code.**

### `Value` variant

Closures reuse the `Ptr` variant (the env's `type_id` in ObjectHeader identifies
its concrete struct type). No new `Value` variant is added.

## Compiler Pipeline Changes

### Phase 1: Parser

**New AST node: `LambdaExpr`**

```cpp
struct CaptureEntry {
    StringView name;          // variable name
    CaptureMode mode;         // Move (only mode for now)
    SourceLocation loc;
};

struct LambdaExpr {
    Span<CaptureEntry> captures;  // explicit capture list (may be empty)
    Span<Param> params;           // lambda parameters
    TypeExpr* return_type;        // optional return type
    Stmt* body;                   // block body or expression body
    SourceLocation loc;
};
```

Add `Lambda` to `ExprKind`.

**New AST node: `FunTypeExpr`**

```cpp
struct FunTypeExpr {
    Span<TypeExpr*> param_types;
    TypeExpr* return_type;     // nullptr for void
};
```

Add `FunType` to `TypeExprKind`.

**Function reference expressions:** When a bare identifier resolves to a function (not followed by `(`), treat it as a function reference expression. This can be handled in semantic analysis rather than parsing - the parser already produces `VariableExpr` for identifiers.

**Parsing rules:**

```
funType      → "fun" "(" typeList? ")" ( "->" type )?
captureList  → "[" captureEntry ( "," captureEntry )* "]"
captureEntry → "move" IDENTIFIER
lambdaExpr   → "fun" captureList? "(" params? ")" ( ":" type )? ( block | "=>" expression )
```

Note the distinction: function *types* use `->` for the return type, while lambda *expressions* use `:` (matching normal function declaration syntax). The optional capture list in `[...]` appears before the parameter list.

The parser disambiguates `fun` as:
- Start of a `FunDecl` at the declaration level
- Start of a `FunTypeExpr` in type position
- Start of a `LambdaExpr` in expression position

### Phase 2: Semantic Analysis

**Capture analysis** is the core new work. When analyzing a lambda body:

1. Process the explicit capture list (if any): validate that each `move` entry names a variable in scope, and that the variable is noncopyable
2. Push a new **capture boundary** scope marker
3. Analyze the lambda body normally
4. When a variable reference crosses the capture boundary (defined in an outer scope), record it as a captured variable
5. For each captured variable, determine the capture mode:
   - If the variable appears in the explicit capture list with `move` → `Move`
   - If the variable is copyable (primitive, copyable struct, `ref`, `weak`) → `Copy` (implicit)
   - If the variable is noncopyable and **not** in the capture list → **compile error**
6. After analysis, produce a **resolved capture list**: ordered list of (variable_name, type, capture_mode)

```cpp
enum class CaptureMode { Copy, Move };

struct CaptureInfo {
    StringView name;
    Type* type;
    CaptureMode mode;
    u32 outer_scope_depth;   // distance to the defining scope
};
```

**Capture mode determination:**

| Variable type | In capture list? | Result |
|---------------|-------------------|--------|
| Copyable (primitives, copyable structs) | No (implicit) | `Copy` |
| `ref T` | No (implicit) | `Copy` (ref itself is copied) |
| `weak T` | No (implicit) | `Copy` (weak ref is copied) |
| `uniq T` | `move var` | `Move` (original consumed) |
| `List<T>`, `Map<K,V>` | `move var` | `Move` (original consumed) |
| Noncopyable struct | `move var` | `Move` (original consumed) |
| Any noncopyable | Not listed | **Compile error** |

**Lambda lifting:** Each lambda is lifted to a top-level function with a mangled name (e.g., `__lambda_0`, `__lambda_1`). Captured variables become extra parameters prepended to the parameter list. The semantic analyzer rewrites the lambda's body to reference these parameters instead of the outer variables.

```roxy
// Source (implicit capture of copyable `n`):
fun make_adder(n: i32): fun(i32) -> i32 {
    return fun(x: i32): i32 => x + n;
}

// After lifting:
fun __lambda_0(__capture_n: i32, x: i32): i32 {
    return x + __capture_n;
}
fun make_adder(n: i32): fun(i32) -> i32 {
    return __create_closure(__lambda_0, n);   // capture n by value
}
```

```roxy
// Source (explicit move capture of noncopyable `items`):
fun make_getter(items: List<i32>): fun(i32) -> i32 {
    return fun[move items](i: i32): i32 {
        return items[i];
    };
}

// After lifting:
fun __lambda_1(__capture_items: List<i32>, i: i32): i32 {
    return __capture_items[i];
}
fun make_getter(items: List<i32>): fun(i32) -> i32 {
    return __create_closure(__lambda_1, items);   // move items into closure
}
```

**Type checking for function values:**
- Assignment: check that source function type matches target function type
- Call: check argument types and return type against the function type
- Function reference: check that the named function's signature matches the target type

### Phase 3: IR Builder

**New IR operations:**

```
Closure(env_struct_name, call_function_name, capture_0, capture_1, ...) -> env_ptr
    High-level construction op. Lowering expands to NEW_OBJ + SetField writes:
    one for `__call_idx` (resolved to the call function's bytecode index at
    lowering time) and one per capture (using the env struct's field layout).
    Result is the env pointer typed as Function<sig>.

CallIndirect(closure_value, arg_0, arg_1, ...) -> result
    Indirect dispatch through a Function<sig>-typed value. The runtime reads
    __call_idx from the env's first u32 field, places the env pointer at the
    callee's first register (the lifted function's hidden __env parameter),
    and copies explicit args after.
```

**IR generation:**

- `gen_lambda_expr` emits a single `IROp::Closure` referencing the synthesized
  env struct and call function names. The captures span is empty until capture
  lowering lands.
- `gen_call_expr`, when the callee is a local (variable or parameter) of
  `is_function()` type, emits `IROp::CallIndirect` instead of the normal `Call`.
  Direct calls to named functions continue to use `IROp::Call`.

### Phase 4: Lowering (IR to Bytecode)

**New opcodes:**

```
CALL_INDIRECT = 0xDD
    Format (two-word):
      word 1: [CALL_INDIRECT:8][dst:8][closure_reg:8][arg_count:8]
      word 2: [reserved:32]   (future inline-cache slot)

    Reads __call_idx from the env at regs[closure_reg].
    Sets up the callee frame with env_ptr as the first (hidden) argument,
    copies arg_count explicit arguments after, and dispatches.
```

`IROp::Closure` does not get its own opcode. Lowering expands it into:
1. `NEW_OBJ dst, env_type_idx` — allocate the env (registers the env struct in
   the module's type table on first use).
2. `LOAD_INT idx_reg, call_idx` (or `LOAD_CONST` for indices > 32K) — materialize
   the call function's bytecode index.
3. `SET_FIELD dst, idx_reg, slot_count=1` at slot_offset 0 — store `__call_idx`.
4. For each capture: load into a register, then `SET_FIELD dst, cap_reg,
   slot_count=field.slot_count` at the field's slot_offset.

Composition of existing ops, no new lowering primitives.

### Phase 5: Interpreter

**CALL_INDIRECT handler:**
1. Read `dst`, `closure_reg`, `arg_count` from word 1; skip the reserved word.
2. Load `env_ptr = regs[closure_reg]` (null-check; error on null).
3. Read `func_idx = *(u32*)env_ptr` (the env's first field).
4. Resolve `BCFunction* callee = vm->function_ptrs[func_idx]`.
5. Allocate callee_regs from the register file; place env_ptr at `callee_regs[0]`
   and copy `param_register_count - 1` explicit args from `regs[first_arg]`.
6. Push frame; dispatch.

**Cleanup:** Closure values are noncopyable `uniq` pointers, so the standard
cleanup-record machinery emits a typed `DELETE` at scope exit. The slab
allocator dispatches via the env's `type_id`. With captures, the env struct
gets a synthetic default destructor (per the existing
`Type::noncopyable()`-triggered codegen path) that runs each capture's
destructor in reverse-LIFO order. **No closure-specific cleanup opcodes.**

**Default ownership:** function values are `uniq`-flavored — the closure is
owned by exactly one variable; passing to a function moves it; to share, use
`ref fun(...)`. This mirrors `Coro<T>`.

## Detailed Example

```roxy
fun apply_twice(f: fun(i32) -> i32, x: i32): i32 {
    return f(f(x));
}

fun make_multiplier(factor: i32): fun(i32) -> i32 {
    return fun(x: i32): i32 => x * factor;
}

fun main(): i32 {
    var double: fun(i32) -> i32 = make_multiplier(2);
    var result: i32 = apply_twice(double, 3);
    print(f"{result}");   // 12
    return 0;
}
```

**After lambda lifting:**

```roxy
// Lifted lambda: captures `factor` as first parameter
fun __lambda_0(__capture_factor: i32, x: i32): i32 {
    return x * __capture_factor;
}

fun apply_twice(f: fun(i32) -> i32, x: i32): i32 {
    // f is a closure pointer; calls via CALL_CLOSURE
    return f(f(x));
}

fun make_multiplier(factor: i32): fun(i32) -> i32 {
    // CREATE_CLOSURE: __lambda_0, captures=[factor]
    return __create_closure(__lambda_0, factor);
}

fun main(): i32 {
    var double: fun(i32) -> i32 = make_multiplier(2);
    var result: i32 = apply_twice(double, 3);
    print(f"{result}");
    return 0;
}
```

**Bytecode for `make_multiplier`:**
```
r0 = factor (parameter)
CREATE_CLOSURE r1, __lambda_0_idx, 1   // 1 capture
    capture[0] = r0                     // capture `factor`
RET r1                                  // return closure pointer
```

**Bytecode for `apply_twice`:**
```
r0 = f (parameter, closure pointer)
r1 = x (parameter)
CALL_CLOSURE r2, r0, 1     // r2 = f(x), 1 explicit arg starting after r2
    arg[0] = r1
CALL_CLOSURE r3, r0, 1     // r3 = f(r2), 1 explicit arg
    arg[0] = r2
RET r3
```

## Interaction with Existing Features

### Move Semantics

Closures that capture `uniq` variables must use an explicit capture list with `move`:

```roxy
fun take_ownership(r: uniq Resource): fun() -> i32 {
    return fun[move r](): i32 { return r.value; };
    // `r` is moved into closure, cannot use after this point
}
```

The semantic analyzer's existing move-state tracking handles this naturally - a `move` capture is equivalent to passing the variable to a function parameter.

### Generics

Generic functions can accept function-typed parameters:

```roxy
fun map<T, U>(list: ref List<T>, f: fun(T) -> U): List<U> {
    var result: List<U> = List<U>();
    for (var i: i32 = 0; i < list.len(); i = i + 1) {
        result.push(f(list[i]));
    }
    return result;
}
```

Type inference works naturally: `map(my_list, fun(x: i32): i32 => x * 2)` infers `T=i32, U=i32` (the lambda uses `:` for its return type, matching function declaration syntax).

### Traits

Closures do not automatically implement traits. A closure's type is its function signature, not a named type. Trait method dispatch does not apply to closures.

### Exception Handling

If an exception is thrown during a closure call, normal stack unwinding occurs. The closure object itself is cleaned up by the caller's cleanup records (since it's a heap object owned by a register).

### Coroutines

Closures can be used inside coroutines. If a closure is captured in a coroutine's state, it is stored in the coroutine's heap-allocated state struct.

## Implementation Plan

### Done

- **Function-type syntax + AST**: `TypeExprKind::Named|Function`, `LambdaExpr`,
  `CaptureEntry`, `CaptureInfo`. Parser handles `fun(T) -> R` in type position
  and `fun[...](params): R { ... }` / `=> expr` in expression position.
- **Type system**: `TypeKind::Function` is noncopyable, slot count 2, formatted
  as `fun(T) -> R` (with `->` for non-void return) in error messages.
- **Capture detection**: `ScopeKind::Lambda` boundary scope and
  `Symbol::defining_scope` back-pointer; `analyze_identifier_expr` walks scopes,
  detects boundary crossings, records into the active `LambdaCaptureContext`
  (deduped by `Symbol*`), and rewrites the `IdentifierExpr` in-place to
  `ExprGet(IdentifierExpr("__env"), name)` so the lifted body reads from the
  env field.
- **Implicit copy capture** for copyable variables (primitives, copyable structs,
  `ref` / `weak`) — discovered lazily on first body reference; captured by value.
- **Explicit `[move x]` capture** for noncopyables (`uniq`, structs with default
  destructors, etc.) — pre-populated at the lambda's expression site, marks the
  outer symbol moved via `mark_moved` so subsequent references fail with
  use-after-move. `[move]` on a copyable type is a clear compile error.
- **Synthesis**: `analyze_lambda_expr` registers the env struct type early
  (placeholder layout), pushes a `LambdaCaptureContext`, runs body analysis,
  then backfills the env's fields with `[__call_idx, captures...]` in
  declaration order. If any capture is noncopyable, a `DestructorInfo{name=empty,
  decl=nullptr}` is attached so the IR builder auto-emits a default destructor
  via `build_synthesized_default_destructor` (cleans up captured noncopyables in
  reverse-LIFO order).
- **IR**: `IROp::Closure` populated with capture `ValueId`s (sourced via the IR
  builder's local-scope map at the lambda's expression site). `IROp::CallIndirect`
  dispatches through a Function-typed value.
- **Bytecode**: `CALL_INDIRECT = 0xDD`, two-word format. `IROp::Closure` lowers
  to `NEW_OBJ` + `SET_FIELD` for `__call_idx` and one `SET_FIELD` per capture
  using the env-struct field layout.
- **Interpreter**: handler reads `__call_idx` from the env's first u32 field,
  places the env pointer at `callee_regs[0]`, copies explicit args.
- **Function references** (`var f = double`, `var p = print`,
  `var f: fun(i32)->i32 = identity`): `gen_identifier_expr` falls through to
  `gen_function_ref` when a bare identifier resolves to a top-level function
  symbol. The function builds a `FunctionRefTarget` descriptor that captures
  the call kind, then `gen_function_ref` synthesizes a per-target trampoline
  IRFunction `(__env: ref EnvT, args...)` whose single body instruction is
  selected from the descriptor:

  | Source kind                    | Symbol                                        | Body op                  |
  |--------------------------------|-----------------------------------------------|--------------------------|
  | Script function                | `Function`, `is_native=false`                 | `IROp::Call`             |
  | Native function                | `Function`, `is_native=true`                  | `IROp::CallNative`       |
  | Imported native                | `ImportedFunction`, `is_native=true`          | `IROp::CallNative`       |
  | Imported script (cross-module) | `ImportedFunction`, `is_native=false`         | `IROp::CallExternal`     |
  | Generic instantiation          | template name + monomorphized name on the AST | `IROp::Call`             |

  Trampolines are cached on `m_function_refs` keyed by the unique target name
  (the mangled IRFunction name for scripts, `module::name` for cross-module
  imports, the registry name for natives) so multiple references to the same
  target share one trampoline. The result is a normal `IROp::Closure` with
  empty captures — calls go through the same `CALL_INDIRECT` path as
  ordinary closures.

  **Generic templates** require monomorphization at the reference site. Since
  there's no syntax for `identity<i32>` as a value (yet), inference relies on
  surrounding type context: `analyze_identifier_expr` marks the identifier
  with `is_generic_template_ref = true` and returns `error_type` as a
  sentinel. A `coerce_generic_template_ref` helper fires at every assignment
  site (var init with annotation, call-arg passing, return statement, struct
  literal field) — it unifies the template's signature against the expected
  function type via the existing `unify_type_expr` (Function-kind branch),
  instantiates via `GenericInstantiator::instantiate_fun`, and stashes the
  monomorphized name on the `IdentifierExpr`. The IR builder reads that
  stashed name and routes through `gen_function_ref` with a Script-kind
  descriptor whose target name is the (mangled) monomorphized function.
- **Nested closures** with implicit copy captures: `analyze_identifier_expr`
  walks every `ScopeKind::Lambda` boundary between the use site and the
  symbol's defining scope, recording the capture into each enclosing context
  (deduped by `Symbol*`). `CaptureInfo::source_expr` is set per-context: the
  outermost crossed lambda reads the value directly via `IdentifierExpr(name)`;
  every inner one reads via `ExprGet(IdentifierExpr("__env"), name)` against the
  enclosing context's env (typed using `LambdaCaptureContext::env_struct_type`).
  At IR-build time, `gen_lambda_expr` evaluates `cap.source_expr` in the
  enclosing function's IR scope, which resolves correctly because each lambda
  is constructed inside a function whose `__env` parameter is in scope. A
  general indirect-call path in `gen_call_expr` handles chained calls like
  `make()()` (callees that aren't bare identifiers but resolve to Function).
- **`self` capture** inside struct methods. `analyze_this_expr` detects when an
  `ExprThis` reference crosses a `ScopeKind::Lambda` boundary before reaching
  the enclosing struct scope and routes through one of three modes:
  - **Implicit ref-self** (default): captures `ref Self`. Ref-counted; safe for
    heap receivers; cycle-prone. For *copyable* structs whose receiver could be
    stack-allocated, the IR builder emits an `IROp::AssertHeap` instruction
    (lowered to `ASSERT_HEAP = 0xDE`, which calls `vm->allocator->owns(ptr)`)
    before storing the ref into the env. The trap message points the user at
    `[copy self]`.
  - **`[copy self]`**: synthesizes a struct literal `Self { f0 = <self_ref>.f0, ... }`
    as the source expression; the env field stores a value snapshot. The
    `<self_ref>` is `ExprThis` for lambdas directly in a method, or
    `ExprGet(__env, __self)` (reading from the enclosing lambda's env) for
    nested cases. Required static checks: copyable struct, no when-clauses.
  - **`[weak self]`**: source is `ExprThis` or `ExprGet(__env, __self)`
    (`ref Self`); the env field is `weak Self` and the env-store auto-wraps
    via `maybe_wrap_weak`. For copyable structs, an `AssertHeap` runtime check
    fires when the source is `ExprThis` (since the receiver may be stack-
    allocated). In nested cases the source is the outer's `__env.__self`,
    which already passed the outer's check, so no additional check is needed.
- **`IROp::Closure` lowering** handles value-struct captures via `STRUCT_COPY`
  (memory-to-memory) — the source SSA value is a pointer to the struct, and
  the env field is value-typed with multi-slot layout. Primitive / ref / weak
  captures continue to use `SET_FIELD` (the source value packs into one or
  two registers).
- **Multi-boundary self captures** (`ensure_self_captured_through`): when a
  nested lambda explicitly takes `[copy self]` / `[weak self]`, or when an
  inner body implicitly references `self` across multiple Lambda boundaries,
  every enclosing lambda context gets an implicit ref-self capture
  (recursively populated). The outermost reads `self` directly via
  `ExprThis`; inner ones read from the immediately-enclosing context's
  `__env.__self`. The body rewrite produces `ExprGet(__env, __self)` against
  the innermost lambda's env, whose field type drives the result type.
- **Tests**: `tests/e2e/test_closures.cpp` covers lambda creation, multi-arg,
  block body, void return, higher-order (pass and return), implicit i32 / f64 /
  multi-capture / dedup'd capture / closure-from-parameter, `[move]` of
  `uniq T` with use-after-move enforcement, capture rule errors (implicit
  noncopyable, `[move]` on copyable, `[move]` of unknown variable), function
  references in typed and inferred bindings (with cache-dedup and void return),
  nested closures (`make_adder`, fully curried two-level, three-level transitive
  capture, capture from outer-body local), self capture in all three modes
  (implicit ref-self for noncopyable; copyable + uniq receiver heap-check pass;
  copyable + stack receiver runtime trap; `[copy self]` snapshot semantics;
  `[weak self]` for noncopyable and copyable+uniq cases; runtime trap for
  copyable+stack `[weak self]`), nested self capture (nested `[copy self]`
  on copyable+uniq; nested `[weak self]` on noncopyable; nested implicit
  ref-self propagation; nested `[copy self]` on copyable+stack runtime trap),
  and the remaining deferred-error paths.

### Dropped (not implementing)

- **`[move self]`** — would require receiver-kind annotations on methods
  (`fun uniq T.method(...)`) plus consumption-tracking at the call site. The
  use cases are niche enough that the language complexity isn't worth it;
  users who need a closure to own a `uniq Self` can refactor to a free
  function that takes `uniq Self` and pass the value explicitly.

### Deferred (follow-up commits)

- **`fun uniq T.method(...)` receiver-kind annotation** — would let the body
  statically know the receiver is heap-only, eliding the runtime
  `AssertHeap` check for copyable structs (and unblocking nested self capture
  on copyable + stack-allocated receivers, which traps today).
- **Transitive `[move]` captures** across nested lambdas — would force every
  enclosing lambda to also move, leaving them unable to use the variable.
  `analyze_lambda_expr` rejects this with a clear error today.
- **Cross-module generic-template references** — referencing a generic
  template imported from another module (`from mod import identity` then
  `var f: fun(i32) -> i32 = identity`) doesn't yet route through the
  monomorphization machinery, since the import only exposes the resolved
  symbol. Within-module generic refs work via context-driven inference.
- **Explicit `identity<i32>` value-position syntax** — today only the
  inferred form is wired (the parser commits `<types>` only when followed
  by `(`, `{`, or `.`). The inferred form covers var-init annotations,
  call-arg passing, return position, and struct-field initializers, which
  cover the realistic uses.
- **Generics + exception unwinding through closure boundaries** — should fall
  out naturally from the existing struct/method machinery; needs explicit test
  coverage when those scenarios become relevant.