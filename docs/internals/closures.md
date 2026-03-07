# Closures and First-Class Functions

This document describes the design for adding closures and first-class functions to Roxy.

## Current State

Functions are compile-time entities only:
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

### Closure Object

A closure is a heap-allocated object with the following layout:

```
[ObjectHeader]           16 bytes (weak_generation, ref_count, type_id)
[func_idx: u32]           4 bytes (index into module's function table)
[capture_count: u16]      2 bytes
[padding: u16]            2 bytes
[capture_0: u64]          8 bytes (first captured value)
[capture_1: u64]          8 bytes
...
[capture_N: u64]          8 bytes
```

Each captured value occupies one u64 slot (matching register width). Captured structs that span multiple registers require multiple capture slots.

### Bare Function References

When a named function is referenced without captures, we can optimize by creating a **bare closure** - a closure object with `capture_count = 0`. This avoids special-casing in the call path.

Alternatively, a bare function reference could be a tagged pointer (e.g., func_idx stored directly in the register with a sentinel bit), but this adds complexity. The simpler approach is to always allocate a closure object, even for bare references. The overhead is small (24 bytes for a zero-capture closure) and the uniform representation simplifies the VM.

### Value Type Extension

Add a new variant to the runtime `Value` enum:

```cpp
struct Value {
    enum Type : u8 {
        Null, Bool, Int, Float, Ptr, Weak,
        Closure,   // NEW: pointer to closure object
    };
    // ...
};
```

Or, since closures are pointer-based, reuse `Ptr` with the `type_id` in the ObjectHeader distinguishing closure objects from other heap objects. This avoids adding a new Value variant and is consistent with how other heap objects (strings, lists, maps) are represented.

**Decision: Reuse `Ptr`.** The `type_id` in ObjectHeader identifies the object as a closure. The VM checks `type_id` when performing an indirect call.

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
CreateClosure(func_name, capture_0, capture_1, ...) -> closure_ptr
    Allocates a closure object, stores func_idx and captured values.

CallIndirect(closure_ptr, arg_0, arg_1, ...) -> result
    Loads func_idx from closure object, loads captured values,
    calls the lifted function with captures prepended to arguments.
```

**Function reference:** `CreateClosure(func_name)` with zero captures.

**IR generation for lambda expressions:**
1. Emit the lifted function as a separate `IRFunction`
2. At the lambda expression site, emit `CreateClosure` with the lifted function name and capture values

### Phase 4: Lowering (IR to Bytecode)

**New opcodes:**

```
CREATE_CLOSURE = 0xA2
    Format: [CREATE_CLOSURE:8][dst:8][func_idx:8][capture_count:8]
    Followed by: capture_count bytes, each specifying a source register

    Allocates a closure object on the heap.
    Stores func_idx and copies register values into capture slots.
    Result: pointer to closure object in dst.

CALL_CLOSURE = 0xA3
    Format: [CALL_CLOSURE:8][dst:8][closure_reg:8][arg_count:8]

    Reads func_idx from closure object.
    Loads captured values from closure into callee's initial registers.
    Copies explicit arguments after captured values.
    Sets up call frame and dispatches.
```

**Alternative: Unified CALL.** Instead of a separate `CALL_CLOSURE`, we could make `CALL` polymorphic - check at runtime whether the operand is a static func_idx or a closure pointer. But this adds a branch to every function call. A dedicated opcode is cleaner and keeps the fast path (static calls) fast.

### Phase 5: Interpreter

**CREATE_CLOSURE handler:**
1. Compute closure object size: `sizeof(ObjectHeader) + 8 + capture_count * 8`
2. Call `object_alloc(vm, closure_type_id, data_size)`
3. Write `func_idx` (u32) and `capture_count` (u16) to the object data
4. Copy each capture register value into the capture slots
5. Store the resulting pointer in `regs[dst]`

**CALL_CLOSURE handler:**
1. Read closure pointer from `regs[closure_reg]`
2. Load `func_idx` and `capture_count` from closure object
3. Look up `BCFunction` via `module->functions[func_idx]`
4. Allocate register window for callee
5. Copy captured values into callee's first `capture_count` registers
6. Copy explicit arguments into subsequent registers
7. Push call frame and dispatch (same as regular CALL from here)

**Cleanup:** Closure objects are heap-allocated with `ObjectHeader`. They follow the same lifecycle as other heap objects:
- `uniq fun(...)` closures: deleted at scope exit
- `ref fun(...)` closures: ref-counted
- Passing a closure by value: copies the pointer (shared reference)

**Decision on default ownership:** A closure value behaves like a `ref`-counted pointer by default. When a closure is created, it has ref_count = 0 (owned by the creating scope, like `uniq`). When passed to a function parameter of type `fun(...)`, the ref is incremented. This matches how other heap objects work in Roxy.

Actually, to keep things simple and consistent: **closure values are `uniq` by default**, just like `uniq` struct instances. A `fun(...)` typed variable owns the closure. Passing it to another function moves it. To share, use `ref fun(...)`.

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

### Step 1: Function Types in Type System
- Make `TypeKind::Function` types usable as value types
- Define size (pointer-width) and register count (1) for function types
- Add `FunTypeExpr` to the AST and parser

### Step 2: Lambda Parsing
- Add `LambdaExpr` to AST (with `Span<CaptureEntry> captures`)
- Parse optional capture list: `fun[move x, move y](params): type { body }`
- Parse `fun(params): type { body }` and `fun(params): type => expr` in expression position
- Disambiguate from `FunDecl` (expression context vs declaration context)

### Step 3: Capture Analysis and Lambda Lifting
- Add capture boundary tracking to semantic analyzer's scope stack
- Detect cross-boundary variable references and build capture lists
- For copyable types: implicit capture by value
- For noncopyable types: require explicit `move` in capture list, emit compile error otherwise
- Lift lambdas to top-level functions with captures as extra parameters
- Apply move-state tracking for `move`-captured variables

### Step 4: Function References
- Allow bare function names in expression position to produce closure values
- Create zero-capture closures for named function references

### Step 5: IR Operations
- Add `IROp::CreateClosure` and `IROp::CallIndirect`
- Generate lifted functions as `IRFunction` entries
- Emit closure creation and indirect calls in the IR builder

### Step 6: Bytecode and Lowering
- Add `CREATE_CLOSURE` and `CALL_CLOSURE` opcodes
- Lower `CreateClosure` and `CallIndirect` IR ops to bytecode
- Handle closure cleanup records for exception safety

### Step 7: Interpreter
- Register closure object type in the type registry
- Implement `CREATE_CLOSURE` handler (allocate, store captures)
- Implement `CALL_CLOSURE` handler (load captures, dispatch)
- Add cleanup support for closure objects

### Step 8: Testing
- Basic lambda creation and calling
- Closures capturing copyable variables (implicit value capture)
- Closures capturing `uniq` variables (`move` capture)
- Closures capturing `List`/`Map` (`move` capture)
- Compile error when capturing noncopyable type without `move`
- Higher-order functions (passing/returning closures)
- Function references to named functions
- Closures with generics
- Closures inside loops (each iteration creates a fresh capture)
- Mixed captures (implicit copyable + explicit `move` noncopyable)
- Exception handling across closure boundaries
- Nested closures (closure capturing a closure)