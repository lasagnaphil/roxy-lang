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
- [Weak references and generations](#weak-references-and-generations) — the other
  half of the safety story.
- [Applying the model](#applying-the-model) and
  [Container element lvalues](#container-element-lvalues) — how every language
  feature interacts with the count.
- [Runtime foundations](#runtime-foundations) — the object header, the slab
  allocator, and generations that everything above rests on.
- [RAII, moves, and `borrowed`](#raii-moves-and-borrowed) — the user-facing model.
- [Lifecycle implementation and status](#lifecycle-implementation-and-status) —
  how the lifecycle is derived and lowered, **what is not implemented**, and the
  plan for [separating Drop from Copy](#separating-drop-from-copy--landed-2026-08-02).
- [Limitations and future directions](#limitations-and-future-directions).

> **A note on status.** This document describes both what the compiler does and
> what the model requires. Where they differ, the gap is called out inline and
> summarised in
> [Lifecycle implementation and status](#lifecycle-implementation-and-status).
> Sections marked **PLANNED** are design, not description. An earlier revision
> presented the lifecycle model as if it were fully built; it was not, and a
> struct field silently leaking its `string` went unnoticed for exactly that
> reason.

---

## The three reference types

Roxy boxes objects on the heap and manages them with three reference types — no
garbage collector, no ownership cycles to leak.

| Type | Owns? | Nullable? | Mechanism | Job |
|------|-------|-----------|-----------|-----|
| `uniq` | Yes | Yes | RAII (sole owner) | Allocates and frees the object |
| `ref`  | No  | No  | **Counted borrow** — `ref_count` in the object header | A free is *blocked* while any `ref` is live |
| `weak` | No  | Yes | **Generational** — a 64-bit random id snapshot | Observes that a free has *already happened* |

The two safety mechanisms have clean, separate jobs: **counting decides whether a
free is legal; generations let a `weak` notice that a free already happened.** `ref`
is purely counted; `weak` is purely generational; the two never overlap.

## Constraint references

The invariant `ref` maintains:

> An object's `ref_count` equals the number of live `ref` borrows of it. **No object
> may be freed, by any path, while its `ref_count` is nonzero — the free traps.**

This is a *borrow* count, not an *ownership* count. `uniq` is the sole owner and
never touches `ref_count`; the count only ever *blocks* a free, it never *causes*
one. So there are no ownership cycles to leak — a `ref` cannot keep its owner alive,
only keep it from being freed out from under a live borrow. Errors are **eager**:
they fire at the offending `delete`, not at a later dangling use.

Because `ref` borrows only heap objects (see
[The second-class family](#the-second-class-family)), the count always has a home —
the `ObjectHeader.ref_count` (see [Runtime foundations](#runtime-foundations)) — and
every inc/dec site is *statically known to be heap*, so no runtime "is this on the
heap?" test is needed anywhere except the one promotion gate
([Promotion](#promotion)). `ref` stays a **thin pointer** (2 slots); what makes it
special is that it is a count-bearing *type*: copying one increments, dropping one
decrements, with complete bookkeeping on every path.

### What the model guarantees

Three scenarios that would be use-after-free or leaks in a naive design, and how the
count catches each one:

- **A borrow outliving a freed owner.** `var b: ref T = owner;
  consume_and_free(owner); use(b);` — creating `b` increments `owner`'s count to 1.
  When the callee frees it, `object_free` sees count 1 and **traps** at the free,
  before `b` can dangle.
- **A borrow escaping by `return`.** A function returns a `ref` to a local it then
  destroys — the returned `ref` carries its count out, so the local owner's RAII
  drop sees count 1 and **traps** at the drop.
- **An owner killed mid-call through an alias.** `heap_obj.method()` whose body
  reaches and frees `heap_obj`, or `evil(l[0], inout l); l.clear();` — the call site
  holds a count on the receiver / element for the call's duration, so the in-flight
  free sees count ≥ 1 and **traps**.

A missed *decrement* makes an owner permanently undeletable (a loud trap at its
`delete`, never silent corruption); a missed *free-trap* would be a hole. Both
mechanisms are centralized so completeness is auditable: the trap lives in one
function, and the decrements ride the same scope-cleanup machinery as `uniq` drops.

## The value lifecycle

The borrow model above governs `ref`. Cutting across it — and across every other
type — is a second question: **what has to run when a value is duplicated, and
when its storage dies?**

Every type has three *independent* properties:

| Property | Question | Runs when |
|---|---|---|
| **Drop** | what must be released? | a location holding the value dies |
| **Retain** | what must be acquired? | the value is *implicitly duplicated* into a second location |
| **Move-only** | may it be duplicated at all? | — (a static permission) |

They are independent, and conflating any two of them produces a bug. The one
principle that ties them together:

> **A type is move-only exactly when its drop has no inverse.**

If dropping a value can be undone by a retain, the value can be duplicated: give
the second location its own retain and each location drops once. If dropping is
destructive — a free, a buffer release — there is nothing to undo, so a second
location cannot exist without a *deep* copy, which Roxy deliberately makes
explicit (`.copy()`).

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
still copyable.** They are the reason "needs cleanup" and "is move-only" cannot
be the same bit. A struct is move-only because of what it *contains*, not
because it happens to have earned a destructor — a user-written destructor
forces it too, since arbitrary side effects must not run twice.

`.copy()` is the escape hatch for the move-only rows: an explicit, deep
duplication. It is a different operation from Retain, which is implicit and
shallow.

### Where each one is enforced

- **Drop** — `compute_drop_plan(Type) -> DropPlan` (`types.cpp`), lowered by both
  backends. See
  [One derivation, two executions](#one-derivation-two-executions).
- **Retain** — `compute_retain_plan(Type) -> RetainPlan` (`types.cpp`), emitted by
  `emit_value_retain` / `emit_struct_clone_glue` at every duplication site. See
  [status](#lifecycle-implementation-and-status).
- **Move-only** — `Type::noncopyable()` / `is_copy()`, consumed by the move
  checker, call-argument and return lowering, and container element handling.

> The three were once one bit: `noncopyable()` on a struct meant literally *"has
> a default destructor"*, so a struct earned a drop and lost copyability in the
> same instant. They are now derived independently — see
> [Separating Drop from Copy](#separating-drop-from-copy--landed-2026-08-02) for
> what that took and what it exposed.

## The second-class family

`ObjectHeader.ref_count` exists only on heap objects, so **`ref` borrows heap
objects, period.** A `ref` can be created only from a heap source — a `uniq`,
another `ref`, or a [`borrowed`](#the-borrowed-type-modifier) subscript of a
heap-pointee element (a *view*, not ownership) — and binding one to a stack
value-struct does not type-check (`var r: ref Vec2 = some_stack_vec` is an error).

Stack value-structs are passed by reference only through the **second-class
family** — `out`, `inout`, and the method receiver `self`:

- They may be dereferenced and passed *onward* as further second-class arguments
  (downward the call stack), and nothing else: never bound to a `ref`, stored,
  returned, or captured.
- They carry the rule that they cannot be converted to `uniq` / `ref` / `weak` in
  any way (see [overview.md](../overview.md)).
- Downward-only flow makes them safe *by construction* — the frame that owns the
  stack struct outlives every callee — so they need no count and no header.

`self` belongs to this family. Although the IR types the receiver as a pointer
(`methods.md` writes it `ref<T>`), it is **not** a first-class counted `ref`: it
obeys the second-class rules and works uniformly on stack and heap receivers. A
function that needs to *retain* a borrow it receives takes `weak` or a copy — it
cannot stash a second-class borrow.

The consequence to internalize: there is no "polymorphic `ref`" that might be stack
or heap. `ref` is always heap (counted, statically); the stack-capable things
(`out` / `inout` / `self`) are always second-class (uncounted, downward). The two
never mix, so no representation or per-deref test straddles them.

## Counting mechanics

> **Picking up the unwind-path cleanup bug?** `HANDOFF.md` at the repo root has
> the operational half — both reproductions, the traps that cost a previous
> session time, and the allocation-tagging / refcount-pairing method that found
> the causes fixed so far. This section stays the canonical model: cleanup runs
> on **every** exit path, exception unwinding included, exactly once.

### Increments

A `ref`'s count goes up when:

- a `ref` is **created** from a `uniq` / `ref` / `borrowed`-subscript of a heap
  pointee (a first-class borrow is born);
- a `ref` is **copied** into a binding that holds it for a lifetime — another `ref`
  local, a `List<ref T>` / `Map<_, ref T>` slot, a closure capture, or a
  `ref`-typed parameter;
- a **call site borrows a heap root**: when a method receiver, or an `out`/`inout`
  argument, roots in a statically-heap object (`heap_obj.method()`,
  `bump(inout heap_obj.field)`), the call site increments that root's count for the
  call's duration. The root's heap-ness is known statically, so this needs **no**
  runtime test. A *stack* root increments nothing (second-class, safe by downward
  flow); a receiver that is already a counted `ref` (`r.method()`,
  `list[i].method()`) is already covered by that `ref`'s own count.

### Decrements

Every point a live `ref` (or a call-site heap-root borrow) dies — and on **every**
exit path: normal scope exit, `return`, `break`, `continue`, and exception
unwinding. A returned `ref` is *not* decremented at the returning frame; its count
hands off to the caller, mirroring how a moved `uniq` is not dropped.

`ref` locals are tracked as `OwnedKind::RefBorrow` entries in the same
`OwnershipTracker` entry list as `uniq` locals (`OwnedKind::Owned`) and owned
string temporaries (`OwnedKind::StrOwn`), so they inherit LIFO scope cleanup and
exception records for free; a `BCCleanupKind::RefDec` cleanup record makes
`execute_cleanup` decrement rather than destroy. Two subtleties the bookkeeping must
get right:

- **`ref` parameters must decrement on exception unwind, not only at `return`.** A
  whole-function-scoped `RefDec` record per `ref` param, with that param's register
  liveness pinned to the function end, ensures the unwind decrement reads a valid
  register even on throw-only paths.
- **The return hand-off is a 1:1 transfer.** Every `ref` return carries *exactly
  one* count (a ref local hands off its create-inc; a ref param, a fresh ref —
  field / borrowed subscript / `ref x` — or an *owner borrowed on the way out*
  increments to produce one; a call result already carries one). The binder then
  **adopts** a call result (no inc) and **increments** any other still-live
  source. Otherwise a returned ref local bound by the caller would double-count —
  a safe over-count, but a spurious trap.
- **Skipping the inc and adopting the temporary are the same decision.** The one
  count a `ref`-returning call hands over needs an owner in the caller. When the
  result is bound (`var r: ref T = f();`, `r = f();`, `return f();`) the binding
  adopts it and no inc is emitted. When it is *not* bound
  (`box.borrow_item().v`), nothing would — so the result is tracked as a
  temporary `RefBorrow` and released by ordinary scope cleanup. Leaving that out
  leaked the count outright and made the owner permanently undeletable, which is
  why `acquire_ref_borrow` performs both halves in one place rather than letting
  each bind site re-derive them. Consequence to know: a discarded borrow lives to
  the end of its *enclosing scope*, the same rule every temporary follows — so
  `delete`ing the owner in that same scope still trips the free-trap (see
  `TODO.md`). A loop *body* is its own scope, so borrows there release
  per-iteration and don't accumulate.
- **Move-vs-borrow on the return path is decided by the function's *declared
  return type*, never by the returned expression's type.** They answer different
  questions: the declared type says whether the frame hands over ownership or
  hands out a borrow; the expression's type only says how to produce the value.
  Conflating them let `fun f(): ref P { return p; }` — an owner returned
  *directly*, without an intermediate `ref` local — take the move path, so no
  count was handed off, the caller's `RefDec` underflowed, and the destroyed
  local was read through the returned pointer instead of trapping at its drop.
  The same conflation on the sema side rejected `fun P.borrow_kid(): ref Child { return self.kid; }`
  with "cannot move out of a struct field" — a borrow of a field is not a move of
  it. Both sides now read the declared type, mirroring how `check_call_args`
  decides an argument's move from the *parameter* type.

### The teardown invariant

The free-trap below catches a *premature* free. It cannot catch the opposite
error — an object that is never freed at all — because it only fires when
something calls `delete`. That left every leak and every unbalanced retain
invisible: a missed `RefDec` shows up only if a later `delete` happens to trip
over it, and a missed drop shows up never.

So the runtime takes a census at the end of its life. `vm_destroy` counts what
is still alive **after** `__module_shutdown` has torn down the globals and
**before** the slabs are freed — the one moment the number is meaningful — and
leaves it on `RoxyVM::teardown_heap_stats` for the caller to read
(`roxy_rt_heap_stats()` is the AOT-side equivalent over the global slab). At a
clean exit `leaked` must be **0**: RAII dropped every `uniq` local, every
container and dynamic string was released, and every borrow's count came back
down. Interned string *literals* are immortal by design and are counted out
rather than reported.

Two consumers:

- **`roxy --check-leaks`** reports the count, broken down by type name, and exits
  70. Off by default, so a program's own exit code is unaffected. The breakdown
  is what turns "215 objects leaked" into a lead — it is how the Lox leak was
  split into a `string`-field bug and a separate per-call `List` leak.
- **The E2E harness asserts it on every program it runs** (`run_and_capture`), so
  ~880 existing tests check for leaks without having been written to. A test that
  pins a *known* leak opts out with a scoped `ExpectedLeak` naming the `TODO.md`
  entry it pins — the opt-outs are the live list of unfixed leaks.

Type names in the report resolve through the VM's object-type registry, whose
indices are kept identical to the shared runtime's `ROXY_TYPEID_*` constants
(a reserved slot 0, then string/list/map) — `roxy_rt` stamps those constants
straight into the object header. They used to disagree, which mislabeled every
container in a leak report *and* meant a heap map resolved to the first user
struct type, so `object_free` never ran `map_destructor`.

The check covers the **VM only**. The C backend has its own codegen paths (see
c-backend.md "Known C-backend gaps"), and the generated binary does not run a
census; extending it there means emitting the check into generated code.

### The free-trap

The single choke point is **`object_free`** (`object.cpp` / `roxy_free`): before
freeing, if `ref_count != 0`, it sets the error ("cannot delete: object has N active
borrows") and refuses. Putting the check here — rather than in the `delete` opcode
alone — makes **every** free path trap uniformly: explicit `delete`, RAII drop, the
descriptor walk (`delete_value`), container element cleanup, reassignment-overwrite,
and move-then-drop.

### Call-site heap-root borrows

A call site that borrows a heap root (a `uniq` receiver, or a field-rooted
`out`/`inout` argument) must hold the count across exactly the call, on both the
normal and the exception path, without disturbing the owner's own cleanup. The
mechanism, around the call:

```
PinnedCopy → RefInc → Call → RefDec → Nullify
```

- The borrow rides a `Copy` of the receiver/root flagged `no_copy_prop`, which
  copy-propagation leaves intact. This gives the call-straddling borrow a **distinct
  SSA value** (hence a distinct register, since its live range overlaps the
  receiver's), so its `RefDec` + `Nullify` cleanup cannot clobber the owned local's
  or temp's own `Delete` record.
- The exception-path `RefDec` record is **deferred and appended after all
  owned-local records** (`m_call_borrow_cleanups`, flushed in `end_function_body`),
  so reverse-order unwind releases the borrow *before* the owner's `Delete` (else
  the `Delete` sees `ref_count != 0` and spuriously traps). Lowering narrows the
  record to the `[RefInc-pc, RefDec-pc)` window.

This is what makes the "owner killed mid-call through an alias" guarantee hold for
every `uniq` receiver shape and every field-rooted `out`/`inout` argument (see
[Applying the model](#applying-the-model)).

### Interior pointers

A `borrowed` subscript or a `[ref self]` promotion can target an inline value-struct
field of a heap object, so the count must be reachable from an *interior* pointer.
`resolve_header(ptr)` goes through the allocator's sorted slab-range index
(`find_slab_containing` + rounding down by `slot_size`) to find the owning slot's
header; large objects use the same range index in place of a base-keyed map.

## Promotion

`self` is second-class, but `[ref self]` / `[weak self]` capture — and binding `self`
into a first-class `ref` — must turn it into a counted/generational reference, which
is legal only if the receiver is actually on the heap. This is the **one** place
storage isn't known statically (a method doesn't know whether it was called on a
stack or heap receiver), so it is the **one** runtime test: the slab-range check
(`AssertHeap`). On the heap → capture the count (or generation) and produce a
first-class reference. On the stack → **trap** ("cannot retain a borrow of stack
data — copy it, or allocate the receiver with `uniq`").

Promotion is wired for every shape:

- **`[ref self]` capture** — the `ref` capture is `RefInc`'d at env construction
  (after the heap check, so a stack receiver still traps before the inc) and the
  env's destructor `RefDec`s it.
- **Binding / returning / storing `self`** (`var r: ref T = self`, `return self`,
  `r = self`) routes the borrow inc through `emit_ref_borrow_inc`, which inserts the
  `AssertHeap(self)` gate before the inc whenever the source is a bare `self`.
- **Passing `self` to a `ref` parameter** (`f(self)`) is gated at the call site
  (`lower_call_args`), because the unsound inc is the callee's ref-param entry inc —
  so a stack receiver must trap *before* the call.

The promotion gate fires for *every* bare-`self` source (a method's receiver storage
is never known at its compile site), so it correctly traps a **noncopyable** stack
value-struct receiver as well. Inside a lambda body, `self` is already rewritten to
`__env.__self` (sourced from a heap-checked env), so only the bare `self` of a direct
method body reaches the gate. Where the analysis *does* know the receiver's storage,
the test could fold to an unconditional inc or a compile error; that folding is a
[future optimization](#limitations-and-future-directions).

Promotion depends on closures cleaning up their envs correctly. Because a closure
flows through the uniform `fun() -> R` type — which erases *which* env struct it is —
an env's cleanup can't be resolved statically at the delete site
(`var g = makeClosure()`). It is dispatched virtual-destructor style: a synthesized
destructor is built per env struct and looked up by the env's runtime `type_id` on
delete (`RoxyVM::closure_env_dtors`, `BCDeleteDesc::Closure`). This both frees
captured `[move uniq]` values and `RefDec`s captured `ref`s.

## Weak references and generations

`weak` is the **only** consumer of generational references. A `weak T` is
`{ptr, generation}` (4 slots), captured via `WeakCreate`, validated on use by
`WeakCheck` / `roxy_weak_valid`, and yields null / false when the referent is
tombstoned or recycled. Taking, copying, or dropping a `weak` needs **zero
bookkeeping** — a snapshot on create, a no-op on drop — and the owner's memory
**frees immediately** on delete.

### Why generational

Those two properties (zero-bookkeeping weaks copied freely as values; immediate
free) are exactly what a no-GC, churny-allocation, value-semantic runtime needs. The
alternatives each give one of them up:

| Approach | Why rejected for Roxy |
|---|---|
| **Auxiliary weak count + deferred free** (`shared_ptr`/`weak_ptr` style) | Reintroduces all-paths inc/dec on the *most* casually-used reference — the exact fragility just removed from `ref` — and a forgotten weak pins the dead object's slot, which creeps in long-lived weak registries with no GC to reclaim it. Also fights the slab's slot recycling. |
| **No slot reuse + liveness bit** | No ABA → no generation and a 2-slot `{ptr}` weak, but only by abandoning the free-list recycling the slab uses to fight fragmentation. Trades a tiny collision probability for unbounded slot retention under churn. |
| **Intrusive back-list** (object nulls its weaks on death) | Requires every `weak` to live at a stable, registered address; Roxy weaks are values copied through registers and realloc'd containers. Fundamentally incompatible. |
| **Page-protection / fault-on-use** | Page granularity is absurd for ~32-byte objects, and a fault is a crash, not the graceful null-on-test a `weak` must give. |

Generations cost only header bytes. The width is a **random 64-bit** value: 2⁻⁶⁴
collision probability per slot recycle, and resistance to deliberate reuse attacks
from untrusted embedded scripts. A 32-bit generation would shrink the header but
weaken a *correctness* property, and the object header isn't on a hot enough
per-object path to justify it — so the header stays
`{ u64 weak_generation, u32 ref_count, u32 type_id }` = 16 bytes, and a `weak` stays
4 slots.

## Applying the model

How the count interacts with each language feature.

### Methods and `self`

`self` is a second-class receiver borrow. A method call on a **statically-heap**
receiver counts that receiver for the call (so an alias-kill of it mid-method traps);
on a **stack** receiver it counts nothing (downward-safe); on an **already-`ref`**
receiver the existing count covers it. Binding / returning / storing `self`, or
passing it to a `ref` param, is a [promotion](#promotion).

The receiver borrow fires for every `uniq` receiver shape, using the
[call-site heap-root mechanism](#counting-mechanics):

- a **bare identifier** (`c.method()`);
- a **`uniq` field root** (`o.inner.method()`) — the borrow lands on the receiver
  object itself, which a `delete o` would also try to free, so it traps;
- a **heap-returning temp** (`make().method()`) — counted distinctly from the temp's
  own scope-exit `Delete` via the pinned copy.

Stack and `ref` receivers are correctly skipped.

### Closures

Captures are by copy. Capturing a first-class `ref` copies it → increments; the env's
destructor decrements. `[ref self]` / `[weak self]` are [promotions](#promotion): the
slab-range test runs at capture, trapping on stack receivers.

### Coroutines

A coroutine's parameters and promoted locals live in its heap state struct, so a
`ref` *parameter* of a coroutine is a first-class counted borrow held for the **state
struct's lifetime** — `ref_inc` when stored into the state at creation
(`init_func`), `ref_dec` in the generated `$$delete`. Deleting the borrowed owner
while a suspended coroutine still holds the borrow traps — even before the first
resume.

The per-frame resume-flow inc/dec are *suppressed* for coroutine functions
(`m_ref_params` cleared in the IR builder), because the coroutine split scatters the
entry-inc / exit-dec across resume states and would miss the dec on early destroy.
Counting for the state struct's lifetime instead keeps the balance whether the coro
runs to completion or is destroyed mid-iteration. A `ref` *local* is decremented on
whichever path comes first — its scope exit if the coroutine gets that far, else
`$$delete` — the resume path clearing its field so the two cannot both fire.

A catch param `e` is a `ref` field too, but is never *counted*: it is set by
exception dispatch, and what it holds is an owned object (below). It is therefore
excluded from the `ref_dec` and freed instead — as `uniq E` for a typed catch, or
type-erased for a catch-all — again under the field-clearing rule, since the catch
scope frees it on the resume path.

### Caught exceptions

A thrown exception is a heap object the catch does not *borrow* but *owns*: it is
registered as an owned local of the catch scope, so scope cleanup frees it once on
every exit, and a re-throw hands it off (guarded against the in-flight exception in
the free path) rather than freeing it. A coroutine suspended inside the catch
inherits that obligation through its state field (see [Coroutines](#coroutines)).
This is ownership/RAII, not counting — see
[exceptions.md](exceptions.md) "Exception object lifetime" for the full model.

### Containers are move-only

A `List<T>` / `Map<K,V>` owns a heap buffer, so — like `uniq` — it is **noncopyable
regardless of element type**: passing it by value moves it, and the source can't be
used afterward. An explicit `.copy()` deep-copies when an independent duplicate is
genuinely wanted. The callee-side value-param deep-copy (`lowering.cpp`) is skipped
for noncopyable containers, so a value param is a true move, not a move-then-copy.
(This also removed an old leak: copyable containers used to be deep-copied on
value-pass but never destroyed.)

### Containers are borrowable

A container value *is* the pointer to its slab-allocated header, so borrowing one
is `uniq → ref` with a different pointee: same thin pointer, same
`ObjectHeader.ref_count`, same free-trap, and — because a container is *always*
heap — no [promotion](#promotion) gate. `ref List<T>` / `ref Map<K,V>` is therefore
an ordinary first-class counted borrow, and `List<T> → ref List<T>` is an implicit
conversion at any typed site (`can_convert_ref`), exactly like `uniq T → ref T`. It
needs no call-site marker.

```roxy
fun total(xs: ref List<i32>): i32 { ... }   // borrows; caller keeps its list
fun consume(xs: List<i32>): i32 { ... }     // moves; caller's list is gone
```

This is what a **read-only** container parameter should be. Before it existed the
only non-owning option was `inout`, which forced a function that merely reads its
argument to advertise mutation, and — being second-class and exclusive — forbade
passing the same container twice (`compare(xs, xs)`).

`ref` is a borrow, **not an immutable borrow**: it names the object, so mutating
through it (`xs.push(1)`) is legitimate, consistent with `ref` on a struct. What it
cannot do is reassign the caller's *slot* — that is what `inout` is for, and it is
why `ref → inout` is rejected. Conversely a borrow cannot be moved out of the
borrowing frame (`return xs;` from a `ref List<i32>` parameter is an error);
`.copy()` is the way to leave with an independent owner.

Counting is the generic `ref`-parameter machinery — `RefInc` at entry, `RefDec` on
every exit path including exception unwind — so nothing container-specific tracks
it. An `inout` container may also be narrowed to a `ref` on the way down, which is
sound for the same reason the whole feature is: the pointee is heap either way.

Two consequences worth naming, both inherited from `ref` rather than new:

- A `ref` element borrow and a container borrow are **separate counters**: the
  borrow takes `ref_count` (blocking free), while `inout xs[i]` takes the
  container's `borrow_count` (blocking realloc). See
  [Container element lvalues](#container-element-lvalues).
- Rebinding a `ref` binding to a fresh owner (`fun f(r: ref List<i32>) { r = List<i32>(); }`)
  type-checks and then fails at runtime. That is a pre-existing hole in `ref`
  rebinding generally — `uniq` behaves identically — not something containers
  introduced; see `TODO.md`.

### Containers of borrows hold counted borrows

`List<ref T>` and `Map<_, ref V>` count their borrowed elements: acquiring a borrow
`RefInc`s the pointee, and every release path `RefDec`s it. Deleting an owner while a
container still borrows it traps.

- **`List<ref T>`** — `push` acquires, `pop` hands the count off to the caller (the
  ref-return-adopt convention), overwrite (`refs[i] = x`) releases-old /
  acquires-new.
- **`Map<_, ref V>`** — `insert` acquires (and on a replacing insert, releases the
  old value first), `remove` releases the removed value, `clear` releases all. The
  mutator path is runtime-side: a `value_is_ref` flag on the map header (set by
  `roxy_map_mark_ref_values`, emitted right after a `Map<_, ref V>` is constructed)
  gates the `RefInc`/`RefDec` in `roxy_map_insert` / `remove` / `clear`, and
  `roxy_map_copy` re-`RefInc`s each copied borrow.
- **Destroy** (both containers) `RefDec`s each `ref` element via a
  `BCDeleteDesc::RefDec` element descriptor — `delete_slot_entry` reads the borrowed
  pointer and `ref_dec`s it; the C-emitter's `emit_delete_slot` emits
  `roxy_ref_dec`.

### Containers of owners destroy their owners

`Map.remove` / `Map.clear` / an insert-replace also destroy **noncopyable (`uniq`)**
values. Rather than a runtime per-value destructor callback, the cleanup is emitted
as ordinary IR at the call site, where the value type is statically known, so both
backends get it for free:

- `m.remove(k)` emits a `contains`-guarded `delete m[k]` before the raw remove.
- `m.clear()` emits a bucket-iteration loop (via the `__map_iter_*` natives) deleting
  each value before the raw clear.
- `m.insert(k, v)` replace destroys the old value too: the value-arg consume is
  *deferred* past the `contains`-guard, so a replaced `uniq` value is freed and the
  incoming temp is consumed in the right order.

### Reassignment and overwrite cleanup

An overwrite must, like a field assignment, destroy what it replaces and consume what
it stores:

- **`container[i] = v`** (`gen_assign_index`) — for a noncopyable element/value type,
  destroy the overwritten element before storing (unconditional for a List; the index
  is always in bounds — `contains`-guarded for a Map — an old value exists only for a
  present key), then consume the right-hand temporary so its scope-exit delete is
  suppressed. The consume keys off the *container's* element/value type.
- **`slot = uniq T(..)` / `slot = nil`** through an `inout`/`out` owning
  (`uniq`/`List`/`Map`/`Coro`) pointer — load and `Delete` the old value before the
  store, and consume the RHS temp. (Without this the old object leaks and the new one
  is double-owned.)

### `borrowed T`

A subscript on a **heap-pointee** element (`List<uniq T>`) yields a counted `ref` to
the pointee — realloc moves the buffer, not the pointee, so the borrow stays valid. A
subscript on an **inline** element (`List<Vec2>`) is a second-class borrow (the
buffer has no per-element header to count): expression-scoped, and the container may
not be mutated while it is live (realloc would move the element). See
[RAII, moves, and `borrowed`](#raii-moves-and-borrowed) for the type transform.

### `out` / `inout`

The second-class family alongside `self`. An argument rooted in a heap object
(`bump(inout b.a)`) counts that root for the call (using the
[call-site heap-root mechanism](#counting-mechanics)), so a mid-call alias-kill
traps; a stack-rooted argument counts nothing and is safe by downward flow.

- **Escape rule.** A noncopyable `out`/`inout` cannot be moved out of its frame —
  bind / return / store / by-value-pass / capture-by-move are rejected at compile
  time. (Copyable `out`/`inout` escapes were already blocked: there is no
  value→reference conversion.)
- **Root counting.** A field-rooted lvalue (`f(inout heap_obj.field)`,
  `f(inout a.b.c)`) borrows the innermost heap object it points into for the call
  (`heap_root_of_lvalue` walks the pure-path chain to that object). A `ref`-rooted
  lvalue is already covered by the `ref`'s own count; a bare-identifier lvalue roots
  in the caller's frame (no heap root to count).
- **Index-rooted lvalues** (`f(inout list[i])`) need more than a count — their
  element buffer lives outside the slab, so realloc, not just free, can dangle the
  pointer. See [Container element lvalues](#container-element-lvalues).

### FFI / AOT

A `ref T` passed to a native function is counted for the call's duration, so the
object **cannot be freed during the call**, even by reentrant Roxy code — the
native's raw pointer is guaranteed live. The runtime surface this rests on is the
free-trap in `roxy_free` and `resolve_header`; `weak`'s `{ptr, gen}` ABI is
unchanged.

### Move checker

`ref` is *copyable* (copy = inc), so it is not move-tracked. Instead, `ref` bindings
gain cleanup records that emit `RefDec` on all exit paths, reusing the
noncopyable-cleanup machinery.

## Container element lvalues

`f(inout list[i])` passes the **actual address** of the element in the backing
buffer, so the callee mutates it in place — no copy, true aliasing. (Copy-in /
copy-out — read the element to a stack slot, pass that, write it back — was rejected:
it is sound but is not a real lvalue, and a concurrent read during the call sees the
stale value.) This works on both backends for primitive, struct, *and* `uniq`
elements of both `List` and `Map`.

### The hazard

`list[i]`'s address points into the container's separately-`malloc`'d element buffer,
which lives **outside** the slab, so the header's generational / `ref_count`
machinery does not protect it. Three operations invalidate the address, and any can
happen mid-call if the callee reaches the container through another channel (a second
argument, a global):

| Mid-call operation | Effect on `&list[i]` |
|---|---|
| `delete list` | buffer freed → dangling |
| `list.push(x)` (grow) | buffer realloc'd / moved → dangling |
| `pop` / `remove` / `clear` | element gone / buffer freed → dangling |
| `list[j] = v` (in-place set) | **safe** — same slot, no move |

A count on the header blocks only the *free*; it does nothing about realloc. So a
count alone is not sufficient here.

### The pin

While `&list[i]` is outstanding, the container is frozen against exactly the
operations that move or free the buffer:

- **Free** is blocked by the ordinary constraint-reference free-trap: the element
  borrow takes a `ref_count` on the container object, so `delete`-while-borrowed
  traps.
- **Structural mutation** (realloc / shrink / element removal) is blocked by a
  separate **`borrow_count`** in the container header. Structural mutators trap while
  `borrow_count > 0`.

The mutation guard uses a *separate* `borrow_count` rather than reusing `ref_count`
because `fill(r: ref List<i32>) { r.push(1) }` is legitimate — but `r` is a `ref`
parameter whose entry `RefInc` makes `ref_count > 0`, and a `ref_count`-based push
trap would wrongly reject it. Mutation-blocking must be scoped to *element* borrows
only; free-blocking can safely reuse `ref_count`.

The pieces:

1. A `borrow_count` in the shared `roxy_list_header` / `roxy_map_header` (absorbed
   into existing header padding — no size change).
2. **Mutation guards** in the shared runtime (`roxy_rt.cpp`): `roxy_list_push` /
   `pop`, `roxy_map_insert` / `remove` / `clear` raise a *fatal, non-catchable*
   runtime trap (a thread-local channel distinct from catchable user exceptions)
   while `borrow_count > 0`, leaving the buffer untouched so the borrowed pointer
   can't dangle. In-place `set` and all reads stay allowed. The VM routes
   `list.push` → `roxy_list_push`, so **one guard per op covers both backends**.
3. **`INDEX_ADDR_LIST` / `INDEX_ADDR_MAP`** — a bounds-/key-checked element-address
   op (`IROp::IndexAddr`), mirroring `IndexGet` but storing the *pointer* rather than
   the loaded value. `gen_lvalue_addr`'s `ExprIndex` case emits it; no reload is
   needed (the address *is* the storage).
4. **Pin/unpin around the call** — `ContainerPin` / `ContainerUnpin` ops on a pinned
   copy of the container (`borrow_count++` before the call, `--` after and on unwind,
   via a deferred `IRCleanupKind::Unpin` record narrowed to the call window), plus
   the ordinary container `ref_count` free-trap borrow.

### Soundness

| Mid-call event | Outcome |
|---|---|
| `delete` the container | `ref_count` free-trap |
| `push` / `insert` (realloc) | `borrow_count` mutation-trap |
| `pop` / `remove` / `clear` | `borrow_count` mutation-trap |
| in-place `list[j] = v` | allowed (valid slot) |
| free + slot recycled into a new container | impossible — the free is trapped first |

**Owning elements** (`List<uniq T>` / `Map<K, uniq V>`): an `inout`/`out` subscript
re-types to the raw element type (`uniq T`), not the `borrowed` read view (`ref T`),
so the callee gets reassignable access to the owning slot. The in-place reassign
frees the old pointee, and the escape rule forbids moving the element out of the
frame, so the container still owns exactly one value per slot at delete time — no
double-free, no leak.

The rule this surfaces: **you cannot structurally mutate a container while an element
of it is borrowed (`inout` / `out`).** The pin is per-container (coarse — borrowing
one element freezes the whole container's structure), which is simple and
sufficient.

## Runtime foundations

The model above rests on a few facts about how heap objects are laid out, allocated,
and freed.

### Object header

Every heap-allocated object is prefixed by a 16-byte `ObjectHeader`: a 64-bit random
`weak_generation` (0 = dead/tombstoned), a `ref_count` of active `ref` borrows, and a
`type_id` for runtime type info. `is_alive()` is `weak_generation != 0`. The unified
definition lives in `roxy_rt.h` as `roxy_object_header`. The `ref_count` is the count
[the constraint-reference model](#constraint-references) maintains and
[the free-trap](#counting-mechanics) checks; the `weak_generation` is what `weak`
validates against.

### Slab allocator

Heap objects are allocated from fixed-size slabs chosen by object size:

| Class | Slot size | | Class | Slot size |
|---|---|---|---|---|
| 0 | 32 B | | 4 | 512 B |
| 1 | 64 B | | 5 | 1024 B |
| 2 | 128 B | | 6 | 2048 B |
| 3 | 256 B | | 7 | 4096 B |
| | | | 8+ | large (multiple pages) |

The allocator backs slabs with platform virtual-memory operations
(`reserve`/`commit`/`decommit`/`release`/`remap_to_zero`/`page_size`; see
`rt/vmem.hpp`). It lives in `roxy_rt` and is shared by both backends: VM mode plugs a
per-VM `SlabAllocator` into `roxy_ctx.allocator` via a vtable; AOT mode uses a
process-wide slab created by `roxy_rt_init`. Both get identical generation-based
weak-ref soundness; a malloc fallback applies only when no ctx is active.

### Tombstoning and recycling

When an object is freed (the path the free-trap guards):

1. The whole slot (header + data) is zeroed, so `weak_generation` reads 0 and
   `is_alive()` is false.
2. The slot is pushed onto its slab's intrusive free list for the next allocation in
   that size class. The next-pointer sits past the header, so `weak_generation` keeps
   reading zero while the slot is parked.
3. Memory stays mapped, so weak references can keep dereferencing safely — they see
   `is_alive() == false` until the slot is re-allocated.

Stale-weak safety after recycle comes from the random generation: a reused slot gets
a fresh random `weak_generation`, so any weak holding the old one mismatches
(collision probability 2⁻⁶⁴ per recycle).

### Slab reclamation

Recycling solves slot-level fragmentation, but a slab whose live set has drained to
zero still holds physical memory. `reclaim_tombstoned()` scans slabs and, for each
drained one (`live_count == 0`), calls `remap_to_zero()` over the whole slab
(releases physical pages, keeps the vaddr mapped as zeros), sets
`free_head = 0xFFFFFFFF` so no further slots are handed out, and marks it `remapped`
(idempotent across passes).

### Random generational references

`weak` validates against the header's 64-bit random `weak_generation`
([Weak references and generations](#weak-references-and-generations) covers why
random and why 64-bit). The PRNG is xorshift128+ (`RandomGen`, seeded via
SplitMix64). `weak_ref_valid(data, generation)` returns false on null; otherwise it
reads the header (always safe — memory stays mapped whether alive or tombstoned) and
returns `is_alive() && weak_generation == generation`.

## RAII, moves, and `borrowed`

### Implicit destruction (RAII)

`uniq` variables, value-structs with destructors, and noncopyable containers are
cleaned up automatically at scope exit — no manual `delete` in most code. At every
exit point the compiler emits cleanup for the live noncopyable locals of that scope,
in **LIFO** (reverse-declaration) order:

| Exit point | What's cleaned up |
|---|---|
| End of block `{ … }` | locals declared in that block |
| `return` | all locals in all enclosing scopes |
| `break` / `continue` | locals in scopes inside the loop / loop body |
| End of function | all function-scope locals |

A destructor (`fun delete T()`) runs before the memory is freed; a noncopyable
container runs a per-element cleanup loop before freeing its buffers and header (see
[list.md](list.md), [maps.md](maps.md)). Deleting a null pointer is a safe no-op, so
`var x: uniq T = nil;` and moved-out (null-ified) variables never double-free.

### Move semantics

Binding / passing / returning a noncopyable value of matching type **moves**
ownership; the source becomes invalid. This applies to `uniq`, value-structs with
destructors, and noncopyable containers.

- Pass to a matching parameter → ownership transfers, source consumed.
- Return → ownership transfers to the caller, no scope-exit delete.
- `var copy = items` → ownership transfers (no implicit deep copy; use `.copy()` for
  an independent duplicate).
- Explicit `delete` → consumed.
- Reassigning → the old value is destroyed before the new one is stored.

**Moving a field out.** A noncopyable *pointer* field (`uniq`/`List`/`Map`/…) may be
moved out of a local value struct (`var x = o.field`, `f(o.field)`,
`return o.field`, `Foo { x = o.field }`, `y = o.field`); the compiler nulls that
field in the root at the move site, so the root's destructor no-ops it and still
frees the surviving siblings (no double-free, no leak). For use-checking, the *whole*
root is conservatively marked moved (siblings can't be read afterward; per-field move
state is not tracked). Moving a noncopyable *value-struct* field out is a compile
error — borrow it with `ref`, make it `uniq`, or move the whole struct.

### Use-after-move detection

The semantic analyzer's `LifetimeChecker` (`compiler/sema/lifetime_checker.hpp`) tracks
a move state per noncopyable local; using a `Moved` or `MaybeValid` variable is a
compile error:

| State | Meaning |
|---|---|
| `Live` | owns a valid value |
| `Moved` | ownership transferred — use is an error |
| `MaybeValid` | conditionally moved (e.g. moved in one `if` branch only) |

**Who destroys a `MaybeValid` value.** Rejecting every later *read* is what makes
this answerable without a runtime drop flag: the value is dead at the merge no
matter which path ran, so the branches that did **not** move it destroy it where
they leave (`IRBuilder::reconcile_divergent_moves`). Its lifetime therefore ends
at the branch construct rather than at scope exit — an early drop, never a
missing or a doubled one.

The merge cannot instead pick one answer for the shared move flag, because
either choice is wrong for half of its predecessors: "moved" leaks on the paths
that still hold the value, "not moved" double-frees on the paths that already
gave it away. Both were reachable — a plain `if` took the first and leaked, an
`if/else if` chain took the second and double-freed. Where the non-moving path
is an implicit fall-through edge with no block of its own, one is materialized
to hold the drop.

**Alternative paths must also start from the same move state.** `if`, the
`if/else if` chain, and `when` all snapshot it before the first branch and
restore it per branch, because a branch that destroys a local marks it moved and
that flag would otherwise carry into its siblings, which then skip their own
destroy. `when` did not, and leaked whichever arm ran second. Two edges are
exempt: an exhaustive `when`'s *trapping* else (unreachable, runs no body, so
rolling back to pre-when would resurrect what every real arm moved), and `catch`
(see below) — both are cases where the "sibling branch" framing does not hold.

`try`/`catch` is the deliberate exception: a catch clause does **not** restore
the move state. It is not an alternative to the try body from a common start —
it runs *after* part of that body, so rolling the flag back would re-enable the
implicit destroy in `r = uniq T()` for a `uniq` the try body already consumed,
double-freeing a dead slot. Use-after-move in a catch is the semantic analyzer's
job to reject, not the IR builder's to repair.

### The `borrowed` type modifier

`borrowed T` is a **resolve-time type transform** that demotes an owning type to a
borrow — it lets a built-in container accessor express "I return a *view*, not
ownership" in a signature whose element type is not known until monomorphization,
where returning an owning `uniq T` by alias would double-free.

> **Not user-facing syntax.** `borrowed` is recognized **only while parsing a
> native binding signature** (`Parser::set_native_signature_mode`, set by
> `NativeRegistry::parse_signature`); in user source it is an ordinary
> identifier, usable as a variable, function, or type name. It was briefly
> accepted in any type position, which made it a front-end divergence — the
> LSP's error-recovering parser never recognized it, so a program using it
> compiled but did not analyze in the IDE — and no program ever had a reason to
> write it, since `borrowed uniq T` is just `ref T` spelled longer. Its three
> consumers are the whole surface: `List<T>.index`, `Map<K, V>.index`, and
> `Map<K, V>.get`.

| `borrowed X` | → | rationale |
|---|---|---|
| `uniq T` | `ref T` | borrow the heap pointee instead of transferring it |
| `fun(…) -> R` | `ref fun(…) -> R` | a closure is a heap env pointer; `ref fun` shares its representation and is callable |
| copyable `T` | `T` | a copy aliases nothing |
| `ref T` / `weak T` | unchanged | already a borrow |
| other noncopyable (value struct, coro, `List`/`Map`) | unchanged | identity (see below) |

Within a native signature `borrowed` is a **contextual keyword**, never reserved;
it never persists as a `Type` — resolution maps it to a concrete type and rides on
`TypeExpr::is_borrowed` through generic substitution, so `borrowed T` resolves per
monomorphization. The native `List`/`Map` `index` (and `Map.get`) are typed
`borrowed T` / `borrowed V`, so `var x: uniq Point = list[i]` is a plain `ref → uniq`
type error.

**Callable borrows.** A `ref fun` / `weak fun` borrows a closure value (a heap-env
pointer with a header) and, sharing `fun`'s representation, is callable: the call
paths unwrap the borrow via `base_type()` before reading the call index. So
`List<fun>` indexing yields a callable, storable `ref fun`. A bare `fun` also
converts to `ref fun` / `weak fun` (`fun → weak fun` via `WeakCreate`).

For the remaining noncopyable kinds (inline value structs, coroutines, `List`/`Map`),
`borrowed` is the identity, and the lifetime checker's native-index guard
(`LifetimeChecker::consume_noncopyable`) is the backstop that rejects only the unsound *move-out* of
those while leaving every safe use (storage, per-element cleanup, in-place field
reads / method calls) intact. An inline value struct *can't* be borrowed out (no
header) but doesn't need to be; coroutines and noncopyable containers could later
demote to `ref` once their `ref`-receiver dispatch lands.

## Lifecycle implementation and status

### One derivation, two executions

`compute_drop_plan(Type) -> DropPlan` (`types.cpp`) decides the *kind* of drop
once — `DropKind` (None / CallDtor / WalkFields / List / Map / Closure / RefDec /
StrRelease) plus `free_obj` and the involved types — and **both backends lower
the same plan**:

- **VM** keeps its **native** `delete_value` walk over `BCDeleteDesc` — that *is*
  the VM's drop-glue executor; nothing is inlined per site, so there is nothing
  to "factor out", and emitting interpreted bytecode glue would be *slower*.
- **AOT/C** lowers the plan to generated `roxy_drop__<T>` glue functions (and a
  struct's `$$delete`), which the C compiler inlines and ICF-folds.

Each backend keeps the execution that is efficient for it; neither re-derives.
At a *true* erasure boundary — a closure env dropped by `type_id` — a single
drop-glue dispatch survives; that is one pointer for one operation, **not** a
per-operation vtable.

`member_needs_drop()` — "does a value in an opaque member slot (struct field,
container element) need cleanup?" — **derives** from `compute_drop_plan`, so the
eligibility gate and the lowering it gates cannot disagree. It is deliberately
non-recursive: a nested value struct that owns something carries its own
synthesized destructor, propagated by the synthetic-destructor fixpoint.

### What is actually implemented

| Property | Derivation | Consumed by | Status |
|---|---|---|---|
| **Drop** | `compute_drop_plan` | both backends, `member_needs_drop` | ✅ complete — **except** `StrRelease` on a struct field, gated off (below) |
| **Retain** | `compute_retain_plan` | *nobody yet* | ⚠️ **derived but unwired.** The plan landed 2026-08-02 and is pinned by tests; no codegen consumes it, so retains are still emitted ad hoc per store site and struct copies emit none |
| **Move-only** | "struct has a default destructor" | move checker, call/return lowering | ⚠️ **mis-derived** — see below |

The ad-hoc retain sites that *do* exist and are correct: binding a `string`
local, storing one into a struct field (retains the new value and releases the
overwritten one), and pushing one into a container. What has no retain at all is
**duplicating a whole struct** — `IROp::StructCopy` copies the bytes and nothing
else.

`Type::needs_retain()` and `Type::is_trivial()` are **dead predicates**: they
compute the right answer and nobody asks. `compute_retain_plan` supersedes them
as the derivation codegen will consume; the predicates remain only as the
`is_trivial` convenience the emitter may want later.

### The gap: Drop and Copy are one bit

`noncopyable()` on a struct means literally *"has a default destructor"*. So the
moment a struct earns drop glue it also becomes move-only. That has held up only
because the two sets coincide: the members that earn a synthetic destructor
(`uniq`, `List`, `Map`, `Coro`, closures, `ref`) are exactly the move-only ones.

`string` and `ref` are the rows in
[the lifecycle table](#every-type-kind) that break the coincidence, and the one
bit gives the **wrong answer in both directions** — both reproducible today:

| Field | Today | Should be | Symptom |
|---|---|---|---|
| `s: string` | struct earns no drop, stays copyable | drop **and** retain-on-copy | **leaks the string** |
| `r: ref T` | struct earns drop → forced move-only | copyable, `ref_inc` on copy | `var b = a;` rejected as *"use of moved value"* |

```roxy
struct Box { s: string; }
var d: string = a + "y";
var b: Box = Box { s = d };   // leaks: retained on store, never released

struct Holder { r: ref Point; }
var h2: Holder = h;           // rejected — but a copy is just another borrow
```

`string`'s drop is therefore **knowingly skipped**: `member_needs_drop` excludes
`DropKind::StrRelease`, because enabling it alone makes string-bearing structs
move-only (measured: four `Structured Gen` cases start failing), and making them
copyable *without* retain glue is worse than the leak — two owners, two releases,
use-after-free.

### Separating Drop from Copy ✅ *(landed 2026-08-02)*


Three changes, in this order. The order matters: making `ref`-bearing structs
copyable before the glue that keeps their counts balanced exists would trade an
over-restriction for a use-after-free. Steps 1 and 2 have landed; step 3 is
partly done — see "What remains" below.

**1. A retain derivation, mirroring the drop plan.** ✅ **Landed** —
`compute_retain_plan(Type) -> RetainPlan` in `types.cpp`, beside
`compute_drop_plan`, pinned by the `Lifecycle Predicates` suite:

```
enum class RetainKind { None, StrRetain, RefInc, WalkFields };
```

`string` → `StrRetain`, `ref` → `RefInc`, a copyable struct with any retaining
field → `WalkFields`, everything else (including every move-only kind, which is
never implicitly duplicated) → `None`. Consumed by `emit_value_retain` /
`emit_struct_clone_glue`, and by the `member_needs_retain` predicate that gates
container acquisition.

**2. Move-only becomes structural.** ✅ **Landed** — an `is_move_only` flag on
`StructTypeInfo`, filled by `derive_struct_move_only` in a whole-program fixpoint
(`SemanticAnalyzer::derive_move_only_flags`) that runs beside — and before — the
synthetic-destructor fixpoint:

```
is_move_only(S) = S has a USER-WRITTEN default destructor
               || ∃ field f : is_move_only_type(f.type)

is_move_only_type(T) = Uniq | List | Map | Coroutine | Function
                     | (Struct && is_move_only(T))
```

`noncopyable()`'s struct arm reads the flag instead of scanning destructors.
`string`, `ref`, `weak`, primitives and enums are not move-only. This is the
step that makes `ref`-bearing structs copyable — a **user-visible language
change**, correct but not a silent one, and the one behavioural difference it
produced is pinned in `E2E Lifetimes`: passing such a struct by value no longer
ends the caller's borrow, so deleting the owner while a borrowing struct is live
now traps (correctly) instead of silently dropping a reachable borrow.

The flag is *precomputed*, and `noncopyable()` **asserts** it has been derived
rather than reading a default-initialized `false`. That assert is load-bearing:
it is what found the two ordering gaps (`resolve_global_var` asking before the
derivation ran; native structs never deriving at all) instead of letting them
silently report a move-only struct as copyable. `resolve_type_members` is now
split into three phases for it — shape, derive, everything that may ask.

**3. Clone glue at the duplication sites, and enable `StrRelease`.** ✅
**Landed.** The glue (`emit_struct_clone_glue` / `emit_value_retain`) is keyed on
`member_needs_drop`, so acquisition and release are inverses by construction, and
the carve-out in that predicate is gone. A `string` struct field is now released;
`examples/lox`'s census went from 38 leaked strings to 0.

### The ordering constraint

Each change is unbalanced *on its own*, which is what dictates the order:

| Landed alone | Result |
|---|---|
| retain glue | retains with no matching release — the imbalance grows, and the teardown census cannot see it (same object, higher count) |
| `StrRelease` in the gate | string-bearing structs earn a destructor → become **move-only** (measured: four `Structured Gen` cases fail) |
| structural move-only | `ref`-bearing structs become copyable while nothing balances their counts → **use-after-free** |

An earlier revision concluded from this that steps 2 and 3 had to land as one
change. **That is no longer true, and the reason is worth keeping**: the row for
structural move-only says "nothing balances their counts", which stopped being
the case once the clone glue landed. With the glue in place, a `ref`-bearing
struct that becomes copyable gets a `RefInc` at every duplication, so step 2 was
safe on its own — verified across the whole suite on both backends plus every
example, with Lox's census unchanged.

The general form: **a step is safe alone once its counterpart exists**, not
because of where it sits in the list. Re-derive the row rather than trusting it.

Only the full combination is sound overall: the struct earns a drop, stays
copyable, and every duplication retains.

### The container side

Flipping the gate also makes **containers** release counted keys and values on
teardown, so each store has to acquire.

- `List.push` acquires through `emit_value_retain`, generalized from the
  `string`-only case.
- The map value store — `m.insert(k, v)` and `m[k] = v`, which are the same
  operation — goes through `emit_map_value_ownership`: acquire for the slot
  unconditionally, and release whatever the slot held before. The release is
  `contains`-guarded, since a new key replaces nothing. `clear()` and `remove()`
  release on the same gate, so discarding an entry costs exactly what destroying
  the container would.
- `values()` and `copy()` hand back a container that **shares** the original's
  elements — both natives memcpy the slots — so the new one acquires its own
  counts through a retain loop (`emit_list_elements_retain` /
  `emit_map_values_retain`). Without it the two containers release what one of
  them took, and the element dies under whichever outlives the other.

A **struct literal used inline** is an owner too, which is easy to miss because
it has no name. Its field stores acquire (a `string` field retains), and the
storage they land in is a bare stack allocation — so it is tracked for cleanup
like any other temporary. Bound to a variable it is adopted; as an argument
(`xs.push(S { s = f"..." })`) its counts previously had nowhere to go. Only
*copyable value* literals need this: a `uniq S { ... }` is a heap object that
self-tracks at creation, and a move-only literal's contents are transferred by
the move machinery.

Map **keys** are counted too, but in the *runtime* rather than in emitted IR —
`roxy_map_insert` acquires, `roxy_map_remove` / `roxy_map_clear` release, and
`roxy_map_keys` acquires for the `List<K>` it hands back. The split is forced by
what each side can see: only the runtime knows *which* key is actually stored
(insert replaces in place and keeps the key it already has, so the incoming one
must not be retained, and remove must release the stored one rather than the
caller's equal-valued copy), while only the compiler knows enough about an
arbitrary value type to walk it. Keys are a closed set of runtime-known kinds
(`key_kind`), which is what makes the runtime side possible at all.

A **copyable struct key holding a counted member** is deliberately unhandled: the
runtime cannot walk it to acquire, and such a key could never match on lookup
anyway, since `map_keys_equal` compares key bytes and two equal strings at
different addresses miss. The teardown descriptor's key gate excludes it to
match — it drops a key that is move-only (moved in, so nothing was acquired) or a
`string` (counted), and nothing else.

### What the flip exposed in the unwind path

Turning a copyable struct into something with drop glue put **value structs**
into exception cleanup records for the first time, and that broke two assumptions
that had held only because every tracked local used to be pointer-shaped:

- **A cleanup record's start was the enclosing block's start**, which covers the
  *call* that initializes the local. Unwinding out of that call ran cleanup on a
  register that had not been written. Harmless for a pointer (registers start
  zeroed, `Delete` of null is a no-op); a value struct has no null form, so a
  stale register — a live loop counter, in the case that surfaced — was
  dereferenced as a struct. Records now carry `live_start_pc` separately from
  `scope_start_pc`: the throw test uses the former, the "is the handler in scope"
  test must keep using the latter, or narrowing the range lets a handler fall
  outside a scope it is really in and both paths clean up.
- **The `DELETE` opcode zeroed its operand register**, which is double-free
  protection for a freeing delete but pure clobber for an in-place one, where
  nothing is freed and the address stays valid. It zeroed a struct literal's
  storage out from under the copy that read it. It now nulls only when
  `free_obj`.

A third assumption fell later (2026-08-08, the last Lox leak): **a record's
single PC interval can cover a scope**. It cannot — RPO lays a throw-terminated
branch out *after* the scope's normal-exit block (the branch has no successors,
so it post-orders first and is placed last), and the interval, truncated at the
normal-path `Delete`'s `Nullify`, missed the throw entirely. An owned by-value
`List` parameter in `fun take(args) { if (…) { throw …; } return 0; }` leaked on
every throw. Nor could the interval simply be widened: SSA liveness considered
the value dead in the throwing block, so its register was already recycled there
(a record firing would have deleted a string constant as a list).

Lowering now computes each record's **coverage** — every block reachable from
the value's def without passing an ownership-ending kill (a `Nullify`, or the
record's own cleanup op), following exception edges into a handler only when the
handler's continuation demonstrably cleans the value up (or, for a rethrowing
`finally`, never exits normally) — then pins the value's register across the
whole coverage (liveness Pass 5b in `compute_liveness`) and emits **extension
records** (`BCCleanupRecord::is_extension`) for covered PC runs outside the main
interval. The unwinder evaluates a head record plus its extensions as one group:
throw-site and handler-in-scope tests consult every interval in the group, and
the action runs at most once. One subtlety carries the whole thing: a moved
argument's kill is anchored at the **consuming call's boundary** (the word after
the `CALL`), not at its `Nullify`, which is only lowered after the call's
return-value materialization — a throw escaping the callee surfaces at exactly
that boundary, when the callee already owns (and on unwind frees) the value, so
covering the gap double-freed it (`Interpreter.eval_call` did exactly this).
The C backend is untouched: it consumes `IRCleanupInfo` directly and replays
every record once from its `__unwind` label with null guards, which already
covers layout-independent unwinding.

### Move-only containers

**Move-only** is the `!is_copy()` case: a `List`/`Map` (it owns a heap buffer), a
struct with a `ref` field (today — see the gap above), and a coroutine with a
`ref` param are all move-only, and each counts the borrows it holds for its
lifetime. The per-feature mechanics live under
[Applying the model](#applying-the-model) —
[containers](#containers-are-move-only),
[their counted borrows](#containers-of-borrows-hold-counted-borrows), and
[coroutine `ref` params](#coroutines).

## Limitations and future directions

### Residual risks and sharp edges

- **Trapping during unwind.** In well-structured code, cleanup is LIFO, so a borrow
  declared after its owner is decremented before the owner is freed — no spurious
  trap. A genuine escape (a borrow stored in something longer-lived) freed during
  *exception* unwinding will trap mid-unwind; that is a real "cannot safely unwind"
  situation, and a clear trap beats a use-after-free. Debug builds should assert
  count balance at frame exit.
- **Completeness is the whole game.** A single missed decrement makes the owner
  permanently undeletable (a loud trap, not unsafety); a single un-trapped free path
  would be a use-after-free hole. Centralizing the trap in `object_free` and the
  decrements in the shared cleanup machinery is what makes completeness auditable.
- **Count under/overflow.** `ref_count` is `u32`. Underflow indicates an unbalanced
  dec and is a tripwire ("ref_dec: reference count already zero") in *both*
  runtimes — the VM sets `vm->error` (`object.cpp`), the AOT runtime asserts in
  debug and records the same message through the fatal trap channel in release
  (`roxy_ref_dec`); with complete balancing it never fires. Overflow is bounded by
  live-borrow count and not a practical concern.
- **Single-threaded.** Inc/dec are non-atomic, matching the VM's single-thread
  assumption. A threaded runtime would need atomic counts.
- **Malloc-fallback allocator** (AOT before `roxy_rt_init`): the slab is the
  supported configuration; `resolve_header` and the free-trap assume slab-backed
  objects.

### Future directions

- **Refcount elision** is a planned optimization phase that *removes* inc/dec pairs
  wherever the owner provably cannot be freed during the borrow's lifetime — no
  intervening `delete`, move, or call that could reach a free, with the owner's death
  sites all visible. The critical property: elision only ever removes
  *provably-redundant* counts, so conservative or incomplete elision yields slower
  code, never unsafe code — soundness never depends on the analysis being clever. The
  easiest wins are call-site receiver/arg counts where the heap root is a local the
  callee can't reach (the common `local.method()` case), which elide to nothing.
- **Folding the promotion gate.** Where the receiver's storage is statically known,
  the runtime `AssertHeap` ([Promotion](#promotion)) could fold to an unconditional
  inc or a compile error.
- **AOT trap reporting for container pins.** The
  [container element-lvalue](#container-element-lvalues) mutation guard refuses the
  unsafe op memory-safely on both backends, but the C backend's clean trap *report*
  (abort with a message) is part of the broader AOT-trap-reporting work, not yet
  done.
- **`roxy::ref<T>` as a borrow handle.** The AOT C++ wrapper should be a borrow
  handle — copy increments the borrowee's count, destruction decrements, and it never
  frees — matching the constraint-reference semantics (rather than shared ownership).

## Related docs

- [overview.md](../overview.md) — reference-type philosophy; the `out`/`inout`
  restrictions the second-class family shares.
- [methods.md](methods.md) — `self` as the receiver (second-class here).
- [closures.md](closures.md) — `self` capture modes; `AssertHeap` is the
  [promotion](#promotion) gate.
- [coroutines.md](coroutines.md) — the state-struct promotion referenced under
  [Applying the model](#applying-the-model).
- [list.md](list.md), [maps.md](maps.md) — container internals and per-element
  cleanup.
