# Lifetime & Borrow Soundness

> **Status:** Largely implemented (VM). The *constraint-reference* model below
> is live: `ref` is a fully-counted borrow, the free-trap is centralized across
> every free path, the three audited findings are fixed, the `out`/`inout`
> second-class rule and `[ref self]` capture counting are enforced, and
> **call-site counting of a `uniq` method receiver** is now in (§4/§6/§8 — it did
> *not* need polymorphic dispatch after all; a copy-prop-pinned receiver borrow
> gives the call-straddling count a distinct SSA identity). Still **deferred**:
> the rest of call-site heap-root counting (the `out`/`inout` root borrow, and
> receivers that root in a field / heap-returning temp), refcount elision (§11,
> always a later phase), and full AOT/C-backend parity beyond the `roxy_free`
> trap. See the per-item status in [§13](#13-implementation-status).
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
traps before the inc) and the env's destructor `RefDec`s it. Binding `self` into
a first-class `ref` or returning/storing it (the other promotions) reuse the same
gate but are not yet wired end-to-end.

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
  Returning or storing `self` is a promotion (§6). *(**Implemented** for a `uniq`
  identifier receiver — §13. The borrow rides a copy-prop-pinned `Copy` of the
  receiver so its RefDec + Nullify cleanup own a distinct SSA value/register and
  can't clobber the owned-local's Delete record; the borrow's exception record is
  appended after the owned-local records so it RefDecs *before* the owner's
  Delete on unwind. Stack / `ref` receivers are correctly skipped.)*
- **Closures:** captures are by copy. Capturing a first-class `ref` copies it →
  increments; the env's destructor decrements. `[ref self]` / `[weak self]` are
  promotions (§6): slab-range test at capture, trap on stack receivers.
- **Coroutines:** a coroutine's parameters and promoted locals live in the heap
  state struct — so a `ref` parameter of a coroutine is a first-class counted
  borrow, incremented into the state struct at init and decremented when the
  coroutine is destroyed. Deleting the borrowed owner while a suspended coroutine
  still holds the borrow traps. *(**Not yet implemented** — §13.)*
- **Containers:** `List<ref T>` / `Map<_, ref T>` hold counted borrows — push
  increments, and pop / remove / overwrite / container-destroy decrement (the
  element-cleanup machinery learns that `ref` elements are count-bearing).
  Deleting an owner while a container still borrows it traps; clear the container
  first. *(**Not yet implemented** — §13. Note the related, separate gap that
  `Map.remove` / `Map.clear` don't yet destroy noncopyable values at all.)*
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
  were already blocked by the type system (no value→reference conversion).
  *Deferred:* the call-site root counting (§13).
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
  interpreter) is fixed. Gated to `uniq` *identifier* receivers; `ref` receivers
  are skipped (already counted), stack receivers skipped (second-class).
- **Deferred — the rest of call-site heap-root counting.** The `out`/`inout`
  root borrow (`bump(inout heap_obj.field)`), and receivers that root in a field
  (`a.b.method()`) or a heap-returning temp (`make().method()`). The receiver
  mechanism above generalizes to these, but each needs its own gate. Binding /
  returning / storing `self` as a first-class `ref` (the non-capture promotions)
  are also not yet wired.
- *Done (separate fix):* **inout/out owning-pointer reassignment frees the
  overwritten value.** `slot = uniq T(..)` / `slot = nil` through an `inout`/`out`
  `uniq`/`List`/`Map`/`Coro` pointer now loads and Deletes the old value before
  the store, and consumes the RHS temp — previously it leaked the old object and
  double-owned the new one (the same class as the index-set fix, §9). This is
  also what makes the mid-call receiver kill testable (§8). *Still open, also
  found while testing and left for a separate fix:* a module-global `uniq`
  initializer doesn't run its constructor.

