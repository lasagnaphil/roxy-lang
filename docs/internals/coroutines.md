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

## Language Rules

- A function is a coroutine iff its return type is `Coro<T>` **and its body contains a `yield`** (`FunDecl::is_coroutine`). A non-yielding `Coro<T>`-returning function is ordinary: it produces/forwards a coroutine value and may `return` one.
- The yielded expression must be assignable to `T`. Inside a coroutine only bare `return;` is allowed (it ends the coroutine early).
- `yield` inside `finally` is an error — `finally` runs in multiple contexts (normal and exception exit), making state management infeasible.
- `Coro<T>` is built in, takes exactly one type argument, and cannot be user-defined.
- Calling a coroutine function returns a `Coro<T>` without executing the body; `.resume()` runs to the next `yield` and returns its value; `.done()` is `true` once execution has passed the last yield point.
- Yields may appear in straight-line code, `if`/`else`, loops (including nested, with `break`/`continue`), `when`, and `try`/`catch` blocks.

## IR Generation

`IROp::Yield` is a block terminator whose operand is the yielded value. Right after it, the builder captures the live locals using ordinary SSA block arguments: it creates a `coro.resume` block with one parameter per live local, jumps to it passing the current values, and rebinds the locals to its parameters.

## Coroutine Lowering Pass

`coroutine_lower()` runs after IR generation and before either backend (and before the optimizer), transforming each coroutine into three ordinary functions: init (the original name), `__coro_<func>$$resume`, and `__coro_<func>$$delete`. A `Yield` surviving lowering is rejected by the IR validator.

It derives the **promoted variables** — those that must survive across yields — from the resume blocks' parameters, and synthesizes a state struct `__coro_<func>`:

| Field | Type | Slot | Purpose |
|-------|------|------|---------|
| `__resume_idx` | `u32` | 0 | Resume function's dispatch index |
| `__state` | `i32` | 1 | Current state-machine state |
| `__yield_val` | `T` | 2 | Cached yield value |
| `<param>` | varies | 3… | One field per function parameter |
| `<local>` | varies | … | One field per promoted local |

The first two fields are a runtime layout contract: `__resume_idx` at slot 0 so `CALL_INDIRECT` dispatches `resume()` on an erased value exactly like a closure env's `__call_idx`, and `__state` at a fixed slot so `done()` inlines a load + compare.

**Init** allocates the struct, seeds `__resume_idx` with `IROp::FuncIndex("__coro_<func>$$resume")` (a `LOAD_INT` of the function index in bytecode / a `g_closure_fns[]` slot in C), sets `__state = 0`, and copies each parameter into its field.

**Resume** is built by transforming the function in place, in two phases (so no block cloning or cross-function value remapping is needed):

- *Promote variables.* Replace the params with a single `self`. In every block, prepend `GetField` loads for all promoted vars and remap uses to them; on each jump edge carrying promoted arguments, write them back with `SetField`; then drop the promoted block params/args. Loads are per block (not one global remap) because the IR builder rebinds local names to resume-block params after a yield, so later blocks not dominated by the resume block would otherwise see non-dominating references. A catch block's exception parameter is never promoted.
- *Split at yields, add dispatch.* Each `Yield` becomes: store `__yield_val`, store the next state, return the value. Each `Return` sets `__state = CORO_STATE_DONE` and returns a default (never observed). An entry dispatch chain branches on `__state` to the original entry (state 0) or the resume blocks (1..N), trapping on invalid states. Exception-handler BlockIds are remapped alongside terminator targets.

**No done function.** `done()` is inlined as `__state == CORO_STATE_DONE` (`0x7FFFFFFF`, a positive sentinel above all yield-point states), uniform across known and erased values.

For the C backend's handful of coroutine-specific emitter rules, see [c-backend.md → Coroutines](c-backend.md#coroutines).

## First-Class Coroutine Values

A `Coro<T>` can be assigned to an annotated variable, passed, returned, and stored — even when the concrete coroutine function is unknown at the use site (an "erased" `Coro<T>`):

```roxy
fun ones(): Coro<i32> { yield 1; yield 1; }
fun twos(): Coro<i32> { yield 2; yield 2; }

fun sum_two(c: Coro<i32>): i32 {        // c is erased — could be either coroutine
    return c.resume() + c.resume();
}
```

- **Two representations unify.** An annotated `Coro<T>` resolves to the interned generic type (empty `func_name`); a coroutine value carries its per-function type (`coroutine_type_for_func`). Two coroutine types are assignable when their **yield types** match — `func_name` is irrelevant to dispatch.
- **Erased deletion.** An owned erased `Coro<T>` drops via `DropKind::Closure`: the VM dispatches the state-struct destructor by runtime `type_id`, the C backend by `__resume_idx`. Both run `__coro_<func>$$delete`, then free.

### In containers

`List<Coro<T>>` and `Map<K, Coro<T>>` work on both backends and own their
coroutines. `Coro<T>` is noncopyable, so it behaves like any move-only element:
indexing **borrows** it (`l[0].resume()` advances the stored coroutine in place),
moving it out is rejected, and `pop()` takes ownership. `.copy()` and printing of
such a container are rejected — `Coro<T>` implements neither Copy nor `Printable`.

`mangle_type_name` names the instantiation `Coro$<yield>`, keyed on the **yield
type** rather than `func_name`: two `Coro<i32>`s from different functions are the
same type — that is what makes an erased `Coro<T>` assignable — and keying on
`func_name` would mint a separate container instantiation per producing function.

## Coroutine Methods

