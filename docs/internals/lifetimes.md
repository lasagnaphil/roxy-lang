# Memory, Lifetimes & Lifecycle

The single reference for Roxy's no-GC memory model: how heap objects are allocated
and freed, how `uniq` / `ref` / `weak` stay sound, and how values are dropped,
copied, and moved.

**In two sentences:** `ref` is a *constraint reference* — a borrow of a **heap**
object that increments a count in the object's header while it lives, and an object
cannot be freed, by any path, while that count is nonzero (the free traps); **stack**
value-structs are never borrowed with `ref` — they pass by reference only through the
second-class `out` / `inout` / `self` family, which flows downward and cannot escape;
and `weak` is the sole user of generational references. Orthogonally, every type has
a **value lifecycle** — what runs when a value is duplicated and when its storage
dies — and a type is move-only exactly when its drop has no inverse.

**Reading guide:**

- [The three reference types](#the-three-reference-types) and
  [Constraint references](#constraint-references) — the borrow model.
- [The value lifecycle](#the-value-lifecycle) — Drop / Retain / Move-only, the
  three *independent* properties every type has, and the principle that relates
  them. **Read this before the mechanics below**; it is what they all implement.
- [The second-class family](#the-second-class-family),
  [Counting mechanics](#counting-mechanics), and [Promotion](#promotion) — how the
  count stays complete and how stack data is handled without one.
- [Weak references and generations](#weak-references-and-generations).
- [Applying the model](#applying-the-model) and
  [Container element lvalues](#container-element-lvalues) — how each language
  feature interacts with the count.
- [Runtime foundations](#runtime-foundations) — object header, slab allocator,
  generations.
- [RAII, moves, and `borrowed`](#raii-moves-and-borrowed) — the user-facing model.
- [Lifecycle implementation and status](#lifecycle-implementation-and-status) —
  how the lifecycle is derived and lowered, and the ordering constraints behind
  [separating Drop from Copy](#separating-drop-from-copy).
- [Limitations and future directions](#limitations-and-future-directions).

---

## The three reference types

| Type | Owns? | Nullable? | Mechanism | Job |
|------|-------|-----------|-----------|-----|
| `uniq` | Yes | Yes | RAII (sole owner) | Allocates and frees the object |
| `ref`  | No  | No  | **Counted borrow** — `ref_count` in the object header | A free is *blocked* while any `ref` is live |
| `weak` | No  | Yes | **Generational** — a 64-bit random id snapshot | Observes that a free has *already happened* |

**Counting decides whether a free is legal; generations let a `weak` notice that a
free already happened.** `ref` is purely counted; `weak` is purely generational; the
two never overlap.

## Constraint references

The invariant `ref` maintains:

> An object's `ref_count` equals the number of live `ref` borrows of it. **No object
> may be freed, by any path, while its `ref_count` is nonzero — the free traps.**

This is a *borrow* count, not an *ownership* count. `uniq` is the sole owner and
never touches `ref_count`; the count only ever *blocks* a free, it never *causes*
one. So there are no ownership cycles to leak. Errors are **eager**: they fire at
the offending `delete`, not at a later dangling use.

Because `ref` borrows only heap objects (see
[The second-class family](#the-second-class-family)), the count always has a home —
`ObjectHeader.ref_count` — and every inc/dec site is *statically known to be heap*,
so no runtime "is this on the heap?" test is needed anywhere except the one
[promotion](#promotion) gate. `ref` stays a thin pointer; copying one increments,
dropping one decrements, with complete bookkeeping on every path.

### What the model guarantees

- **A borrow outliving a freed owner.** `var b: ref T = owner;
  consume_and_free(owner); use(b);` — `b` holds a count, so the callee's free
  **traps** before `b` can dangle.
- **A borrow escaping by `return`.** A returned `ref` to a local carries its count
  out, so the local owner's RAII drop **traps**.
- **An owner killed mid-call through an alias.** `heap_obj.method()` whose body
  frees `heap_obj`, or `evil(l[0], inout l); l.clear();` — the call site holds a
  count on the receiver / element for the call's duration, so the in-flight free
  **traps**.

A missed *decrement* makes an owner permanently undeletable (a loud trap, never
silent corruption); a missed *free-trap* would be a hole. Both are centralized so
completeness is auditable: the trap lives in one function, and the decrements ride
the same scope-cleanup machinery as `uniq` drops.

## The value lifecycle

Cutting across the borrow model — and across every other type — is a second
question: **what has to run when a value is duplicated, and when its storage dies?**

Every type has three *independent* properties:

| Property | Question | Runs when |
|---|---|---|
| **Drop** | what must be released? | a location holding the value dies |
| **Retain** | what must be acquired? | the value is *implicitly duplicated* into a second location |
| **Move-only** | may it be duplicated at all? | — (a static permission) |

Conflating any two of them produces a bug. The principle that ties them together:

> **A type is move-only exactly when its drop has no inverse.**

If dropping a value can be undone by a retain, the value can be duplicated: give
the second location its own retain and each location drops once. If dropping is
destructive — a free, a buffer release — a second location cannot exist without a
*deep* copy, which Roxy deliberately makes explicit (`.copy()`).

### Every type kind

| Type | Drop | Retain | Move-only | Why |
|---|---|---|---|---|
| primitives, `enum`, `bool` | — | — | no | trivial: copy is a memcpy |
| `weak T` | — | — | no | a `{pointer, generation}` snapshot holds no count |
| **`string`** | `roxy_string_release` | `roxy_string_retain` | **no** | reference-counted — release has an exact inverse |
| **`ref T`** | `ref_dec` | `ref_inc` | **no** | a counted borrow — a copy is simply another borrow |
| `uniq T` | run destructor, free | — | **yes** | a free cannot be undone |
| `List<T>`, `Map<K,V>` | drop members, free buffers | — | **yes** | duplicating means copying the buffer — that is `.copy()` |
| `Coro<T>` | run `__coro_*$$delete`, free state | — | **yes** | as `uniq` |
| `fun` closure | dispatch env destructor, free env | — | **yes** | as `uniq` |
| value `struct` | compose over fields | compose over fields | iff a field is move-only, or it has a **user-written** destructor | composition |

Two rows carry the whole subtlety: **`string` and `ref` need drop glue and are
still copyable.** They are why "needs cleanup" and "is move-only" cannot be the same
bit. A struct is move-only because of what it *contains*, not because it earned a
synthesized destructor — though a user-written destructor forces it, since arbitrary
side effects must not run twice.

`.copy()` (explicit, deep) is a different operation from Retain (implicit, shallow).

### Where each one is enforced

- **Drop** — `compute_drop_plan` (`types.cpp`), lowered by both backends; see
  [One derivation, two executions](#one-derivation-two-executions).
- **Retain** — `compute_retain_plan` (`types.cpp`), emitted at every duplication
  site by `emit_value_retain` / `emit_struct_clone_glue`.
- **Move-only** — `Type::noncopyable()` / `is_copy()`, consumed by the move
  checker, call-argument and return lowering, and container element handling.

## The second-class family

`ObjectHeader.ref_count` exists only on heap objects, so **`ref` borrows heap
objects, period.** A `ref` can be created only from a heap source — a `uniq`,
another `ref`, or a [`borrowed`](#the-borrowed-type-modifier) subscript of a
heap-pointee element — and binding one to a stack value-struct does not type-check
(`var r: ref Vec2 = some_stack_vec` is an error).

Stack value-structs are passed by reference only through the **second-class
family** — `out`, `inout`, and the method receiver `self`:

- They may be dereferenced and passed *onward* as further second-class arguments
  (downward the call stack), and nothing else: never bound to a `ref`, stored,
  returned, or captured, and never converted to `uniq` / `ref` / `weak` (see
  [overview.md](../overview.md)).
- Downward-only flow makes them safe *by construction* — the frame that owns the
  stack struct outlives every callee — so they need no count and no header.

`self` belongs to this family. Although the IR types the receiver as a pointer
(`methods.md` writes it `ref<T>`), it is **not** a first-class counted `ref`: it
works uniformly on stack and heap receivers. A function that needs to *retain* a
borrow it receives takes `weak` or a copy.

The consequence: there is no "polymorphic `ref`" that might be stack or heap. `ref`
is always heap (counted, statically); `out` / `inout` / `self` are always
second-class (uncounted, downward). No representation or per-deref test straddles
them.

## Counting mechanics

Cleanup runs on **every** exit path, exception unwinding included, exactly once.

### Increments

A `ref`'s count goes up when:

- a `ref` is **created** from a `uniq` / `ref` / `borrowed`-subscript of a heap
  pointee;
- a `ref` is **copied** into a binding that holds it — another `ref` local, a
  `List<ref T>` / `Map<_, ref T>` slot, a closure capture, or a `ref` parameter;
- a **call site borrows a heap root**: a method receiver or an `out`/`inout`
  argument rooted in a statically-heap object (`heap_obj.method()`,
  `bump(inout heap_obj.field)`) is counted for the call's duration, with no runtime
  test. A *stack* root counts nothing; a receiver that is already a `ref`
  (`r.method()`, `list[i].method()`) is covered by that `ref`'s own count.

### Decrements

Every point a live `ref` (or call-site heap-root borrow) dies, on every exit path —
scope exit, `return`, `break`, `continue`, and exception unwinding. A returned `ref`
is *not* decremented at the returning frame; its count hands off to the caller,
mirroring how a moved `uniq` is not dropped.

`ref` locals are `OwnedKind::RefBorrow` entries in the same `OwnershipTracker` list
as `uniq` locals and owned string temporaries, so they inherit LIFO scope cleanup
and exception records; a `RefDec` cleanup record decrements rather than destroys.
The subtleties:

- **`ref` parameters decrement on exception unwind, not only at `return`.** Each has
  a whole-function `RefDec` record, with its register liveness pinned to the
  function end so the unwind decrement reads a valid register on throw-only paths.
- **The return hand-off is a 1:1 transfer.** Every `ref` return carries *exactly
  one* count (a ref local hands off its create-inc; a ref param, a fresh ref, or an
  owner borrowed on the way out increments; a call result already carries one). The
  binder **adopts** a call result (no inc) and **increments** any other source —
  otherwise a returned ref local bound by the caller would double-count.
- **Skipping the inc and adopting the temporary are one decision.** When a
  `ref`-returning call's result is bound, the binding adopts the count. When it is
  *not* bound (`box.borrow_item().v`), the result is tracked as a temporary
  `RefBorrow` and released by scope cleanup — otherwise the count leaks and the
  owner becomes undeletable. `acquire_ref_borrow` does both halves in one place for
  that reason. Consequence: a discarded borrow lives to the end of its *enclosing
  scope*, so `delete`ing the owner in that same scope still trips the free-trap (see
  `TODO.md`). A loop body is its own scope, so borrows there don't accumulate.
- **Move-vs-borrow on return is decided by the *declared return type*, never the
  returned expression's type.** The declared type says whether the frame hands over
  ownership or a borrow; the expression's type only says how to produce the value.
  `fun f(): ref P { return p; }` must borrow (and hand off a count), and
  `fun P.borrow_kid(): ref Child { return self.kid; }` is a borrow of a field, not a
  move of it. This mirrors how `check_call_args` decides an argument's move from the
  *parameter* type.

### The teardown invariant

The free-trap catches a *premature* free; it cannot catch an object that is never
freed, because it only fires on `delete`. So the runtime takes a census:
`vm_destroy` counts what is still alive **after** `__module_shutdown` has torn down
the globals and **before** the slabs are freed, leaving it on
`RoxyVM::teardown_heap_stats` (`roxy_rt_heap_stats()` is the AOT equivalent). At a
clean exit `leaked` must be **0**. Interned string *literals* are immortal and
counted out.

Consumers:

- **`roxy --check-leaks`** reports the count broken down by type name and exits 70.
- **The E2E harness asserts it on every program it runs** (`run_and_capture`). A
  test pinning a *known* leak opts out with a scoped `ExpectedLeak` naming its
  `TODO.md` entry — the opt-outs are the live list of unfixed leaks.

Constraint: the VM's object-type registry indices must stay identical to the
runtime's `ROXY_TYPEID_*` constants (reserved slot 0, then string/list/map), since
`roxy_rt` stamps those constants straight into the header. A mismatch mislabels
leak reports *and* makes `object_free` pick the wrong destructor (e.g. skip
`map_destructor`).

The check covers the **VM only**; the generated C binary runs no census.

### The free-trap

The single choke point is **`object_free`** (`object.cpp` / `roxy_free`): if
`ref_count != 0` it errors ("cannot delete: object has N active borrows") and
refuses. Checking here rather than in the `delete` opcode makes **every** free path
trap uniformly: explicit `delete`, RAII drop, the descriptor walk (`delete_value`),
container element cleanup, reassignment-overwrite, and move-then-drop.

### Call-site heap-root borrows

A call site that borrows a heap root must hold the count across exactly the call, on
both normal and exception paths, without disturbing the owner's own cleanup:

```
PinnedCopy → RefInc → Call → RefDec → Nullify
```

- The borrow rides a `Copy` flagged `no_copy_prop`, giving it a **distinct SSA
  value** (hence a distinct register), so its `RefDec` + `Nullify` cleanup cannot
  clobber the owned local's or temp's own `Delete` record.
- The exception-path `RefDec` record is **appended after all owned-local records**
  (`m_call_borrow_cleanups`, flushed in `end_function_body`), so reverse-order
  unwind releases the borrow *before* the owner's `Delete` (else the `Delete`
  spuriously traps). Lowering narrows the record to the `[RefInc-pc, RefDec-pc)`
  window.

### Interior pointers

A `borrowed` subscript or a `[ref self]` promotion can target an inline value-struct
field of a heap object, so the count must be reachable from an *interior* pointer.
`resolve_header(ptr)` finds the owning slot's header through the allocator's sorted
slab-range index (large objects use the same index).

## Promotion

`self` is second-class, but `[ref self]` / `[weak self]` capture — and binding `self`
into a first-class `ref` — must turn it into a counted/generational reference, legal
only if the receiver is on the heap. A method doesn't know whether it was called on
a stack or heap receiver, so this is the **one** runtime test: the slab-range check
`AssertHeap`. Heap → produce the first-class reference; stack → **trap** ("cannot
retain a borrow of stack data — copy it, or allocate the receiver with `uniq`").

Gated shapes:

- **`[ref self]` capture** — `RefInc`'d at env construction (after the heap check),
  `RefDec`'d by the env's destructor.
- **Binding / returning / storing `self`** — `emit_ref_borrow_inc` inserts the gate
  before the inc whenever the source is a bare `self`.
- **Passing `self` to a `ref` parameter** — gated at the call site
  (`lower_call_args`), because the unsound inc is the callee's entry inc, so a stack
  receiver must trap *before* the call.

The gate fires for *every* bare-`self` source, so it also traps a noncopyable stack
receiver. Inside a lambda body, `self` is already rewritten to `__env.__self`
(sourced from a heap-checked env), so only a direct method body's bare `self` reaches
the gate.

Promotion depends on closure envs being cleaned up correctly. Because `fun() -> R`
erases which env struct a closure holds, env cleanup is dispatched
virtual-destructor style by the env's runtime `type_id` (`RoxyVM::closure_env_dtors`,
`BCDeleteDesc::Closure`), freeing captured `[move uniq]` values and `RefDec`ing
captured `ref`s. See [closures.md](closures.md).

## Weak references and generations

`weak` is the **only** consumer of generational references. A `weak T` is
`{ptr, generation}`, captured via `WeakCreate`, validated on use (`WeakCheck` /
`roxy_weak_valid`), and yields null / false when the referent is tombstoned or
recycled. Taking, copying, or dropping a `weak` needs **zero bookkeeping**, and the
owner's memory **frees immediately** on delete.

### Why generational

Those two properties are exactly what a no-GC, churny-allocation, value-semantic
runtime needs. The alternatives each give one up:

| Approach | Why rejected for Roxy |
|---|---|
| **Auxiliary weak count + deferred free** (`shared_ptr`/`weak_ptr` style) | Reintroduces all-paths inc/dec on the *most* casually-used reference, and a forgotten weak pins the dead object's slot — which creeps in long-lived weak registries with no GC to reclaim it. Also fights the slab's slot recycling. |
| **No slot reuse + liveness bit** | No ABA → no generation, but only by abandoning free-list recycling. Trades a tiny collision probability for unbounded slot retention under churn. |
| **Intrusive back-list** (object nulls its weaks on death) | Requires every `weak` at a stable, registered address; Roxy weaks are values copied through registers and realloc'd containers. |
| **Page-protection / fault-on-use** | Page granularity is absurd for ~32-byte objects, and a fault is a crash, not a graceful null-on-test. |

The generation is a **random 64-bit** value: 2⁻⁶⁴ collision probability per slot
recycle, and resistance to deliberate reuse attacks from untrusted scripts. A
32-bit generation would shrink the header but weaken a *correctness* property, and
the header isn't on a hot enough path to justify it — so it stays 16 bytes.

## Applying the model

How the count interacts with each language feature.

### Methods and `self`

A method call on a **statically-heap** receiver counts it for the call (an
alias-kill mid-method traps); a **stack** receiver counts nothing (downward-safe);
an **already-`ref`** receiver is covered by its own count. Binding / returning /
storing `self`, or passing it to a `ref` param, is a [promotion](#promotion).

The receiver borrow uses the
[call-site heap-root mechanism](#call-site-heap-root-borrows) for every `uniq`
receiver shape: a bare identifier (`c.method()`), a `uniq` field root
(`o.inner.method()` — a `delete o` would free the receiver, so it traps), and a
heap-returning temp (`make().method()` — counted distinctly from the temp's own
`Delete` via the pinned copy).

### Closures

Captures are by copy. Capturing a `ref` increments; the env's destructor
decrements. `[ref self]` / `[weak self]` are [promotions](#promotion).

### Coroutines

A coroutine's parameters and promoted locals live in its heap state struct, so a
`ref` *parameter* is a counted borrow held for the **state struct's lifetime** —
`ref_inc` at creation (`init_func`), `ref_dec` in the generated `$$delete`. Deleting
the borrowed owner while a suspended coroutine holds the borrow traps, even before
the first resume.

The per-frame entry-inc / exit-dec for `ref` params are *suppressed* for coroutines
(`m_ref_params` cleared), because the coroutine split scatters them across resume
states and would miss the dec on early destroy. A `ref` *local* is decremented on
whichever comes first — its scope exit, else `$$delete` — the resume path clearing
its field so both cannot fire.

A catch param `e` is a `ref` field too, but is never *counted*: it holds an owned
exception object (below), so it is excluded from the `ref_dec` and freed instead
(as `uniq E`, or type-erased for a catch-all), under the same field-clearing rule.

### Caught exceptions

A thrown exception is a heap object the catch *owns*, not borrows: it is an owned
local of the catch scope, freed once on every exit, and a re-throw hands it off
rather than freeing it. This is ownership/RAII, not counting — see
[exceptions.md](exceptions.md) "Exception object lifetime".

### Containers are move-only

A `List<T>` / `Map<K,V>` owns a heap buffer, so — like `uniq` — it is **noncopyable
regardless of element type**: passing by value moves it; `.copy()` deep-copies. The
callee-side value-param deep-copy is skipped for noncopyable containers, so a value
param is a true move.

### Containers are borrowable

A container value *is* the pointer to its slab-allocated header, so borrowing one is
`uniq → ref` with a different pointee: same `ObjectHeader.ref_count`, same free-trap,
and — a container is *always* heap — no [promotion](#promotion) gate.
`List<T> → ref List<T>` is an implicit conversion at any typed site
(`can_convert_ref`), needing no call-site marker.

```roxy
fun total(xs: ref List<i32>): i32 { ... }   // borrows; caller keeps its list
fun consume(xs: List<i32>): i32 { ... }     // moves; caller's list is gone
```

This is the **read-only** container parameter. `inout` is the wrong tool for that:
it advertises mutation and, being exclusive, forbids passing the same container
twice (`compare(xs, xs)`).

`ref` is a borrow, **not an immutable borrow**: mutating through it (`xs.push(1)`) is
legitimate. What it cannot do is reassign the caller's *slot* (that is `inout`, hence
`ref → inout` is rejected), or be moved out of the borrowing frame (`return xs;` is
an error; `.copy()` leaves with an independent owner). Counting is the generic
`ref`-parameter machinery. An `inout` container may be narrowed to a `ref` on the way
down — the pointee is heap either way.

Two consequences inherited from `ref`:

- A `ref` borrow and an element lvalue borrow are **separate counters**: `ref`
  takes `ref_count` (blocks free), `inout xs[i]` takes the container's
  `borrow_count` (blocks realloc). See
  [Container element lvalues](#container-element-lvalues).
- Rebinding a `ref` to a fresh owner (`fun f(r: ref List<i32>) { r = List<i32>(); }`)
  type-checks and then fails at runtime — a general `ref`-rebinding hole (`uniq`
  behaves identically); see `TODO.md`.

### Containers of borrows hold counted borrows

`List<ref T>` and `Map<_, ref V>` count their borrowed elements: every acquire
`RefInc`s the pointee, every release `RefDec`s it, so deleting an owner a container
still borrows traps.

- **`List<ref T>`** — `push` acquires, `pop` hands the count to the caller
  (ref-return adopt), `refs[i] = x` releases-old / acquires-new.
- **`Map<_, ref V>`** — `insert` acquires (releasing a replaced value), `remove` /
  `clear` release, `copy` re-acquires. This is runtime-side, gated by a
  `value_is_ref` header flag the compiler sets (`roxy_map_mark_ref_values`) right
  after construction.
- **Destroy** `RefDec`s each element via a `RefDec` element descriptor (VM) /
  `roxy_ref_dec` (C).

### Containers of owners destroy their owners

`Map.remove` / `Map.clear` / a replacing insert also destroy **noncopyable** values.
The cleanup is emitted as ordinary IR at the call site, where the value type is
statically known (rather than a runtime per-value destructor callback), so both
backends get it for free: a `contains`-guarded `delete m[k]` before `remove`, a
`__map_iter_*` deletion loop before `clear`, and for `insert` the value-arg consume
is *deferred* past the guard so the replaced value is freed and the incoming temp
consumed in the right order.

### Reassignment and overwrite cleanup

An overwrite must destroy what it replaces and consume what it stores:

- **`container[i] = v`** (`gen_assign_index`) — for a noncopyable element, destroy
  the old one (unconditionally for a List; `contains`-guarded for a Map), then
  consume the RHS temporary. The consume keys off the *container's* element type.
- **`slot = uniq T(..)` / `slot = nil`** through an owning `inout`/`out` pointer —
  `Delete` the old value before the store and consume the RHS temp.

### `borrowed T`

A subscript on a **heap-pointee** element (`List<uniq T>`) yields a counted `ref` —
realloc moves the buffer, not the pointee, so the borrow stays valid. A subscript on
an **inline** element (`List<Vec2>`) is a second-class, expression-scoped borrow (no
per-element header to count). See
[The `borrowed` type modifier](#the-borrowed-type-modifier).

### `out` / `inout`

An argument rooted in a heap object counts that root for the call via the
[call-site heap-root mechanism](#call-site-heap-root-borrows); a stack-rooted one is
safe by downward flow.

- **Escape rule.** A noncopyable `out`/`inout` cannot be moved out of its frame —
  bind / return / store / by-value-pass / capture-by-move are compile errors.
  (Copyable ones can't escape either: there is no value→reference conversion.)
- **Root counting.** A field-rooted lvalue (`f(inout a.b.c)`) borrows the innermost
  heap object it points into (`heap_root_of_lvalue`). A `ref`-rooted lvalue is
  covered by the `ref`; a bare identifier roots in the caller's frame.
- **Index-rooted lvalues** (`f(inout list[i])`) need more than a count — see
  [Container element lvalues](#container-element-lvalues).

### FFI / AOT

A `ref T` passed to a native is counted for the call, so the object **cannot be
freed during the call**, even by reentrant Roxy code — the native's raw pointer is
guaranteed live. `weak`'s `{ptr, gen}` ABI is unchanged.

### Move checker

`ref` is copyable (copy = inc), so it is not move-tracked; `ref` bindings instead get
cleanup records that `RefDec` on all exit paths.

## Container element lvalues

`f(inout list[i])` passes the **actual address** of the element in the backing
buffer — true aliasing, no copy. Copy-in / copy-out was rejected: sound, but not a
real lvalue (a concurrent read during the call sees the stale value). Works on both
backends for primitive, struct, and `uniq` elements of `List` and `Map`.

### The hazard

The element buffer is separately `malloc`'d **outside** the slab, so the header's
machinery does not protect it. Any of these, reached mid-call through another
channel (a second argument, a global), invalidates `&list[i]`:

| Mid-call operation | Effect on `&list[i]` |
|---|---|
| `delete list` | buffer freed → dangling |
| `list.push(x)` (grow) | buffer realloc'd → dangling |
| `pop` / `remove` / `clear` | element gone / buffer freed → dangling |
| `list[j] = v` (in-place set) | **safe** — same slot, no move |

A header count blocks only the *free*, not realloc — so a count alone is not enough.

### The pin

While `&list[i]` is outstanding, the container is frozen against exactly the
operations that move or free the buffer:

- **Free** — blocked by the ordinary free-trap: the call site takes a `ref_count` on
  the container.
- **Structural mutation** — blocked by a separate **`borrow_count`** in the
  container header (`roxy_list_header` / `roxy_map_header`), incremented by
  `ContainerPin` before the call and decremented by `ContainerUnpin` after it and on
  unwind (a deferred `Unpin` cleanup record narrowed to the call window).

`borrow_count` must be separate from `ref_count` because
`fill(r: ref List<i32>) { r.push(1) }` is legitimate, yet `r`'s entry `RefInc` makes
`ref_count > 0`. Mutation-blocking must be scoped to *element* borrows only.

The mutation guards live in the shared runtime (`roxy_list_push` / `pop`,
`roxy_map_insert` / `remove` / `clear`) and raise a *fatal, non-catchable* trap
(a thread-local channel distinct from user exceptions) while `borrow_count > 0`,
leaving the buffer untouched. The VM routes through the same functions, so one guard
per op covers both backends. The element address comes from `IROp::IndexAddr`
(`INDEX_ADDR_LIST` / `INDEX_ADDR_MAP`): a checked element-address op, no reload
needed.

### Soundness

| Mid-call event | Outcome |
|---|---|
| `delete` the container | `ref_count` free-trap |
| `push` / `insert` (realloc) | `borrow_count` mutation-trap |
| `pop` / `remove` / `clear` | `borrow_count` mutation-trap |
| in-place `list[j] = v` | allowed (valid slot) |
| free + slot recycled into a new container | impossible — the free is trapped first |

**Owning elements** (`List<uniq T>` / `Map<K, uniq V>`): an `inout`/`out` subscript
re-types to the raw element type (`uniq T`), not the `borrowed` view (`ref T`), so the
callee can reassign the owning slot. The reassign frees the old pointee and the
escape rule forbids moving the element out, so the container still owns exactly one
value per slot.

The rule this surfaces: **you cannot structurally mutate a container while an element
of it is borrowed (`inout` / `out`).** The pin is per-container (coarse), which is
simple and sufficient.

## Runtime foundations

### Object header

Every heap object is prefixed by a 16-byte header (`roxy_object_header` in
`roxy_rt.h`): `{ u64 weak_generation, u32 ref_count, u32 type_id }`.
`weak_generation == 0` means dead/tombstoned. `ref_count` is the count the
[constraint-reference model](#constraint-references) maintains; `weak_generation` is
what `weak` validates against.

### Slab allocator

Heap objects come from fixed-size slabs by power-of-two size class (32 B – 4 KB);
larger objects get dedicated pages. Slabs sit on platform virtual memory
(`rt/vmem.hpp`). The allocator lives in `roxy_rt` and is shared by both backends: the
VM plugs a per-VM `SlabAllocator` into `roxy_ctx.allocator`; AOT uses a process-wide
slab created by `roxy_rt_init`. Both get identical weak-ref soundness; a malloc
fallback applies only when no ctx is active.

### Tombstoning and recycling

On free (the path the free-trap guards):

1. The whole slot is zeroed, so `weak_generation` reads 0.
2. The slot goes on its slab's intrusive free list. The next-pointer sits past the
   header, so `weak_generation` keeps reading zero while parked.
3. Memory stays mapped, so weak references can keep dereferencing the header safely.

A recycled slot gets a fresh random `weak_generation`, so a stale weak mismatches.
`weak_ref_valid` returns `is_alive() && weak_generation == generation`.

`reclaim_tombstoned()` releases the physical pages of fully drained slabs with
`remap_to_zero()` (the vaddr stays mapped as zeros, so stale weaks still read a dead
header) and retires them from allocation.

## RAII, moves, and `borrowed`

### Implicit destruction (RAII)

`uniq` variables, value structs with a destructor (user-written, or synthesized for an
owning field — a `string` or `ref` field included), and containers are cleaned up
automatically at every scope exit (`}`, `return`, `break` / `continue`), in **LIFO**
order. A destructor (`fun delete T()`) runs before the memory is freed; a container
runs a per-element cleanup loop first (see [list.md](list.md), [maps.md](maps.md)).
Deleting null is a no-op, so `var x: uniq T = nil;` and moved-out (null-ified)
variables never double-free.

### Move semantics

Binding / passing / returning a noncopyable value **moves** ownership; the source
becomes invalid. This applies to `uniq`, move-only value structs, and containers.
There is no implicit deep copy (`var copy = items` moves; use `.copy()`), and
reassigning destroys the old value before storing the new one.

**Moving a field out.** A noncopyable *pointer* field (`uniq`/`List`/`Map`/…) may be
moved out of a local value struct; the compiler nulls that field in the root at the
move site, so the root's destructor still frees the surviving siblings. For
use-checking, the *whole* root is conservatively marked moved (per-field move state is
not tracked). Moving a noncopyable *value-struct* field out is a compile error.

### Use-after-move detection

`LifetimeChecker` tracks a move state per noncopyable local — `Live`, `Moved`, or
`MaybeValid` (moved on some paths) — and using a `Moved` or `MaybeValid` variable is
a compile error.

**Who destroys a `MaybeValid` value.** Because every later *read* is rejected, the
value is dead at the merge whichever path ran, so the branches that did **not** move
it destroy it where they leave (`IRBuilder::reconcile_divergent_moves`) — no runtime
drop flag. Its lifetime ends at the branch construct rather than at scope exit: an
early drop, never a missing or doubled one. The merge cannot instead pick one value
for the shared move flag: "moved" leaks on paths still holding the value, "not moved"
double-frees on paths that gave it away. Where the non-moving path is an implicit
fall-through edge, a block is materialized to hold the drop.

**Alternative paths must start from the same move state.** `if`, `if/else if`, and
`when` snapshot the state before the first branch and restore it per branch — a
branch that destroys a local marks it moved, which would otherwise make its siblings
skip their destroy. Exempt: an exhaustive `when`'s *trapping* else (unreachable;
restoring would resurrect what every real arm moved), and `catch`.

`catch` deliberately does **not** restore the move state: it runs *after* part of the
try body, not as an alternative from a common start, so rolling back would re-enable
the implicit destroy in `r = uniq T()` for a `uniq` the try body already consumed —
a double-free. Use-after-move in a catch is sema's job to reject.

### The `borrowed` type modifier

`borrowed T` is a **resolve-time type transform** that demotes an owning type to a
borrow — it lets a built-in container accessor say "I return a *view*, not ownership"
in a signature whose element type is unknown until monomorphization, where returning
an owning `uniq T` by alias would double-free.

> **Not user-facing syntax.** `borrowed` is a contextual keyword recognized **only
> while parsing a native binding signature** (`Parser::set_native_signature_mode`);
> in user source it is an ordinary identifier. Keep it that way: the LSP's parser
> does not recognize it, and a user never needs it (`borrowed uniq T` is `ref T`).
> Its consumers are `List<T>.index`, `Map<K, V>.index`, and `Map<K, V>.get`.

| `borrowed X` | → | rationale |
|---|---|---|
| `uniq T` | `ref T` | borrow the heap pointee instead of transferring it |
| `fun(…) -> R` | `ref fun(…) -> R` | a closure is a heap env pointer; `ref fun` shares its representation and is callable |
| copyable `T` | `T` | a copy aliases nothing |
| `ref T` / `weak T` | unchanged | already a borrow |
| other noncopyable (value struct, coro, `List`/`Map`) | unchanged | identity (see below) |

It never persists as a `Type`; it rides on `TypeExpr::is_borrowed` through generic
substitution, so it resolves per monomorphization. `var x: uniq Point = list[i]` is
then a plain `ref → uniq` type error. `List<fun>` indexing yields a callable,
storable `ref fun` (see [closures.md](closures.md)).

For the remaining noncopyable kinds `borrowed` is the **identity**, so the type system
alone cannot reject a move-out. Two mechanisms cover the gap, both keyed on
`MethodInfo::returns_borrowed`:

- **The move checker** (`LifetimeChecker::is_borrowed_native_accessor`) rejects
  *binding* the result (`var stolen: List<i32> = m.get(0);`) while allowing in-place
  uses.
- **The IR builder** (`IRBuilder::is_borrowed_view_call`) does not track the result
  as an owned temporary — otherwise a *discarded* view (`m.get(0).len()`) would be
  destroyed at scope exit, freeing the container's own element.

Keying on the modifier rather than "the method is native" keeps `pop` / `copy` /
`keys` / `values` consumable — they return genuinely fresh values. Once coroutines and
containers get `ref`-receiver dispatch they could demote to `ref`, making both
mechanisms dead weight.

## Lifecycle implementation and status

### One derivation, two executions

`compute_drop_plan(Type) -> DropPlan` (`types.cpp`) decides the *kind* of drop once,
and **both backends lower the same plan**:

- **VM** keeps its **native** `delete_value` walk over `BCDeleteDesc` — that *is* the
  VM's drop-glue executor; emitting interpreted bytecode glue would be slower.
- **AOT/C** lowers the plan to generated `roxy_drop__<T>` glue functions (and a
  struct's `$$delete`), which the C compiler inlines and folds.

Neither re-derives. At a *true* erasure boundary — a closure env dropped by
`type_id` — a single drop-glue dispatch survives; that is one pointer for one
operation, **not** a per-operation vtable.

`member_needs_drop()` ("does a value in a struct field / container element slot need
cleanup?") **derives** from `compute_drop_plan`, so the gate and the lowering it gates
cannot disagree. It is deliberately non-recursive: a nested value struct that owns
something carries its own synthesized destructor, propagated by the
synthetic-destructor fixpoint.

### What is actually implemented

| Property | Derivation | Consumed by |
|---|---|---|
| **Drop** | `compute_drop_plan` | both backends, `member_needs_drop` |
| **Retain** | `compute_retain_plan` | `emit_value_retain` / `emit_struct_clone_glue` at every duplication site (struct copies, `List.push`, the map value store, `values()`/`copy()`), `member_needs_retain` |
| **Move-only** | `StructTypeInfo::is_move_only` (`derive_struct_move_only`) | `noncopyable()`, move checker, call/return lowering |

`Type::needs_drop()`, `needs_retain()` and `is_trivial()` are structural predicates
no codegen consults (pinned by the `Lifecycle Predicates` suite; `needs_drop()` backs
one cross-check assertion in `build_delete_desc`).

### Separating Drop from Copy

Drop, Retain, and Move-only were once one bit — `noncopyable()` on a struct meant
*"has a default destructor"*. That held only while the members earning a synthetic
destructor were exactly the move-only ones. `string` and `ref` break it in opposite
directions: a `string` field earned no drop (leaked), and a `ref` field earned a drop
and so forced move-only (`var b = a;` rejected). The fix is three changes:

1. **A retain derivation** mirroring the drop plan (`compute_retain_plan`):
   `string` → `StrRetain`, `ref` → `RefInc`, a copyable struct with any retaining
   field → `WalkFields`, everything else (including every move-only kind) → `None`.
2. **Structural move-only**, derived in a whole-program fixpoint
   (`SemanticAnalyzer::derive_move_only_flags`) that runs *before* the
   synthetic-destructor fixpoint:
   ```
   is_move_only(S) = S has a USER-WRITTEN default destructor
                  || ∃ field f : is_move_only_type(f.type)
   is_move_only_type(T) = Uniq | List | Map | Coroutine | Function
                        | (Struct && is_move_only(T))
   ```
   `noncopyable()` **asserts** the flag has been derived rather than reading a
   default `false` — that assert is what catches ordering gaps (a query before the
   derivation runs; native structs never deriving). `resolve_type_members` is split
   into three phases for it: shape, derive, everything that may ask. User-visible
   consequence: passing a `ref`-bearing struct by value no longer ends the caller's
   borrow, so deleting the owner while such a struct is live traps.
3. **Clone glue at the duplication sites, and `StrRelease` enabled** in
   `member_needs_drop`. The glue is keyed on `member_needs_drop`, so acquisition and
   release are inverses by construction.

### The ordering constraint

Each change is unbalanced *on its own*:

| Alone | Result |
|---|---|
| retain glue | retains with no matching release — invisible to the teardown census (same object, higher count) |
| `StrRelease` in the gate | string-bearing structs earn a destructor → become **move-only** |
| structural move-only | `ref`-bearing structs become copyable while nothing balances their counts → **use-after-free** |

**A step is safe alone once its counterpart exists**, not because of its position in
the list — e.g. structural move-only became safe on its own once the clone glue
existed. Re-derive the row rather than trusting it. Only the full combination is
sound: the struct earns a drop, stays copyable, and every duplication retains.

### The container side

Containers release counted keys and values on teardown, so each store has to acquire:

- `List.push` acquires through `emit_value_retain`.
- The map value store (`m.insert(k, v)` and `m[k] = v` are the same operation) goes
  through `emit_map_value_ownership`: acquire for the slot, and release the old value
  (`contains`-guarded). `clear()` / `remove()` release on the same gate.
- `values()` and `copy()` memcpy the slots, so the result **shares** the original's
  elements and must acquire its own counts through a retain loop
  (`emit_list_elements_retain` / `emit_map_values_retain`) — otherwise the element
  dies under whichever container outlives the other.

A **struct literal used inline** is an owner too, easy to miss because it has no
name: its field stores acquire and its storage is a bare stack allocation, so it is
tracked for cleanup like any temporary (adopted if bound, released if passed as an
argument like `xs.push(S { s = f"..." })`). Only *copyable* value literals need this:
a `uniq S { ... }` self-tracks, and a move-only literal's contents are moved.

Map **keys** are counted in the *runtime*, not in emitted IR (`roxy_map_insert`
acquires, `remove` / `clear` release, `roxy_map_keys` acquires for its result). Only
the runtime knows *which* key is stored — insert replaces in place and keeps the key
it has, and remove must release the stored key, not the caller's equal copy — while
only the compiler can walk an arbitrary value type. Keys are a closed set of
runtime-known kinds (`key_kind`), which makes the runtime side possible.

A **copyable struct key holding a counted member** is deliberately unhandled: the
runtime cannot walk it, and such a key could never match on lookup anyway
(`map_keys_equal` compares key bytes). The teardown key gate matches: it drops only a
move-only key or a `string`.

### What the flip exposed in the unwind path

Drop glue on copyable structs put **value structs** into exception cleanup records,
breaking assumptions that held only because every tracked local used to be
pointer-shaped (null-safe to delete):

- **A record's live range is not its scope range.** A value struct has no null form,
  so a record must not fire before the local is written (e.g. unwinding out of the
  call that initializes it). Records carry `live_start_pc` (used by the throw test)
  separately from `scope_start_pc` (used by the handler-in-scope test — narrowing
  that one lets a handler fall outside a scope it is in, and both paths clean up).
- **`DELETE` nulls its operand only when `free_obj`.** Nulling is double-free
  protection for a freeing delete but clobbers the storage of an in-place one.
- **One PC interval cannot cover a scope.** RPO places a throw-terminated branch
  *after* the scope's normal-exit block, outside the interval, and SSA liveness may
  already have recycled the register there. Lowering therefore computes each record's
  **coverage** — every block reachable from the value's def without passing an
  ownership-ending kill (a `Nullify`, or the record's own cleanup op), following
  exception edges into a handler only when the handler's continuation demonstrably
  cleans the value up (or a rethrowing `finally` never exits normally) — pins the
  register across it (liveness Pass 5b in `compute_liveness`), and emits
  **extension records** (`BCCleanupRecord::is_extension`) for covered PC runs outside
  the main interval. The unwinder treats a head record plus its extensions as one
  group: every interval is consulted and the action runs at most once.
- **A moved argument's kill is anchored at the consuming call's boundary** (the word
  after the `CALL`), not at its later `Nullify`. A throw escaping the callee surfaces
  at that boundary, when the callee already owns (and on unwind frees) the value, so
  covering the gap double-frees.

The C backend is unaffected: it replays every `IRCleanupInfo` record once from its
`__unwind` label with null guards, which is layout-independent.

## Limitations and future directions

### Residual risks and sharp edges

- **Trapping during unwind.** Cleanup is LIFO, so a borrow declared after its owner is
  released first — no spurious trap. A genuine escape (a borrow stored in something
  longer-lived) freed during *exception* unwinding traps mid-unwind; a clear trap
  beats a use-after-free.
- **Completeness is the whole game.** A missed decrement makes the owner permanently
  undeletable (loud, not unsafe); an un-trapped free path would be a use-after-free
  hole.
- **Count underflow** is a tripwire ("ref_dec: reference count already zero") in both
  runtimes — `vm->error` in the VM, a debug assert / fatal trap in AOT
  (`roxy_ref_dec`). Overflow of the `u32` is bounded by live-borrow count.
- **Single-threaded.** Inc/dec are non-atomic; a threaded runtime needs atomic counts.
- **Malloc-fallback allocator** (AOT before `roxy_rt_init`): `resolve_header` and the
  free-trap assume slab-backed objects.

### Future directions

- **Refcount elision** — remove inc/dec pairs wherever the owner provably cannot be
  freed during the borrow (no intervening `delete`, move, or call that could reach a
  free). Elision only removes *provably-redundant* counts, so incomplete elision is
  slower, never unsafe. Easiest win: call-site receiver/arg counts where the heap root
  is a local the callee can't reach (`local.method()`).
- **Folding the promotion gate** — where the receiver's storage is statically known,
  `AssertHeap` could fold to an unconditional inc or a compile error.
- **AOT trap reporting for container pins** — the mutation guard is memory-safe on
  both backends, but the C backend's clean trap *report* is part of the broader
  AOT-trap-reporting work.

## Related docs

- [overview.md](../overview.md) — reference-type philosophy; `out`/`inout` restrictions.
- [methods.md](methods.md) — `self` as the receiver.
- [closures.md](closures.md) — `self` capture modes; `AssertHeap` is the promotion gate.
- [coroutines.md](coroutines.md) — the state struct.
- [exceptions.md](exceptions.md) — exception object lifetime.
- [list.md](list.md), [maps.md](maps.md) — container internals and per-element cleanup.