**Phase 3 — containers & coroutines. Not started.**
- Count `ref` elements in `List`/`Map` cleanup; count `ref` parameters into
  coroutine state structs. (Separate, related gap: `Map.remove`/`Map.clear` don't
  yet destroy noncopyable values.)

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
| `src/roxy/vm/interpreter.cpp` | `RefDec` cleanup-record kind in `execute_cleanup`; trap error propagation through `delete_value`/`DELETE`; retired `DEL_OBJ` pre-check; `BCDeleteDesc::Closure` env-dtor dispatch |
| `include/roxy/vm/bytecode.hpp` | `BCCleanupKind`, `BCDeleteDesc::Closure`, `BCTypeInfo.dtor_func_idx` |
| `include/roxy/compiler/ssa_ir.hpp` | `IRCleanupKind`, `IRCleanupInfo.whole_function_scope`; `IRInst.no_copy_prop` (pinned copy) + `IRCleanupInfo.call_borrow` (receiver-borrow start-narrowing) |
| `src/roxy/compiler/ir_optimize.cpp` | copy propagation skips `no_copy_prop` copies |
| `src/roxy/compiler/ir_builder.{hpp,cpp}` (receiver counting) | `emit_pinned_copy`; `gen_call_member` emits the `uniq`-receiver borrow (PinnedCopy → RefInc → Call → RefDec → Nullify); deferred `m_call_borrow_cleanups`, flushed after owned-local records in `end_function_body` |
| `src/roxy/compiler/ir_builder.cpp` (inout reassign) | `gen_assign_local` pointer-param branch destroys the old value + consumes the RHS temp before storing through an `inout`/`out` owning pointer |
| `include/roxy/compiler/lowering.{hpp,cpp}` (receiver counting) | `m_ref_inc_pcs`; `call_borrow` scope_start narrowing to the RefInc PC |
| `src/roxy/compiler/ir_builder.{hpp,cpp}` | `OwnedKind::RefBorrow`; ref-local inc/dec + reassign + return hand-off + caller-adopt convention; `[ref self]` capture RefInc + env-field RefDec; per-env synthesized destructors; call-result temp tracking; transitive-`[move]` field nullify; `gen_assign_index` cleanup fix |
| `src/roxy/compiler/lowering.cpp` | RefDec `BCCleanupRecord` + whole-function-scope param records + liveness pin; `Closure` delete desc; `BCTypeInfo.dtor_func_idx` |
| `src/roxy/compiler/semantic.{hpp,cpp}` | reject move-out of noncopyable `out`/`inout` (also closure move-capture); index-assign consume keyed off the container element type; env destructor for `ref` captures |
| `include/roxy/compiler/symbol_table.hpp`, `src/roxy/compiler/symbol_table.cpp` | `is_out_inout` flag on parameter symbols |
| `include/roxy/vm/vm.hpp`, `src/roxy/vm/vm.cpp` | `RoxyVM::closure_env_dtors` (env type_id → destructor index) |
| `tests/e2e/test_lifetimes.cpp` (new), `tests/e2e/test_closures.cpp`, `tests/unit/test_slab_allocator.cpp` | findings 1–3; delete-while-borrowed/return-escape traps; balance across control flow; `out`/`inout` escape rejection; `[ref self]` pin/release; closure capture cleanup; `resolve_header`; **`uniq`-receiver call-site borrow balance + survives-a-throwing-method** |

Deferred (not yet touched): call-site root inc/dec for the `out`/`inout` root
borrow and field / heap-temp receivers in `ir_builder.cpp` (the `uniq`-identifier
receiver case is done); `List`/`Map` ref-element counting and coroutine
`ref`-param promotion; `roxy::ref<T>` → borrow handle in `roxy_rt`; elision in
`lowering.cpp`.

## Related docs

- [memory.md](memory.md) — reference types, slab allocator, generations, and the
  constraint-reference model this completes.
- [overview.md](../overview.md) — reference-type philosophy; the `out`/`inout`
  restrictions the second-class family shares.
- [methods.md](methods.md) — `self` as the receiver (second-class here).
- [closures.md](closures.md) — `self` capture modes; `AssertHeap` is the
  promotion gate (§6).
- [coroutines.md](coroutines.md) — state-struct promotion referenced in §8.