A struct method can be a coroutine — `fun S.count(): Coro<i32> { ... yield ...; }`. A method's `self` is a real first parameter (`ref S`), and lowering captures every parameter into the state struct, so `self` is captured and ref-counted for the coroutine's lifetime exactly like any `ref` parameter.

- Classification mirrors free functions (`MethodDecl::is_coroutine`). The state struct / functions are named from the mangled method name: `__coro_S$$count`, `__coro_S$$count$$resume`, `__coro_S$$count$$delete`.
- `self` is always `ref self` — the receiver must outlive the coroutine (deleting it earlier traps, like any borrowed owner). Value/weak `self` capture is not supported.
- **The receiver must be heap-allocated.** Borrowing a *stack* receiver would leave a pointer into a dead frame (and its counted borrow would write through `data - 8` into a neighbouring local). Init emits `IROp::AssertHeap` on the receiver, trapping with the same message closures use. Call such methods on a `uniq` receiver.
- **Not yet supported:** coroutine methods on generic structs or in traits — rejected with a clear error. Tracked in TODO.md.

## Parameters and promoted locals in the state struct

| Shape | Storage | Access |
|---|---|---|
| Scalars, `uniq`/`ref`/`weak`, containers, `Coro<T>` | Field holds the value | `GetField` at block entry, `SetField` write-back at each jump |
| **Value structs** | Field holds the struct **inline**, sized by `get_type_slot_count` | `GetFieldAddr` at block entry; the body mutates the field in place, and the jump write-back is a `StructCopy` |
| `out` / `inout` params | — | **Rejected at compile time** |

A value struct's SSA value is an *address*, so storing it by value would put a
pointer into a field sized for the struct. Reading the field's address instead
makes the state the variable's storage. Once the source is already the field's
own address, the write-back copy is skipped rather than emitted as a self-copy — it
would otherwise be a per-resume `memcpy` no pass can remove (`StructCopy` has side
effects; `GetFieldAddr` is not CSE-eligible).

`out`/`inout` parameters are second-class ([lifetimes.md](lifetimes.md), "The
second-class family"): the state outlives the call, so capturing one would leave a
pointer into a dead frame. Pass by value, or pass a `uniq`/`ref`.

### Which values a promoted variable owns

Promotion keys a variable on **(name, type)**. Two locals may share a name in
disjoint scopes (only shadowing is banned); keying on name alone gave both one
field sized for whichever was reached first, so a one-slot `x` and a four-slot
struct `x` wrote through the same field. Same-named locals at the *same* type
still share a field — disjoint scopes are never live at once. A later variable
under a taken name gets a reserved `__pv<n>_<name>` field.

Promotion collects values from block parameters, and **being a block parameter is
a precondition the IR builder establishes**, not something the pass infers. The
resume dispatch adds an edge that re-enters a loop, so a local defined before the
loop and merely read inside it stops dominating the body — a null dereference on
the second `resume()`. So a coroutine loop whose body can suspend
(`stmt_contains_yield`, recursive: a yield in an inner loop widens the outer loop
too) threads *every* live local through its header (`collect_live_locals`).

This is fixed at the loop header rather than inside promotion because variable
identity still exists there and does not survive into the pass: SSA erases it
(`var cur: i32 = i;` emits no instruction, so `cur` and `i` share a ValueId), and a
never-reassigned local has no parameter near its definition. Reconstructing
identity from jump arguments needs several fragile workarounds; threading needs
none. The cost inside a suspending loop is only a longer live range for locals the
loop never modifies; loops without a yield are untouched, and the state struct is
unaffected (promoted variables come from the *yield's* live set).

### Cleanup, and who owns it on which path

Two different things can free a promoted field, and exactly one must:

- **Running to completion** — the body's own scope-exit cleanup already dropped
  every local and parameter, so the return path *clears* the owning fields before
  setting the done state (a pointer field is nulled; an inline value struct's
  owning members are cleared through its address). Owned fields inside a `when`
  clause are not reached — variant cleanup is discriminant-guarded.
- **Dropping the `Coro` early** — the generated `$$delete` destroys whatever is
  still live, per `member_needs_drop` — the same shared predicate the
  synthetic-destructor pass and `IRBuilder::emit_field_cleanup` use. Don't
  hand-enumerate kinds here; that is how inline value structs once leaked.

A `ref` *parameter* is the exception: its count is taken once at creation and
released by `$$delete` on *every* path, so the completion path does not clear it.

Two field kinds are released on the resume path *and* by `$$delete`; both clear
the field right after the resume path's release, so `$$delete`'s null guard reads
"already released":

- a `ref` **local** (`RefDec` → `SetField(null)`);
- a **catch param** (`Delete` → `SetField(null)`).

The catch param is the one field whose cleanup is *not* decided by its type: the
field is `ref E` (or `ExceptionRef` for a catch-all), but the binding owns the
caught exception. `$$delete` therefore treats the names in `catch_names` as owning
— a typed catch frees as `uniq E` so `fun delete E` runs, a catch-all frees
type-erased. It matches by *field* name, which keeps it distinct from an unrelated
local sharing the source name.

## Files

- `compiler/ir/coroutine_lowering.cpp` — the state-machine transformation
- `compiler/ir/ir_builder_stmt.cpp` — `gen_yield_stmt`, live-local capture, loop-header threading
- `compiler/ir/ir_builder_expr.cpp` — `resume()` → `CallIndirect`, inline `done()`
- `compiler/sema/semantic.cpp` — coroutine classification and yield/return validation
- `tests/e2e/test_coroutines.cpp`
