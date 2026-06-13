# Lifetime & Borrow Soundness

> **Status:** Design / proposed, not yet implemented. This document specifies the
> intended *constraint-reference* model and how to make it sound and complete.
> It supersedes [memory.md](memory.md)'s description where the two differ:
> memory.md states the same model but describes its current — incomplete and
> unsound — implementation.

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
several free paths. This document specifies the complete version.

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

## 7. How the findings are resolved

- **Finding 1** (`var b: ref = owner; consume_and_free(owner); use(b)`): creating
  `b` increments owner's count to 1. The callee frees it → `object_free` sees
  count 1 → **traps** at the free.
- **Finding 2** (`return r;` borrowing a local owner): the returned `ref` carries
  its count, so the local owner's RAII drop sees count 1 → **traps** at the drop.
- **Mid-call alias-kill** (`evil(l[0], inout l); list.clear()`): `l[0]` is a
  counted borrow of the element; `clear()` tries to free it → count 1 → **traps**.
- **Mid-call receiver kill** (`heap_obj.method()` whose body reaches and frees the
  object): the call site counted the heap receiver for the call's duration →
  the free sees count 1 → **traps**.
- **Finding 3**: a cleanup bug, fixed independently (§9).

## 8. Interactions

- **Methods / `self`:** `self` is a second-class receiver borrow (§3). A method
  call on a statically-heap receiver counts that receiver for the call (so an
  alias-kill of it mid-method traps); on a stack receiver it counts nothing
  (downward-safe); on an already-`ref` receiver the existing count covers it.
  Returning or storing `self` is a promotion (§6).
- **Closures:** captures are by copy. Capturing a first-class `ref` copies it →
  increments; the env's destructor decrements. `[ref self]` / `[weak self]` are
  promotions (§6): slab-range test at capture, trap on stack receivers.
- **Coroutines:** a coroutine's parameters and promoted locals live in the heap
  state struct — so a `ref` parameter of a coroutine is a first-class counted
  borrow, incremented into the state struct at init and decremented when the
  coroutine is destroyed. Deleting the borrowed owner while a suspended coroutine
  still holds the borrow traps.
- **Containers:** `List<ref T>` / `Map<_, ref T>` hold counted borrows — push
  increments, and pop / remove / overwrite / container-destroy decrement (the
  element-cleanup machinery learns that `ref` elements are count-bearing).
  Deleting an owner while a container still borrows it traps; clear the container
  first.
- **`borrowed T`** ([memory.md](memory.md)): a subscript on a heap-pointee
  element (`List<uniq T>`) yields a counted `ref` to the pointee (realloc moves
  the buffer, not the pointee, so the borrow stays valid). A subscript on an
  **inline** element (`List<Vec2>`) is a second-class borrow (the buffer has no
  per-element header to count), expression-scoped, and the container may not be
  mutated while it is live (realloc would move the element).
- **`out` / `inout`:** the second-class family alongside `self`. An argument
  rooted in a heap object (`bump(inout b.a)`) counts that root for the call, so a
  mid-call alias-kill traps; a stack-rooted argument counts nothing and is safe by
  downward flow.
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

## 13. Phased implementation

1. **Complete the count.** Make `ref` copy = inc / drop = dec; emit `RefDec` for
   `ref` bindings on all exit paths via the cleanup machinery; hand off the count
   on return. Move the free-trap into `object_free` so every free path checks it;
   add `resolve_header` for interior borrows.
2. **Second-class family + call-site root counting.** Enforce the downward-only
   rule for `out`/`inout`/`self`; count statically-heap receiver/arg roots for the
   call's duration; add the promotion gate (§6) replacing `ASSERT_HEAP`.
3. **Containers & coroutines.** Count `ref` elements in `List`/`Map` cleanup;
   count `ref` parameters into coroutine state structs. Apply the index-set fix
   (§9).
4. **Elision (optimization).** Remove provably-redundant inc/dec pairs (§11).
   Pure optimization over an already-sound system.

## 14. Files (anticipated)

| File | Change |
|------|--------|
| `src/roxy/vm/object.cpp` | free-trap in `object_free` (every path); `ref_dec` underflow tripwire kept |
| `src/roxy/vm/interpreter.cpp` | `RefDec` on all cleanup/unwind paths; call-site root inc/dec; promotion test |
| `include/roxy/rt/slab_allocator.hpp` / `.cpp` | `resolve_header(ptr)` from interior pointers; sorted range index for large objects; free-trap honored in the vtable free |
| `include/roxy/rt/roxy_rt.h` / `src/roxy/rt/roxy_rt.cpp` | free-trap in `roxy_free`; container element dec on cleanup; `roxy::ref<T>` → borrow handle (copy inc / drop dec, never frees) |
| `src/roxy/compiler/semantic.{hpp,cpp}` | heap-only `ref` creation; second-class enforcement for `out`/`inout`/`self`; promotion sites; reject stored/returned/captured stack borrows |
| `src/roxy/compiler/type_checker.cpp` | remove the unchecked `uniq → ref` return path (it now relies on the count) |
| `src/roxy/compiler/ir_builder.cpp` | inc on ref create/copy/push/capture and call-site heap roots; dec via cleanup records on all paths; count hand-off on return; `gen_assign_index` cleanup fix |
| `src/roxy/compiler/lowering.cpp` | cleanup-record emission for `ref` decs; elision (Phase 4) |
| `tests/e2e/test_heap.cpp` (+ new suite) | regressions for the three findings; delete-while-borrowed traps; return-escape trap; mid-call alias-kill and receiver-kill traps; second-class rejection; promotion trap; interior-pointer counting |

## Related docs

- [memory.md](memory.md) — reference types, slab allocator, generations, and the
  constraint-reference model this completes.
- [overview.md](../overview.md) — reference-type philosophy; the `out`/`inout`
  restrictions the second-class family shares.
- [methods.md](methods.md) — `self` as the receiver (second-class here).
- [closures.md](closures.md) — `self` capture modes; `AssertHeap` is the
  promotion gate (§6).
- [coroutines.md](coroutines.md) — state-struct promotion referenced in §8.
