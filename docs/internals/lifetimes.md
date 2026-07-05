# Memory, Lifetimes & Lifecycle

The single reference for Roxy's no-GC memory model: how heap objects are allocated
and freed, how `uniq` / `ref` / `weak` stay sound, and how values are dropped,
copied, and moved.

**In one sentence:** `ref` is a *constraint reference* — a borrow of a **heap**
object that increments a count in the object's header while it lives, and an object
cannot be freed, by any path, while that count is nonzero (the free traps); **stack**
value-structs are never borrowed with `ref` — they pass by reference only through the
second-class `out` / `inout` / `self` family, which flows downward and cannot escape;
and `weak` is the sole user of generational references.

**Reading guide:**

- [The three reference types](#the-three-reference-types) and
  [Constraint references](#constraint-references) — the core model.
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
- [RAII, moves, and `borrowed`](#raii-moves-and-borrowed) and
  [Value lifecycle: Drop, Clone, Copy](#value-lifecycle-drop-clone-copy) — the
  user-facing model and the unified, vtable-free lifecycle machinery.
- [Limitations and future directions](#limitations-and-future-directions).

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
`m_owned_locals` list as `uniq` locals, so they inherit LIFO scope cleanup and
exception records for free; a `BCCleanupKind::RefDec` cleanup record makes
`execute_cleanup` decrement rather than destroy. Two subtleties the bookkeeping must
get right:

- **`ref` parameters must decrement on exception unwind, not only at `return`.** A
  whole-function-scoped `RefDec` record per `ref` param, with that param's register
  liveness pinned to the function end, ensures the unwind decrement reads a valid
  register even on throw-only paths.
- **The return hand-off is a 1:1 transfer.** Every `ref` return carries *exactly
  one* count (a ref local hands off its create-inc; a ref param or a fresh ref —
  field / borrowed subscript / `ref x` — increments to produce one; a call result
  already carries one). The binder then **adopts** a call result (no inc) and
  **increments** any other still-live source. Otherwise a returned ref local bound
  by the caller would double-count — a safe over-count, but a spurious trap.

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
runs to completion or is destroyed mid-iteration. Only *parameter* `ref` fields are
decremented in `$$delete`; a catch param `e` (a `ref` field set by exception
dispatch, not acquired at creation) is deliberately excluded.

### Caught exceptions

A thrown exception is a heap object the catch does not *borrow* but *owns*: it is
registered as an owned local of the catch scope, so scope cleanup frees it once on
every exit, and a re-throw hands it off (guarded against the in-flight exception in
the free path) rather than freeing it. This is ownership/RAII, not counting — see
[exceptions.md](exceptions.md) "Exception object lifetime" for the full model.

### Containers are move-only

A `List<T>` / `Map<K,V>` owns a heap buffer, so — like `uniq` — it is **noncopyable
regardless of element type**: passing it by value moves it, and the source can't be
used afterward. An explicit `.copy()` deep-copies when an independent duplicate is
genuinely wanted. The callee-side value-param deep-copy (`lowering.cpp`) is skipped
for noncopyable containers, so a value param is a true move, not a move-then-copy.
(This also removed an old leak: copyable containers used to be deep-copied on
value-pass but never destroyed.)

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

The semantic analyzer's `LifetimeChecker` (`compiler/lifetime_checker.hpp`) tracks
a move state per noncopyable local; using a `Moved` or `MaybeValid` variable is a
compile error:

| State | Meaning |
|---|---|
| `Live` | owns a valid value |
| `Moved` | ownership transferred — use is an error |
| `MaybeValid` | conditionally moved (e.g. moved in one `if` branch only) |

### The `borrowed` type modifier

`borrowed T` is a **resolve-time type transform** that demotes an owning type to a
borrow — it lets a function or method express "I return a *view*, not ownership",
most importantly the container subscript, where returning an owning `uniq T` by alias
would double-free.

| `borrowed X` | → | rationale |
|---|---|---|
| `uniq T` | `ref T` | borrow the heap pointee instead of transferring it |
| `fun(…) -> R` | `ref fun(…) -> R` | a closure is a heap env pointer; `ref fun` shares its representation and is callable |
| copyable `T` | `T` | a copy aliases nothing |
| `ref T` / `weak T` | unchanged | already a borrow |
| other noncopyable (value struct, coro, `List`/`Map`) | unchanged | identity (see below) |

`borrowed` is a **soft keyword** (type position only; usable as an identifier
elsewhere); it never persists as a `Type` — resolution maps it to a concrete type and
rides on `TypeExpr` through generic substitution, so `borrowed T` resolves per
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

## Value lifecycle: Drop, Clone, Copy

A unified, trait-based account of value lifecycle — drop, copy, move, clone —
resolved **statically via monomorphization** and eliminated for trivial types, with
**no runtime vtables**. This replaces what used to be scattered special cases (the
`BCDeleteDesc` runtime descriptor, the container `value_is_ref` flag, the move-only
bit) with one model.

### The model

Every type conceptually has `drop(self)` / `copy_init(dst,src)` /
`move_init(dst,src)` / `clone(self) -> Self`, exposed as three traits mapped onto
machinery Roxy already has:

- **`Drop`** ⟵ `fun delete T()` — user-writable; auto-derived for aggregates.
- **`Clone`** ⟵ `.copy()` — explicit deep copy; auto-derived.
- **`Copy`** — a marker: *implicit* copy permitted (else move-only). The exact
  inverse of `Type::noncopyable()`; `is_copy()` is its spelling.

The point is **not** runtime dispatch. Resolution runs through the existing
monomorphized trait machinery (the `Printable`/`Hash`/`Eq` path), so each lifecycle
event lowers to a direct call (inlinable) — or, for trivial types, to nothing.

**The `Copy` + `Drop` wrinkle.** Unlike Rust, Roxy lets the two coexist, because
`ref` is implicitly copyable *and* lifecycle-nontrivial (copy → `ref_inc`,
drop → `ref_dec`). So `Copy` means only "implicit copy allowed", not "trivially
memcpy-able"; trivial types are a *subset* of `Copy`. The two count-bearing-copyable
kinds are `ref` (a counted borrow) and **`string`** (reference-counted since finding
9b — copy → `roxy_string_retain`, drop → `roxy_string_release`, free at zero, with
pooled literals immortal); see [strings.md](strings.md). A struct's retain/drop is
then composed automatically from *containing* one of these.

### Predicates

`Type` carries the structural decisions the lowering consumes:

- `is_copy()` — implicit copy allowed (vs move-only).
- `needs_drop()` — owns/borrows a resource to release (recurses through value-struct
  fields; every indirecting kind is a leaf, so it terminates). True for `ref`, `uniq`,
  containers, coroutines, closures, and **`string`** (release-on-drop, finding 9b).
- `needs_retain()` — implicit copy has a side effect (transitively contains a `ref`,
  or **is / contains a `string`**).
- `is_trivial()` — `is_copy && !needs_drop && !needs_retain` → emit nothing (the
  `is_trivially_destructible` analogue).
- `member_needs_drop()` — a non-recursive variant (`noncopyable() || ref`) used by
  the synthetic-destructor pass, the struct field-walk, and both backends' container
  drops (cycle-safe).

### Move-only containers

**Move-only** is the `!is_copy()` case (the inverse of the `Copy` marker): a
`List`/`Map` (it owns a heap buffer), a struct with a `ref` field, and a coroutine
with a `ref` param are all move-only, and each counts the borrows it holds for its
lifetime. The per-feature mechanics live under
[Applying the model](#applying-the-model) —
[containers](#containers-are-move-only),
[their counted borrows](#containers-of-borrows-hold-counted-borrows), and
[coroutine `ref` params](#coroutines).

### One drop derivation, two executions

`compute_drop_plan(Type) -> DropPlan` (in `types.cpp`) decides the *kind* of drop
once — `DropKind` (None / CallDtor / WalkFields / List / Map / Closure / RefDec /
StrRelease) plus
`free_obj` and the involved types — and **both backends lower the same plan**:

- **VM** keeps its **native** `delete_value` walk over `BCDeleteDesc` — that *is* the
  VM's drop-glue executor; nothing is inlined per site, so there is nothing to
  "factor out", and emitting interpreted bytecode glue would be *slower*.
  `BCDeleteDesc` is therefore not eliminated.
- **AOT/C** lowers the plan to generated `roxy_drop__<T>` glue functions (and a
  struct's `$$delete`), which the C compiler inlines and ICF-folds.

Each backend keeps the execution that's efficient for it; neither re-derives. At a
*true* erasure boundary — a closure env dropped by `type_id` — a single `drop_glue`
function pointer in the header survives; that is one pointer for one operation, **not**
a per-operation vtable.

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
