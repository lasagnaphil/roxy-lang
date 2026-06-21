# Lifetime & Borrow Soundness

> **Status:** Largely implemented (VM). The *constraint-reference* model below
> is live: `ref` is a fully-counted borrow, the free-trap is centralized across
> every free path, the three audited findings are fixed, the `out`/`inout`
> second-class rule and `[ref self]` capture counting are enforced,
> **call-site heap-root counting is complete** (§4/§8): the `uniq` method
> receiver (any shape — a bare identifier, a `uniq` field root `a.b.method()`, or
> a heap-returning temp `make().method()`) *and* an `out`/`inout` argument that
> points into a heap object's storage (`f(inout heap_obj.field)`) are each counted
> for the call's duration via a copy-prop-pinned borrow that gives the
> call-straddling count a distinct SSA identity (no polymorphic dispatch needed),
> and the **non-capture `self` promotions are wired** (§6): binding (`var r: ref T
> = self`), returning (`return self`), storing (`r = self`), and passing `self` to
> a `ref` parameter (`f(self)`) all heap-gate the promotion with an `AssertHeap`
> that traps a stack receiver before the borrow inc; and **`List<ref T>`
> ref-element counting** is in (push `RefInc`s, destroy/overwrite `RefDec`,
> pop hands off — a List of `ref` is move-only). Still **deferred**: `Map<_, ref
> T>` ref-counting and coroutine ref-param counting (Phase 3); refcount elision
> (§11, always a later phase); and full AOT/C-backend parity beyond the
> `roxy_free` trap. See the per-item status in [§13](#13-implementation-status).
>
> This supersedes [memory.md](memory.md) where the two differ; memory.md states
> the same model but describes the older, incomplete implementation.

**In one sentence:** `ref` is a *constraint reference* — a borrow of a **heap**
object that increments a count in the object's header while it lives, and an
object cannot be freed by any path while its count is nonzero (the free traps);
**stack** value-structs are never borrowed with `ref` — they are passed by
reference only through the second-class `out` / `inout` / `self` family, which
flows downward and cannot escape; and `weak` is the sole user of generational
references.

## 1. Why

An audit found three classes of memory unsafety in ordinary safe Roxy code:

1. **`ref` locals are uncounted.** `var b: ref T = owner` emits no inc/dec —
   only `ref` *parameters* were ever counted, and even those decremented before
   any `delete` could observe the count. Moving or deleting `owner` while `b`
   lives is caught by nothing → use-after-free.
2. **`ref` escapes by `return`.** A function returns a borrow of a local it then
   destroys; no count holds the owner alive → dangling reference.
3. **`container[i] = v` for noncopyable elements** neither destroys the old
   element nor consumes the new temporary → leak plus double-free. (A
   move/cleanup bug, not a lifetime bug; fixed alongside, §9.)

The model memory.md describes — "creating a `ref` increments `ref_count`,
destroying it decrements, `delete` fails if `ref_count > 0`" — is *correct*. It
was simply half-built: incomplete counting and a free-trap on only one of the
several free paths. This document specifies the complete version, **now
implemented** — all three findings are fixed (§7, §13).

## 2. The model: constraint references

The invariant:

> An object's `ref_count` equals the number of live `ref` borrows of it. **No
> object may be freed, by any path, while its `ref_count` is nonzero — the free
> traps.**

This is a *borrow* count, not an *ownership* count: `uniq` is the sole owner and
does not touch `ref_count`; the count only ever *blocks* a free, never *causes*
one. So there are no ownership cycles to leak (and `ref` remains banned from
struct fields, with `weak` for back-references), and errors are **eager** — they
fire at the offending `delete`, not at a later dangling use.

Because `ref` borrows only heap objects (§3), the count always has a home (the
`ObjectHeader.ref_count`, already present) and inc/dec sites are **statically
known to be heap** — no runtime "is this on the heap?" test is needed anywhere
except the one promotion gate (§6). `ref` stays a **thin pointer** (2 slots);
what changes is that it becomes a count-bearing *type* — copy increments, drop
decrements — with complete, all-paths bookkeeping.

## 3. `ref` is heap-only; stack data uses the second-class family

`ObjectHeader.ref_count` lives only on heap objects. The language already
reflects this: a `ref` can be created only from a heap source — a `uniq`, another
`ref`, or a `borrowed` subscript of a heap-pointee element — and binding one to a
stack value-struct does not type-check (`var r: ref Vec2 = some_stack_vec` is an
error today). This design makes that the *rule*, not an accident: **`ref` borrows
heap objects, period.**

Stack value-structs are passed by reference only through the **second-class
family** — `out`, `inout`, and the method receiver `self`:

- they may be dereferenced and passed *onward* as further second-class arguments
  (downward the call stack), and nothing else: never bound to a `ref`, stored,
  returned, or captured;
- they carry the existing rule that they "cannot be converted to uniq / ref /
  weak in any way" ([overview.md](../overview.md));
- downward-only flow makes them safe *by construction* — the frame that owns the
  stack struct outlives every callee — so they need no count and no header.

`self` belongs to this family. Although the IR types the receiver as a pointer
(methods.md writes it `ref<T>`), it is **not** a first-class counted `ref`: it
obeys the second-class rules and works uniformly on stack and heap receivers. A
function that needs to *retain* a borrow it receives takes `weak` or a copy — it
cannot stash a second-class borrow.

The one consequence to internalize: there is no "polymorphic `ref`" that might be
stack or heap. `ref` is always heap (counted, statically); the stack-capable
things (`out`/`inout`/`self`) are always second-class (uncounted, downward). The
two never mix, so no representation or per-deref test straddles them.

## 4. Counting mechanics

### Increments
- creating a `ref` from a `uniq` / `ref` / `borrowed`-subscript of a heap
  pointee (a first-class borrow is born);
- **copying** a `ref` into another binding that holds it for a lifetime —
  another `ref` local, a `List<ref T>` / `Map<_, ref T>` slot, a closure capture,
  a `ref`-typed parameter;
- a **call-site borrow of a heap root**: when a method receiver, or an
  `out`/`inout` argument, roots in a statically-heap object
  (`heap_obj.method()`, `bump(inout heap_obj.field)`), the call site increments
  that root's count for the call's duration. The root's heap-ness is known
  statically at the call site, so this needs **no** runtime test. A *stack* root
  increments nothing (second-class, safe by downward flow); a receiver that is
  already a counted `ref` (`r.method()`, `list[i].method()`) is already covered
  by that `ref`'s count.

### Decrements
Every point a live `ref` (or a call-site heap-root borrow) dies — and on **every**
exit path, which is the part the audit found missing. The existing scope-cleanup
machinery (cleanup records in `ir_builder.cpp`, `execute_cleanup` in the
interpreter) that drops `uniq` locals is extended to also `RefDec` live borrows,
on the same paths: normal scope exit, `return`, `break`, `continue`, and
exception unwinding. A returned `ref` is *not* decremented at the returning frame
— its count hands off to the caller, mirroring how a moved `uniq` is not dropped.

**Implemented as:** `ref` locals are `OwnedKind::RefBorrow` entries in the same
`m_owned_locals` list as `uniq` locals, so they get LIFO scope cleanup and
exception records for free; a new `BCCleanupKind::RefDec` cleanup-record kind
makes `execute_cleanup` decrement rather than destroy. Two subtleties the audit
under-stated surfaced here:

- **Ref *parameters* leaked on exception unwind.** Their decrement was emitted
  only at the normal-path `return`; an exception thrown *through* a `ref`-param
  frame (e.g. the Lox interpreter's `ReturnException`) skipped it, so counts
  accumulated. Fixed by a whole-function-scoped RefDec cleanup record per ref
  param, with that param's register liveness pinned to the function end so the
  unwind decrement reads a valid register on throw-only paths.
- **The hand-off must be a 1:1 transfer.** Every `ref` return carries *exactly
  one* count (a ref local hands off its create-inc; a ref param or a fresh ref —
  field / borrowed subscript / `ref x` — increments to produce one; a call result
  already carries one). The binder then **adopts** a call result (no inc) and
  **increments** any other still-live source. Otherwise a returned ref-local
  bound by the caller would double-count (a safe over-count → spurious trap).

### The free-trap
The single choke point is **`object_free`** (`object.cpp` / `roxy_free`): before
freeing, if `ref_count != 0`, set `vm->error` ("cannot delete: object has N
active borrows") and refuse. Putting the check here — rather than only in the
`DEL_OBJ` opcode, as today — makes **every** free path trap uniformly: explicit
`delete`, RAII drop, the descriptor walk (`delete_value`, which today checks
nothing), container element cleanup, reassignment-overwrite, and move-then-drop.

### Interior pointers
A `borrowed` subscript or a `[ref self]` promotion can target an inline
value-struct field of a heap object, so the count must be reachable from an
*interior* pointer. `resolve_header(ptr)` goes through the allocator's sorted
slab-range index (`find_slab_containing` + rounding down by `slot_size`) to find
the owning slot's header; large objects get the same range index in place of
their base-keyed map.

## 5. `weak`: the sole generational type

`weak` is unchanged and is the **only** consumer of generational references: a
`weak T` is `{ptr, generation}` (4 slots), captured via `WeakCreate`, validated
on use by `WEAK_CHECK` / `roxy_weak_valid`, yielding null / false when the
referent is tombstoned or recycled. The two mechanisms have clean, separate jobs:
**counting decides whether a free is legal; generations let a `weak` observe that
a free already happened.**

### Why generational (alternatives considered)

The generation's distinguishing properties are that taking/copying/dropping a
`weak` needs **zero bookkeeping** (snapshot on create, no-op on drop) and that
the owner's memory **frees immediately** on delete. The surveyed alternatives
each give one of those up in a way that lands badly on Roxy's profile (no GC,
churny allocation, value-semantic weaks copied freely, weak-observer registries):

| Approach | Why rejected for Roxy |
|---|---|
| **Auxiliary weak count + deferred free** (`shared_ptr`/`weak_ptr` style) | Reintroduces all-paths inc/dec on the *most* casually-used reference — the exact fragility just removed from `ref` — and a forgotten weak pins the dead object's slot (bounded to ~1 slot since the destructor still runs, but it creeps in long-lived weak registries, with no GC to reclaim it). Also fights the slab's slot recycling. Its upsides (no generation; 2-slot weak; exact validity) don't outweigh this. |
| **No slot reuse + liveness bit** | No ABA → no generation and a 2-slot `{ptr}` weak, but only by abandoning the free-list recycling the slab added specifically to fight fragmentation. Trades a tiny collision probability for unbounded slot retention under churn. |
| **Intrusive back-list (object nulls its weaks on death)** | Requires every `weak` to live at a stable, registered address; Roxy weaks are values copied through registers and realloc'd containers. Fundamentally incompatible. |
| **Page-protection / fault-on-use** | Page granularity is absurd for ~32-byte objects, and a fault is a crash, not the graceful null-on-test `weak` must give. |

Generations cost only header bytes. The width is **kept at the random 64-bit**
value [memory.md](memory.md) already specifies: 2⁻⁶⁴ collision-per-recycle, and
resistance to deliberate reuse attacks from untrusted embedded scripts. A 32-bit
generation would shrink the header (to as little as 8 bytes, at 2⁻³² per
recycle), but weak validity is a *correctness* property, the heap-object header
is not on a hot enough per-object path to justify weakening it, and 64-bit keeps
the guarantee robust even under adversarial allocation. So the header stays
`{ u64 weak_generation, u32 ref_count, u32 type_id }` = 16 bytes, and a `weak`
stays 4 slots.

## 6. Promotion: the one runtime storage test

`self` is second-class, but `[ref self]` / `[weak self]` capture (and binding
`self` into a first-class `ref`) must turn it into a counted/generational
reference — which is legal only if the receiver is actually on the heap. This is
the **one** place storage isn't known statically (the method doesn't know whether
it was called on a stack or heap receiver), so it is the **one** runtime test:
the slab-range check (today's `ASSERT_HEAP`). Heap → capture the count (or
generation) and produce a first-class reference; stack → **trap** ("cannot retain
a borrow of stack data — copy it, or allocate the receiver with `uniq`"). Where
the analysis already knows the receiver's storage, the test folds away to an
unconditional inc or a compile error.

**Implemented for `[ref self]` capture.** `ASSERT_HEAP` already trapped stack
receivers gracefully; what was missing was the *count*. Now a `ref` capture is
`RefInc`'d at env construction (after the heap check, so a stack receiver still
traps before the inc) and the env's destructor `RefDec`s it.

**Implemented for the non-capture promotions, too.** Binding (`var r: ref T =
self`), returning (`return self` from a `ref`-returning method), storing (`r =
self` reassigning a ref local), and passing `self` to a `ref` parameter
(`f(self)`) now each emit the `AssertHeap` gate before the borrow inc. The first
three share one IR-builder helper (`emit_ref_borrow_inc`) that inserts the gate
when the ref-source is a bare `self`; the fourth is gated at the *call site*
(`lower_call_args`), because the unsound inc is the callee's ref-param entry inc —
so a stack receiver must trap *before* the call. Unlike the capture path's
copyable-only gate, the promotion gate is emitted for *every* bare-`self` source
(a method's receiver storage is never known at its compile site), so it correctly
traps a **noncopyable** stack value-struct receiver as well. Inside a lambda body
`self` is already rewritten to `__env.__self` (sourced from a heap-checked env),
so only the bare `ExprThis` of a direct method body reaches the gate. The
remaining non-capture promotions named in §6 are now all wired; only the runtime
*storage-known* folding (unconditional inc / compile error) is left as a later
optimization.

This required first fixing a **pre-existing closure-cleanup bug**: deleting a
closure was an inert no-op, so envs and their captures (incl. `[move uniq]`)
leaked. Because a closure flows through the uniform `fun()->R` type — which erases
*which* env struct it is — the env's cleanup can't be resolved statically at the
delete site (`var g = makeClosure()`). It is now dispatched virtual-destructor
style: a synthesized destructor is built per env struct and looked up by the
env's runtime `type_id` on delete (`RoxyVM::closure_env_dtors`,
`BCDeleteDesc::Closure`).

## 7. How the findings are resolved

- **Finding 1** (`var b: ref = owner; consume_and_free(owner); use(b)`): creating
  `b` increments owner's count to 1. The callee frees it → `object_free` sees
  count 1 → **traps** at the free. *(Implemented + tested.)*
- **Finding 2** (`return r;` borrowing a local owner): the returned `ref` carries
  its count, so the local owner's RAII drop sees count 1 → **traps** at the drop.
  *(Implemented + tested.)*
- **Finding 3**: a cleanup bug, fixed independently (§9). *(Implemented + tested.)*
- **Mid-call alias-kill** (`evil(l[0], inout l); list.clear()`): `l[0]` is a
  counted borrow of the element; `clear()` tries to free it → count 1 → **traps**.
  *(Covered today by the `ref`-param count on the borrowed argument, per Phase 1.)*
- **Mid-call receiver kill** (`heap_obj.method()` whose body reaches and frees the
  object): the call site counts the heap receiver for the call's duration →
  the free sees count 1 → **traps**. *(**Implemented + tested** for a `uniq`
  *identifier* receiver — §13. Exercised end-to-end by passing the receiver
  `inout` to its own method and reassigning it (`slot = nil`): the now-correct
  inout free of the borrowed object traps.)*

## 8. Interactions

- **Methods / `self`:** `self` is a second-class receiver borrow (§3). A method
  call on a statically-heap receiver counts that receiver for the call (so an
  alias-kill of it mid-method traps); on a stack receiver it counts nothing
  (downward-safe); on an already-`ref` receiver the existing count covers it.
  Binding / returning / storing `self`, or passing it to a `ref` param, is a
  promotion (§6) — now wired, each heap-gated by an `AssertHeap`. *(**Implemented**
  for every `uniq` receiver shape — a bare identifier (`c.method()`), a `uniq` field root
  (`o.inner.method()`, where the borrow lands on the receiver object itself,
  which a `delete o` would also try to free → it traps), and a heap-returning
  temp (`make().method()`, counted distinctly from the temp's own scope-exit
  Delete) — §13. The borrow rides a copy-prop-pinned `Copy` of the receiver so its
  RefDec + Nullify cleanup own a distinct SSA value/register and can't clobber the
  owned-local's / temp's Delete record; the borrow's exception record is appended
  after the owned-local records so it RefDecs *before* the owner's Delete on
  unwind. Stack / `ref` receivers are correctly skipped.)*
- **Closures:** captures are by copy. Capturing a first-class `ref` copies it →
  increments; the env's destructor decrements. `[ref self]` / `[weak self]` are
  promotions (§6): slab-range test at capture, trap on stack receivers.
- **Coroutines:** a coroutine's parameters and promoted locals live in the heap
  state struct — so a `ref` parameter of a coroutine is a first-class counted
  borrow, incremented into the state struct at init and decremented when the
  coroutine is destroyed. Deleting the borrowed owner while a suspended coroutine
  still holds the borrow traps. *(**Not yet implemented** — §13.)*
- **Containers are move-only.** A `List<T>` / `Map<K,V>` owns a heap buffer, so —
  like `uniq` — it is **noncopyable regardless of element type**: passing it by
  value moves it, and the source can't be used afterward. An explicit `.copy()`
  method deep-copies when an independent duplicate is genuinely wanted. This makes
  "owns heap → move-only" uniform (previously containers were the odd copyable-
  yet-leaking case: copyable containers were deep-copied on value-pass but never
  destroyed, so their buffers leaked). The callee-side value-param deep-copy
  (`lowering.cpp`) is skipped for noncopyable containers, so a value param is a
  true move, not a move-then-copy.
- **`List<ref T>` holds counted borrows:** push `RefInc`s, and
  pop / overwrite / container-destroy `RefDec` (the element-cleanup machinery
  learns `ref` elements are count-bearing via a `BCDeleteDesc::RefDec` element
  descriptor — `delete_slot_entry` reads the borrowed pointer and `ref_dec`s it;
  the C-emitter's `emit_delete_slot` emits `roxy_ref_dec`). Deleting an owner
  while a `List<ref T>` still borrows it traps. *(`Map<_, ref T>` ref-counting is
  a follow-on — it needs insert-`RefInc` with replace handling and `Map.remove` /
  `Map.clear` to run per-value cleanup, the related gap that they don't yet
  destroy noncopyable values at all.)*
- **`borrowed T`** ([memory.md](memory.md)): a subscript on a heap-pointee
  element (`List<uniq T>`) yields a counted `ref` to the pointee (realloc moves
  the buffer, not the pointee, so the borrow stays valid). A subscript on an
  **inline** element (`List<Vec2>`) is a second-class borrow (the buffer has no
  per-element header to count), expression-scoped, and the container may not be
  mutated while it is live (realloc would move the element).
- **`out` / `inout`:** the second-class family alongside `self`. An argument
  rooted in a heap object (`bump(inout b.a)`) counts that root for the call, so a
  mid-call alias-kill traps; a stack-rooted argument counts nothing and is safe by
  downward flow. *Implemented:* the escape rule — a noncopyable `out`/`inout`
  cannot be moved out of its frame (bind / return / store / by-value-pass /
  capture-by-move are rejected at compile time); copyable `out`/`inout` escapes
  were already blocked by the type system (no value→reference conversion). *Also
  implemented:* the call-site root counting — a field-rooted `out`/`inout` lvalue
  (`f(inout heap_obj.field)`, `f(inout a.b.c)`) borrows the innermost heap object
  it points into for the call's duration (same pinned-copy + deferred-record
  mechanism as the receiver borrow), so freeing that root mid-call via an alias
  the callee reaches traps. A `ref`-rooted lvalue is already covered by the
  `ref`'s own count; a bare-identifier lvalue roots in the caller's frame (no heap
  root to count). Index-rooted lvalues (`f(inout list[i])`) need more than a
  count — their element buffer lives outside the slab, so realloc, not just free,
  can dangle the pointer; see the pin-based design in
  [§15](#15-container-element-lvalues-inoutout-listi).
- **FFI / AOT:** a `ref T` passed to a native function is counted for the call's
  duration, so the object **cannot be freed during the call**, even by reentrant
  Roxy code — the native's raw pointer is guaranteed live. New runtime surface:
  the free-trap in `roxy_free`, and `resolve_header`. `weak`'s `{ptr, gen}` ABI is
  unchanged.
- **Move checker:** unchanged in spirit. `ref` is *copyable* (copy = inc), so it
  is not move-tracked; instead `ref` bindings gain cleanup records that emit
  `RefDec` on all exit paths, reusing the noncopyable-cleanup machinery.

## 9. The index-set cleanup fix (Finding 3)

Not a lifetime bug, but fixed in the same effort. `gen_assign_index`
(`ir_builder.cpp`) must do what field assignment already does:

1. for a noncopyable element/value type, **destroy the overwritten element**
   before storing (today `roxy_list_set` and `roxy_map_insert`'s update-in-place
   just `memcpy` over the old value — `roxy_rt.cpp`);
2. **consume the right-hand temporary** (`consume_temp_noncopyable`) so its
   scope-exit delete is suppressed (today the temporary stays double-owned with
   the container → double-free).

**Implemented.** The consume keys off the *container's* element/value type, not
the index target's `borrowed` (`ref T`) type, so it fires for `List<uniq T>`.
The old-element destroy is unconditional for a List (the index is always in
bounds) and `contains`-guarded for a Map (an old value exists only for a present
key — a new key destroys nothing). Both reuse existing IR ops, so the C backend
gets the fix too.

## 10. Residual risks and sharp edges

- **Trapping during unwind.** In well-structured code, cleanup is LIFO, so a
  borrow declared after its owner is decremented before the owner is freed — no
  spurious trap. A genuine escape (a borrow stored in something longer-lived)
  freed during *exception* unwinding will trap mid-unwind; that is a real "cannot
  safely unwind" situation, and a clear trap beats a use-after-free. Cleanup
  ordering must guarantee decs precede the owner's free on every path; debug
  builds should assert count balance at frame exit.
- **Completeness is the whole game.** A single missed decrement makes the owner
  permanently undeletable (a loud trap at its delete, not unsafety); a single
  un-trapped free path is a use-after-free hole. Centralizing the trap in
  `object_free` and the decs in the shared cleanup machinery is what makes
  completeness auditable.
- **Count under/overflow.** `ref_count` is `u32`. Underflow indicates an
  unbalanced dec and is a tripwire (today's "ref_dec: reference count already
  zero"); with complete balancing it never fires. Overflow is bounded by
  live-borrow count and not a practical concern.
- **Single-threaded.** Inc/dec are non-atomic, matching the VM's single-thread
  assumption (`object.hpp`). A threaded runtime would need atomic counts.
- **Malloc-fallback allocator** (AOT before `roxy_rt_init`): the slab is the
  supported configuration; `resolve_header` and the free-trap assume slab-backed
  objects.
- **`roxy::ref<T>` AOT wrapper** is documented as "ref-counted, copyable; last
  copy frees" ([c-backend.md](c-backend.md)) — that is shared *ownership*, wrong
  for a constraint reference. It becomes a borrow handle: copy increments the
  borrowee's count, destruction decrements, and it never frees.
- **Migration.** Code that today returns or stores a `ref` derived from a local
  owner now traps (Finding 2 is a trap, not silent). The audit found no such test
  coverage, but it is a behavior change deserving a release note.

## 11. Refcount elision (planned, not in v1)

Full counting is the **standard**: v1 counts every borrow and pays the inc/dec at
every borrow site, and is sound by construction. Elision is a later optimization
phase that *removes* inc/dec pairs wherever the owner provably cannot be freed
during the borrow's lifetime — no intervening `delete`, move, or call that could
reach a free, with the owner's death sites all visible. This is the "compiler can
optimize out some inc-ref / dec-refs" the docs anticipate.

The critical property: **elision only ever removes provably-redundant counts.**
Conservative or incomplete elision yields slower code, never unsafe code — so
soundness never depends on the analysis being clever, and v1 ships correct before
any elision exists. The easiest wins are call-site receiver/arg counts where the
heap root is a local the callee can't reach (the common `local.method()` case),
which elide to nothing.

## 12. Spec changes vs. memory.md

memory.md states this model; the changes are completeness and correctness, not
direction:

| memory.md (as implemented) | this design |
|---|---|
| only `ref` *parameters* counted, decremented before delete observes | every `ref` counted: create / copy inc, all-paths dec, return hand-off |
| free-trap only in `DEL_OBJ` | free-trap in `object_free` → every free path (RAII, descriptor, container, overwrite) |
| `ref` could notionally borrow anything; stack borrows unprotected | `ref` is heap-only; stack data goes by `out`/`inout`/`self` (second-class, downward) |
| `ref` may be stored / returned freely (unsound) | counting makes stored/returned `ref`s safe; escaping a local owner traps at its drop |
| generations notionally shared by `ref` and `weak` | generations for `weak` only; `ref` is purely counted |

## 13. Implementation status

**Phase 1 — complete the count + universal trap. Done (VM).**
- `ref` copy = inc / drop = dec, balanced on every exit path incl. exception
  unwind, via the `RefBorrow` cleanup machinery; return hand-off with the 1:1
  caller-adopts convention (§4).
- Free-trap centralized in `object_free` (and `roxy_free`, in lockstep) so every
  free path traps; redundant `DEL_OBJ` pre-check retired.
- `resolve_header` for interior pointers (slab allocator).
- The index-set cleanup fix (§9) — pulled forward into Phase 1.

**Phase 2 — second-class family + promotion gate. Partially done (VM).**
- *Done:* the `out`/`inout` escape rule (§8) — moving a noncopyable second-class
  param out of its frame is rejected at compile time.
- *Done:* `[ref self]` capture counting (§6), and as a prerequisite, the
  closure-env cleanup fix (runtime-dispatched env destructors).
- *Done:* **call-site counting of a `uniq` method receiver** (§4/§8). The
  blocker — "a method receiver aliases its tracked owned-local (shared SSA
  value), so the call-straddling borrow's `Nullify` clobbers the local's own
  cleanup record, and a `Copy` to get a distinct value is undone by
  copy-propagation" — is resolved *without* polymorphic dispatch: the borrow
  rides an `IROp::Copy` flagged `no_copy_prop`, which copy-prop leaves intact, so
  the borrow owns a distinct SSA value (hence a distinct register, since its live
  range overlaps the receiver's). Around the call the IR builder emits
  `PinnedCopy → RefInc → Call → RefDec → Nullify`; the exception-path RefDec
  record is **deferred and appended after all owned-local records**
  (`m_call_borrow_cleanups`, flushed in `end_function_body`) so reverse-order
  unwind releases the borrow *before* the owner's Delete (else Delete sees
  `ref_count != 0` and spuriously traps). Lowering narrows the record to
  `[RefInc-pc, RefDec-pc)` via `m_ref_inc_pcs` (start) + `m_nullify_pcs` (end).
  The earlier double-delete in exception-heavy `uniq`-receiver code (the Lox
  interpreter) is fixed. Fires for any `uniq` receiver (see next bullet); `ref`
  receivers are skipped (already counted), stack receivers skipped (second-class).
- *Done:* **generalized the receiver borrow to every `uniq` receiver shape, and
  added the `out`/`inout` heap-root borrow** — the rest of call-site heap-root
  counting (§4/§8). The receiver-borrow gate in `gen_call_member` dropped its
  identifier-only restriction: it now fires whenever the receiver's type is
  `uniq`, so `obj` (already the receiver object's heap data pointer) is counted
  uniformly for a bare identifier, a `uniq` field root (`o.inner.method()` — the
  borrow lands on the receiver object, which `delete o` would also free → traps),
  and a heap-returning temp (`make().method()` — distinct from the temp's own
  Delete via the pinned copy). For `out`/`inout` arguments, `gen_call_expr`
  computes the heap root of each field-rooted lvalue (`heap_root_of_lvalue` walks
  the chain to the innermost heap object dereferenced; pure paths only, so the
  re-load is side-effect-free) and brackets the call with the same
  `PinnedCopy → RefInc → … → RefDec → Nullify` + deferred exception record. So
  `f(inout heap_obj.field)` traps if the callee frees `heap_obj` mid-call.
- *Done:* **the non-capture `self` promotions** (§6). Binding (`var r: ref T =
  self`), returning (`return self`), and storing (`r = self`) a bare `self` as a
  first-class `ref` route their borrow inc through `emit_ref_borrow_inc`, which
  emits an `AssertHeap(self)` before the inc when the source is a bare `ExprThis`
  (so a stack receiver traps before the header-writing inc). Passing `self` to a
  `ref` parameter (`f(self)`) is gated at the call site in `lower_call_args`
  (the unsound inc is the callee's ref-param entry inc, so the gate must precede
  the call); the param index uses an offset that is certain only for free/closure
  identifier callees (0) and genuine user-struct method callees (1), skipping
  uncertain shapes (module-qualified / container / field-closure) to avoid a
  spurious trap. Unlike the capture path's copyable-only gate, this fires for
  *every* bare-`self` source, so it also traps a noncopyable stack value-struct
  receiver. The shared `ASSERT_HEAP` trap message was generalized (no longer
  capture-specific) in both backends.
- **Index-rooted `out`/`inout` lvalues** (`f(inout list[i])`): a *true* element
  address, kept valid for the borrow by pinning the container against free and
  structural mutation. Full design in
  [§15](#15-container-element-lvalues-inoutout-listi). **Implemented
  (Phases 1–3) for copyable-element containers**: the runtime `borrow_count` pin +
  mutator guards (C-API-tested), the `INDEX_ADDR` op + `gen_lvalue_addr`
  `ExprIndex` case, and the call-site `ContainerPin`/`Unpin` wiring. `f(inout
  list[i])` / `f(inout map[k])` work on both backends — for primitive, struct,
  *and* `uniq` elements (an owning element re-types to `uniq T` for `inout`, and
  is reassignable in place). A mid-call realloc or (for a noncopyable container)
  free of the borrowed container traps (VM). **Deferred:** C-backend trap
  *reporting* (the mutation is refused memory-safely; the clean abort/report is
  the general AOT-trap-reporting follow-on).
- **Deferred (optimization)** — folding the promotion's runtime `AssertHeap` away
  to an unconditional inc / compile error where the receiver's storage is
  statically known (§6).
- *Done (separate fix):* **inout/out owning-pointer reassignment frees the
  overwritten value.** `slot = uniq T(..)` / `slot = nil` through an `inout`/`out`
  `uniq`/`List`/`Map`/`Coro` pointer now loads and Deletes the old value before
  the store, and consumes the RHS temp — previously it leaked the old object and
  double-owned the new one (the same class as the index-set fix, §9). This is
  also what makes the mid-call receiver kill testable (§8).
- *Done (separate feature):* **module-level globals** now compile end-to-end —
  including running a `uniq` global's constructor at init and its destructor at
  shutdown (RAII). Globals were previously unimplemented (no storage / no init),
  which is why a global `uniq` appeared to "skip its constructor." See
  [globals.md](globals.md).

**Phase 3 — containers & coroutines. Containers move-only + `List` ref-counting done; `Map` ref-counting + coroutines remain.**
- *Done:* **containers are move-only.** `Type::noncopyable` returns true for every
  `List`/`Map` (a container owns a heap buffer, so it's move-only like `uniq`),
  with an explicit `.copy()` method (`native_list_copy`/`native_map_copy` bound as
  `List<T>.copy()` / `Map<K,V>.copy()`; C-emitter maps `copy` → `roxy_*_copy`).
  This also fixes the previous copyable-container buffer leak (copyable containers
  were never destroyed). Fix found along the way: `gen_constructor_call` was
  missing `nullify_moved_field_source`, so `Ctor(o.field)` moved a container field
  without nulling it → double-free (surfaced by the Lox interpreter, now fixed).
  Migrated the copy-reliant tests (lists/generics/raii/interop) to the move idiom
  (`.copy()`, read-before-pass, or move-in-return-out for natives).
- *Done:* **`List<ref T>` ref-element counting.** `push` `RefInc`s the borrow
  (`gen_call_member`), `pop` hands the count off to the caller (the
  ref-return-adopt convention — no change needed), overwrite (`refs[i] = x`)
  releases-old / acquires-new (`gen_assign_index`), and container-destroy
  `RefDec`s each element via a new `BCDeleteDesc::RefDec` element descriptor
  (`build_delete_desc(ref)` → `delete_slot_entry` reads the borrowed pointer and
  `ref_dec`s it; the C-emitter's `emit_delete_slot` emits `roxy_ref_dec`).
  Deleting an owner still borrowed by the list traps. Both backends.
- *Remaining:* **`Map<_, ref T>` ref-counting** — needs insert-`RefInc` (with
  replace handling), and `Map.remove`/`Map.clear` to run per-value cleanup (the
  related gap that they don't destroy noncopyable values at all — likely a
  value-destructor callback on the map header). Map stays copyable until then.
- *Remaining:* count `ref` parameters into coroutine state structs.

**Phase 4 — elision (optimization, §11). Not started** (always a later phase).

**AOT / C backend.** The `roxy_free` trap is kept in lockstep, and fixes built
from existing IR ops (the index-set destroy) carry over; full parity
(`roxy::ref<T>` as a borrow handle, etc.) is a follow-on.

## 14. Files

Implemented (Phase 1 + the done parts of Phase 2):

| File | Change |
|------|--------|
| `include/roxy/rt/slab_allocator.{hpp,cpp}` | `resolve_header(ptr)` from interior pointers; sorted range index for large objects |
| `src/roxy/vm/object.cpp` | free-trap in `object_free`; `ref_dec` underflow tripwire kept |
| `src/roxy/rt/roxy_rt.cpp` | free-trap in `roxy_free` (lockstep with the VM) |
| `include/roxy/rt/roxy_rt.{h}`, `src/roxy/rt/roxy_rt.cpp` (§15 Phase 1) | `borrow_count` in the list/map headers; `roxy_list_pin`/`unpin` + `roxy_map_pin`/`unpin`; structural-mutator guards (`push` / `insert` / `remove` / `clear`); the non-catchable `roxy_runtime_error_*` channel |
| `tests/unit/test_container_pin.cpp` (new, §15 Phase 1) | C-API tests: pin refuses push/insert/remove/clear and raises the trap; unpin restores; nested pins count; in-place set/reads stay allowed; a copy is born unpinned |
| `ssa_ir.{hpp,cpp}`, `ir_optimize.hpp`, `ir_builder.{hpp,cpp}`, `lowering.cpp`, `vm/bytecode.hpp`, `vm/interpreter.cpp`, `c_emitter.cpp` (§15 Phase 2) | `IROp::IndexAddr` + `emit_index_addr` + `gen_lvalue_addr` `ExprIndex`; lowering to `INDEX_ADDR_LIST`/`INDEX_ADDR_MAP` (0xE4/0xE5) + interpreter handlers; C-emitter element-pointer codegen + pointer-tracking |
| `tests/e2e/test_lifetimes.cpp` (§15 Phase 2) | functional `inout`/`out list[i]` (i32, wide i64, struct elements) and `inout map[k]`, both backends |
| `rt/roxy_rt.{h,cpp}` (§15 Phase 3) | `roxy_container_pin`/`unpin` (type-dispatched); `borrow_count` free-guards in `roxy_list_delete`/`roxy_map_delete` |
| `ssa_ir.{hpp,cpp}`, `ir_optimize.{hpp,cpp}`, `ir_validator.cpp`, `ir_builder.{hpp,cpp}`, `lowering.cpp`, `vm/bytecode.hpp`, `vm/interpreter.cpp`, `c_emitter.cpp` (§15 Phase 3) | `IROp::ContainerPin`/`ContainerUnpin` + `IRCleanupKind::Unpin`; `gen_call_expr` pins container-index `inout`/`out` args (pinned copy + deferred Unpin record); `CONTAINER_PIN`/`UNPIN` opcodes (0xE6/0xE7) + handlers + `execute_cleanup` Unpin; VM trap surfacing (runtime-error flag after `CALL_NATIVE`; `borrow_count` check in `delete_value`); C-emitter pin/unpin codegen + Unpin cleanup |
| `tests/e2e/test_lifetimes.cpp` (§15 Phase 3) | adversarial: mid-call `push` of a borrowed List traps (VM); nested element borrows balanced + in-place set allowed + exception-unwind unpin (both backends) |
| `src/roxy/compiler/semantic.cpp` (§15 owning elements) | `check_call_args` re-types an `out`/`inout` container subscript to the raw element/value type (so `inout uniq T` matches), bypassing the `index` method's `borrowed` read view |
| `tests/e2e/test_lifetimes.cpp` (§15 owning elements) | `inout` of a `List<uniq T>` / `Map<K, uniq V>` element reassigns in place (both backends); mid-call free + push of a borrowed `List<uniq T>` trap (VM) |
| `src/roxy/vm/interpreter.cpp` | `RefDec` cleanup-record kind in `execute_cleanup`; trap error propagation through `delete_value`/`DELETE`; retired `DEL_OBJ` pre-check; `BCDeleteDesc::Closure` env-dtor dispatch |
| `include/roxy/vm/bytecode.hpp` | `BCCleanupKind`, `BCDeleteDesc::Closure`, `BCTypeInfo.dtor_func_idx` |
| `include/roxy/compiler/ssa_ir.hpp` | `IRCleanupKind`, `IRCleanupInfo.whole_function_scope`; `IRInst.no_copy_prop` (pinned copy) + `IRCleanupInfo.call_borrow` (receiver-borrow start-narrowing) |
| `src/roxy/compiler/ir_optimize.cpp` | copy propagation skips `no_copy_prop` copies |
| `src/roxy/compiler/ir_builder.{hpp,cpp}` (receiver counting) | `emit_pinned_copy`; `gen_call_member` emits the `uniq`-receiver borrow (PinnedCopy → RefInc → Call → RefDec → Nullify) for *any* `uniq` receiver shape (identifier / field root / heap temp); deferred `m_call_borrow_cleanups`, flushed after owned-local records in `end_function_body` |
| `src/roxy/compiler/ir_builder.{hpp,cpp}` (out/inout root counting) | `heap_root_of_lvalue` (+ `is_pure_field_path`) finds the heap root of a field-rooted `out`/`inout` lvalue; `gen_call_expr` brackets the call with a pinned-copy borrow of each root (same RefInc/RefDec/Nullify + deferred record as the receiver borrow) |
| `src/roxy/compiler/ir_builder.{hpp,cpp}` (self promotions) | `emit_ref_borrow_inc` (+ `is_bare_self`) heap-gates bind/return/store of a bare `self` to a `ref` (`AssertHeap` before the inc); `self_pass_param_offset` + a `lower_call_args` gate cover passing `self` to a `ref` parameter |
| `src/roxy/vm/interpreter.cpp`, `src/roxy/compiler/c_emitter.cpp` (self promotions) | generalized the `ASSERT_HEAP` trap message (capture *and* bind/return/store/pass promotions) |
| `src/roxy/compiler/ir_builder.cpp` (inout reassign) | `gen_assign_local` pointer-param branch destroys the old value + consumes the RHS temp before storing through an `inout`/`out` owning pointer |
| `include/roxy/compiler/lowering.{hpp,cpp}` (receiver / root counting) | `m_ref_inc_pcs`; `call_borrow` scope_start narrowing to the RefInc PC (shared by the receiver and `out`/`inout`-root borrows) |
| `src/roxy/compiler/ir_builder.{hpp,cpp}` | `OwnedKind::RefBorrow`; ref-local inc/dec + reassign + return hand-off + caller-adopt convention; `[ref self]` capture RefInc + env-field RefDec; per-env synthesized destructors; call-result temp tracking; transitive-`[move]` field nullify; `gen_assign_index` cleanup fix |
| `src/roxy/compiler/lowering.cpp` | RefDec `BCCleanupRecord` + whole-function-scope param records + liveness pin; `Closure` delete desc; `BCTypeInfo.dtor_func_idx` |
| `src/roxy/compiler/semantic.{hpp,cpp}` | reject move-out of noncopyable `out`/`inout` (also closure move-capture); index-assign consume keyed off the container element type; env destructor for `ref` captures |
| `include/roxy/compiler/symbol_table.hpp`, `src/roxy/compiler/symbol_table.cpp` | `is_out_inout` flag on parameter symbols |
| `include/roxy/vm/vm.hpp`, `src/roxy/vm/vm.cpp` | `RoxyVM::closure_env_dtors` (env type_id → destructor index) |
| `include/roxy/compiler/types.hpp`, `vm/bytecode.hpp`, `lowering.cpp`, `vm/interpreter.cpp`, `c_emitter.cpp`, `ir_builder.cpp` (Phase 3 — List ref-counting) | `Type::noncopyable` makes `List<ref T>` noncopyable; `BCDeleteDesc::RefDec` + `build_delete_desc(ref)`; `delete_slot_entry` / `delete_value` (VM) and `emit_delete_slot` (C) `ref_dec` ref elements on destroy; `gen_call_member` `RefInc`s a `ref` element on push; `gen_assign_index` releases-old / acquires-new on overwrite of a `ref` element |
| `tests/e2e/test_lifetimes.cpp` (new), `tests/e2e/test_closures.cpp`, `tests/unit/test_slab_allocator.cpp` | findings 1–3; delete-while-borrowed/return-escape traps; balance across control flow; `out`/`inout` escape rejection; `[ref self]` pin/release; closure capture cleanup; `resolve_header`; **`uniq`-receiver call-site borrow balance + survives-a-throwing-method**; **field-rooted & heap-temp receiver balance + mid-call-free traps + throwing-heap-temp destroyed-once**; **`out`/`inout` heap-root balance + mid-call-free trap**; **self-promotion bind/return/store/pass: heap-receiver balance (incl. C backend) + stack-receiver traps (copyable & noncopyable; free-fn & method)** |

Index-rooted `out`/`inout` lvalues (`f(inout list[i])`) — **implemented for
copyable *and* owning (`uniq`) elements, [§15](#15-container-element-lvalues-inoutout-listi)**;
only C-backend trap *reporting* remains. `List<ref T>` ref-element counting is
**done** (§13); deferred: `Map<_, ref T>` ref-counting (insert/remove/clear +
per-value cleanup) and coroutine `ref`-param promotion; `roxy::ref<T>` → borrow
handle in `roxy_rt`; elision in `lowering.cpp`; folding the self-promotion
`AssertHeap` away where storage is known.

## 15. Container element lvalues (`inout`/`out list[i]`)

> **Status:** **Implemented** for both copyable *and* owning/noncopyable
> elements. `f(inout list[i])` / `f(inout map[k])` compile and run on both
> backends, for primitive, struct, *and* `uniq` elements (an `inout uniq T`
> element is reassignable in place — the reassign frees the old pointee, the
> container keeps the new one). The call site pins the container for the borrow,
> so the adversarial case — the same container reached mid-call via a second
> argument (`evil(inout xs[i], inout xs)`) and reallocated (push / insert) or, for
> a noncopyable container, freed (reassign) — **traps** (VM) instead of dangling
> the element pointer. The C backend refuses the mutation too (memory-safe), but
> its clean trap *reporting* is deferred, like the rest of AOT trap reporting.
> This section specifies the full design.

### Goal

`f(inout list[i])` passes the **actual address** of the element in the backing
buffer, so the callee mutates it in place — no copy, true aliasing. (Copy-in /
copy-out — read the element to a stack slot, pass that, write it back — was
considered and rejected: it is sound but is not a real lvalue, and a concurrent
read during the call sees the stale value.)

### The hazard

`list[i]`'s address points into the container's separately-`malloc`'d element
buffer, which lives **outside** the slab, so the generational/`ref_count`
machinery on the object header does not protect it. Three operations invalidate
the address, and any can happen mid-call if the callee reaches the container
through another channel (a second argument, a global):

| Mid-call operation | Effect on `&list[i]` |
|---|---|
| `delete list` | buffer freed → dangling |
| `list.push(x)` (grow) | buffer realloc'd / moved → dangling |
| `pop` / `remove` / `clear` | element gone / buffer freed → dangling |
| `list[j] = v` (in-place set) | **safe** — same slot, no move |

Counting the container header (the originally-noted plan) blocks only the *free*;
it does nothing about realloc. So a count alone is not sufficient here.

### Core mechanism: an element borrow **pins** the container

While `&list[i]` is outstanding, freeze the container against exactly the
operations that move or free the buffer:

- **Free** is blocked by the existing constraint-reference free-trap: the element
  borrow also takes an ordinary **`ref_count`** on the container object — this is
  the `heap_root_of_lvalue` + call-site-borrow machinery (§4/§8) extended to
  index lvalues. `delete`-while-borrowed → traps.
- **Structural mutation** (realloc / shrink) is blocked by a **new `borrow_count`**
  in the container header. Structural mutators trap while `borrow_count > 0`.

Net: while the element is borrowed the buffer can neither move nor be freed, so
the address stays valid. It is the Rust rule — "no structural mutation while
borrowed" — enforced at runtime, which suits single-threaded Roxy where borrows
flow into opaque callees.

**Why a separate `borrow_count` rather than reusing `ref_count` for the mutation
trap?** Because `fill(r: ref List<i32>) { r.push(1) }` is legitimate — but `r`
is a `ref` parameter, so its entry `RefInc` makes `ref_count > 0`, and a
`ref_count`-based push-trap would wrongly reject it. Mutation-blocking must be
scoped to *element* borrows only; free-blocking can safely reuse `ref_count`.

### Pieces

1. **`uint32_t borrow_count`** added to `roxy_list_header` / `roxy_map_header`
   (shared runtime header; the existing `_pad[3]` absorbs it).
2. **Mutation guards in the shared runtime** (`roxy_rt.cpp`): `roxy_list_push` /
   `roxy_list_pop`, `roxy_map_insert` / `roxy_map_remove` / `roxy_map_clear` trap
   when `borrow_count > 0`. The VM routes `list.push` → `native_list_push` →
   `roxy_list_push`, so **one guard per op covers both the VM and the C backend**.
   In-place `set` and all reads stay allowed.
3. **`INDEX_ADDR_LIST` / `INDEX_ADDR_MAP`** — a new IR op for the bounds-/key-checked
   element address. The runtime primitive already exists (`roxy_list_get` /
   `roxy_map_get` return the slot pointer); for the VM it is a small new opcode
   mirroring `INDEX_GET` but storing the *pointer* rather than the loaded value;
   the C backend lowers it to `roxy_list_get` with an explicit bounds trap.
4. **Pin/unpin around the call** — reuse the deferred-cleanup call-site borrow
   machinery (`PinnedCopy → inc → call → dec → Nullify`, exception-safe):
   `borrow_count++` before the call, `--` after and on unwind; plus the ordinary
   container `ref_count` borrow (free-trap) via `heap_root_of_lvalue`.
5. **`gen_lvalue_addr` `ExprIndex` case** → emit `INDEX_ADDR`. **No reload** —
   writes go straight into the buffer (the address *is* the storage); this also
   removes today's IR-build crash.
6. **`heap_root_of_lvalue` extension** for `ExprIndex` → returns the container
   (the free-trap `ref_count` root).
7. **Semantic** — allow `inout` / `out list[i]` (it already type-checks; just
   match the element type to the `inout` parameter and drop the crash path).

### Soundness summary

| Mid-call event | Outcome |
|---|---|
| `delete` the container | `ref_count` free-trap |
| `push` / `insert` (realloc) | `borrow_count` mutation-trap |
| `pop` / `remove` / `clear` | `borrow_count` mutation-trap |
| in-place `list[j] = v` | allowed (valid slot) |
| free + slot recycled into a new container | impossible — the free is trapped first |

A fully sound true lvalue.

### Scope

- **List and Map**, primitive, struct, *and* owning (`uniq`) elements.
- **Owning elements** (`List<uniq T>` / `Map<K, uniq V>`): an `inout`/`out`
  subscript re-types to the raw element type (`uniq T`), not the `borrowed` read
  view (`ref T`), so the callee gets reassignable access to the owning slot. The
  in-place reassign frees the old pointee (the existing inout/out owning-pointer
  reassignment path), and the inout escape rule already forbids moving the element
  out of the frame, so the container still owns exactly one value per slot at
  delete time (no double-free, no leak). For a noncopyable container the delete
  frees the buffer, so the pin's free-guards (`delete_value` / `roxy_*_delete`)
  are live: freeing the borrowed container mid-call traps.
- Non-`inout` element lvalues (binding a `ref` to an element) stay out of scope —
  `borrowed` already covers reads.

### New rule to surface (cf. §8, §10)

"You cannot structurally mutate a container while an element of it is borrowed
(`inout` / `out`)." The pin is per-container (coarse — borrowing one element
freezes the whole container's structure), which is simple and sufficient.

### Phased plan (each independently testable)

1. **Runtime — DONE.** `borrow_count` (a `uint16_t` absorbed into the existing
   `_pad` in both headers, no size change) + `roxy_list_pin`/`unpin`,
   `roxy_map_pin`/`unpin` + structural-mutator guards: `roxy_list_push` and
   `roxy_map_insert` (covers `index_mut`) / `roxy_map_remove` / `roxy_map_clear`
   refuse the op while pinned. A refusal raises a *fatal, non-catchable* runtime
   trap via a new thread-local channel (`roxy_runtime_error_set` /
   `_pending` / `_message` / `_clear`), distinct from catchable user exceptions —
   the buffer is left untouched so the borrowed pointer can't dangle. `pop` /
   in-place `set` / reads stay allowed (they don't move or free the buffer); a
   copy is born unpinned. Unit-tested at the C-API level
   (`tests/unit/test_container_pin.cpp`). The single guard per op lives in shared
   `roxy_rt`, so the VM (which routes `list.push` → `roxy_list_push`) and AOT both
   get it; surfacing the trap as `vm->error` / an AOT abort is Phase 3.
2. **`INDEX_ADDR` op — DONE.** New `IROp::IndexAddr` (mirrors `IndexGet`, carries
   the same `IndexData`) → VM opcodes `INDEX_ADDR_LIST` / `INDEX_ADDR_MAP` (0xE4 /
   0xE5; bounds-/key-checked, store the raw buffer pointer in the dst register) →
   C-emitter (`(ElemType*)roxy_list_get` / `roxy_map_get`, with the primitive-key
   temp; the result local is pointer-tracked, declared `ElemType* vN`). The
   `gen_lvalue_addr` `ExprIndex` case emits it for List/Map containers, typed at
   the subscript's (borrowed) element type; **no reload** is needed (the address
   *is* the storage, unlike the stack-slot copy used for primitive-local inout).
   Functional tests (`bump(inout xs[i])`, `out`, wide `i64`, struct elements, and
   `inout map[k]`) pass on both backends. Sound for the single-arg case; the
   adversarial case waits on the Phase 3 pin.
3. **Call-site wiring — DONE.** `gen_call_expr` brackets a call that has an
   `inout`/`out` container-index argument with `ContainerPin`/`ContainerUnpin` ops
   on a pinned copy of the container (new IR ops → `CONTAINER_PIN`/`UNPIN` opcodes
   0xE6/0xE7 + `roxy_container_pin`/`unpin`; a deferred `IRCleanupKind::Unpin`
   record releases the pin on exception unwind, narrowed to the call window like
   the receiver borrow). The mutation trap surfaces on the VM via the runtime-error
   flag (checked after `CALL_NATIVE`) and a `borrow_count` check in `delete_value`;
   `delete_value` / `roxy_*_delete` carry free-guards for owning-element containers
   (defensive — see Status). E2E tests on both backends: the functional cases
   (Phase 2) plus nested borrows `f(inout xs[i], inout xs[j])` balanced and
   in-place set allowed while borrowed; VM-only: a mid-call `push` of the borrowed
   container traps. The `heap_root_of_lvalue` free-count wasn't needed for index
   lvalues — the buffer free happens *before* `object_free`, so a `ref_count` trap
   there is too late; the pin's `borrow_count` guards the buffer free directly.

### Alternatives considered

- **Copy-in / copy-out** — sound but not a true lvalue (copies; a concurrent read
  sees the stale value). Rejected in favour of real lvalue support.
- **True address with no pin** — dangles on a mid-call realloc. Unsound.
- **Reuse `ref_count` for the mutation guard** — breaks legitimate
  mutate-through-`ref List` (`fill(r: ref List){ r.push() }`). Hence the dedicated
  `borrow_count`.

## Related docs

- [memory.md](memory.md) — reference types, slab allocator, generations, and the
  constraint-reference model this completes.
- [overview.md](../overview.md) — reference-type philosophy; the `out`/`inout`
  restrictions the second-class family shares.
- [methods.md](methods.md) — `self` as the receiver (second-class here).
- [closures.md](closures.md) — `self` capture modes; `AssertHeap` is the
  promotion gate (§6).
- [coroutines.md](coroutines.md) — state-struct promotion referenced in §8.
