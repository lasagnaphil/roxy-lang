# Lifecycle Traits (design)

A unified, trait-based protocol for value lifecycle — drop, copy, move, clone —
resolved **statically via monomorphization** and eliminated entirely for trivial
types. The goal is to replace the current per-construct special-casing (the
`BCDeleteDesc` runtime descriptor, the container `value_is_ref` flag, the
move-only bit on `Type`) with one mechanism, **without** introducing runtime
vtables.

**Status:** migration in progress (see §12). **Done:** step 1 — the structural
predicates (`is_copy`/`needs_drop`/`needs_retain`/`is_trivial` on `Type`); step 2
(C backend) — container drops factored into per-type `roxy_drop__<T>` glue
functions, gated by `needs_drop()`. **VM step 2 is a no-op by design:** the native
`delete_value`/`BCDeleteDesc` walk already *is* the VM's drop-glue executor, and
replacing it with interpreted bytecode glue would be slower (§10 correction).
Step 2a — one shared `compute_drop_plan(Type)` deriving the drop *kind*, consumed
by both backends (VM lowers it to `BCDeleteDesc`, C to glue/dtor calls), plus a
shared `member_needs_drop` condition — is done (§10a). Step 3 — `ref` struct fields
are now allowed as move-only counted borrows (§12 step 3), closing a real gap (the
language previously banned them). Step 4 (partial) — the `Map.remove`/`Map.clear`
`uniq`-value leak is closed via call-site IR cleanup (§12 step 4). **Remaining:**
`m.insert` replace cleanup, the `Copy`-marker rename + forward-looking
`copy_init`/retain glue, and steps 5–6. This formalizes machinery
that already exists in scattered form (`fun delete T()` ≈ `Drop`, `.copy()` ≈
`Clone`, `Type::noncopyable()` ≈ the `Copy` marker, `build_delete_desc` ≈ derived
drop glue) into a single trait-resolved protocol, and specifies the lowering that
makes it zero-cost. Supersedes the ad-hoc lifecycle handling described across
[memory.md](memory.md) and [lifetimes.md](lifetimes.md).

```
Source → … → Semantic (resolve lifecycle traits) → IR Builder (emit glue) → … → {VM, C}
```

## 1. Motivation

Roxy's value lifecycle is currently handled by several unrelated mechanisms, each
specialized to one construct:

- **Destruction** is a runtime-interpreted descriptor tree. `build_delete_desc(T)`
  (`lowering.cpp`) produces a `BCDeleteDesc`; the VM *walks* it at runtime in
  `delete_value` (`interpreter.cpp`), while the C backend *generates code* from it
  in `emit_typed_delete` / `emit_delete_slot` (`c_emitter.cpp`). Two code paths
  for one concept, and the VM pays an interpreter loop per drop.
- **Container element lifecycle** is bespoke: `List` acquires at the IR level
  (`push` emits `RefInc`), `Map` acquires at the runtime level (the `value_is_ref`
  flag on the map header gates `roxy_map_insert/remove/clear`). Two mechanisms for
  the same "the element is count-bearing" idea, and neither handles the general
  case — `Map.remove`/`Map.clear` still leak `uniq` values (see
  [lifetimes.md](lifetimes.md) §13).
- **Copy vs move** is a single boolean, `Type::noncopyable()`, with the copy and
  retain side effects emitted ad hoc at each bind/pass/return site.

The natural unification is "every type has `{acquire, release}` hooks that all
storage sites invoke." Implemented as a **runtime vtable** (a per-type function
table the container/runtime calls through), that would be correct but slow: an
indirect call per element even when the hook is a no-op, defeating inlining, and
penalizing the overwhelmingly common trivial types (`i32`, POD structs).

Roxy already has the tool that makes the vtable unnecessary: **monomorphization**.
`List<T>` is specialized per concrete `T` at compile time, so the element type's
lifecycle is *statically known at every instantiation*. We can therefore generate
the hooks as ordinary monomorphic code, dispatched statically, and delete them
when empty — the model used by Rust's "drop glue" and C++'s special member
functions, not by COM-style vtables.

## 2. Core idea: lifecycle is *derived, monomorphized glue*

The compiler synthesizes, per type, up to four operations:

| Operation | Meaning | Always-safe default |
|---|---|---|
| `drop(self)` | release owned/borrowed resources | nothing (trivial) |
| `copy_init(dst, src)` | **implicit** copy: `memcpy` + *retain* side effects | `memcpy` |
| `move_init(dst, src)` | transfer ownership + invalidate source | `memcpy` + null source |
| `clone(self): Self` | **explicit** deep, independent copy | `copy_init` |

The trait system's role is **not** to dispatch these at runtime. It is to:

1. let users **customize** `drop` and `clone`,
2. drive **structural auto-derivation** of all four for aggregates, and
3. feed a **triviality analysis** that *removes* the glue when it is empty.

Resolution runs through Roxy's existing static/monomorphized trait machinery (the
same path as `Printable`/`Hash`/`Eq`), so each lifecycle event lowers to a direct
call (inlinable) — or, for trivial types, to nothing at all.

## 3. The trait set

```roxy
trait Drop  { fun drop(self); }          // destructor — today's `fun delete T()`
trait Clone { fun clone(self): Self; }   // explicit deep copy — today's `.copy()`
trait Copy {}                            // marker: implicit copy permitted (else move-only)
```

Each maps onto machinery Roxy already has:

- **`Drop`** ⟵ the named destructor `fun delete T()`. `delete` becomes sugar for
  `fun T.drop(self) for Drop`. User-writable; auto-derived for aggregates.
- **`Clone`** ⟵ `.copy()` / the container copy constructor. User-writable;
  auto-derived (clone each field/element).
- **`Copy`** ⟵ the inverse of today's `Type::noncopyable()`. A marker that decides
  whether a bind/pass/return **copies** (runs `copy_init`) or **moves** (runs
  `move_init` + invalidates the source).

### The `Copy` + `Drop` wrinkle (`ref`)

In Rust, `Copy` and `Drop` are mutually exclusive. Roxy **cannot** adopt that rule,
because `ref` is implicitly copyable *and* lifecycle-nontrivial: copying a `ref`
must `ref_inc` the pointee and dropping it must `ref_dec` (the counted-borrow model,
[lifetimes.md](lifetimes.md) §3). So in Roxy:

- `Copy` means only "**implicit** copy is allowed" (vs move-only). It does **not**
  imply "trivially memcpy-able."
- A `Copy` type may still carry `copy_init` retain glue and `drop` release glue.
- **Trivial** types (no glue) are a *subset* of `Copy`.

"Retain on copy" is therefore **never hand-written**. It enters a type only by the
type *containing a `ref`*, and the compiler composes it automatically (§5). There
is no user-facing `Retain` trait — that would be the one piece tempting to make a
runtime hook, and it is exactly the piece we derive instead.

## 4. Builtin impls

The reference and container kinds have compiler-known lifecycle; users never write
these:

| Type | `Copy`? | `copy_init` retain | `drop` | `clone` |
|---|---|---|---|---|
| `i32`, `bool`, … | yes | — | — | `memcpy` |
| `ref T` | yes | `ref_inc(ptr)` | `ref_dec(ptr)` | `ref_inc(ptr)` (a copy of a borrow is another borrow) |
| `weak T` | yes | — (generation-based, no count) | — | `memcpy` |
| `uniq T` | **no** (move-only) | — | run `T::drop`, then `free` | deep: `alloc` + `clone` pointee |
| `List<E>` / `Map<K,V>` | **no** | — | drop each element, then free buffer | deep: alloc + `clone` each element |
| `Coro<T>` | **no** | — | run state-struct `drop`, free | (none — coroutines are not cloneable) |

User structs/enums get their impls **derived** (§5) unless they declare a custom
`Drop`/`Clone`.

## 5. Auto-derivation by structural composition

For an aggregate, each operation is the composition of the same operation over its
parts, in declaration order (drop in reverse, matching scope LIFO):

```
S::drop(self)        = for each field f (reverse order): F::drop(self.f)
S::copy_init(d, s)   = memcpy(d, s); for each field f with retain: F::copy_init(&d.f, &s.f)
S::clone(self): S    = S { f0 = F0::clone(self.f0), f1 = … }
S is Copy            ⟺ every field type is Copy
```

Containers compose over their (single, monomorphic) element type:

```
List<E>::drop(self)  = for i in 0..len: E::drop(&buf[i]); free(buf)
List<E>::clone(self) = let c = List<E>(); for i in 0..len: c.push(E::clone(&buf[i])); c
```

A custom `fun S.drop(self) for Drop { … }` **replaces** the derived drop for `S`'s
own resources but the compiler still runs derived field-drops afterward (as it does
today for `delete` + field cleanup), so a user destructor can't accidentally leak a
`uniq`/`ref` field.

## 6. The elimination engine (what makes it zero-cost)

A structural fixpoint computes three predicates per type:

```
is_trivial(T)   — copy is plain memcpy, drop is a no-op
needs_drop(T)   — drop glue is non-empty
needs_retain(T) — copy_init has side effects (contains a ref)

is_trivial(i32)            = true
is_trivial(struct S)       = ⋀ is_trivial(field)               (and no custom Drop/Clone)
needs_drop(ref T)          = true        needs_retain(ref T)   = true
needs_drop(uniq T)         = true        needs_retain(uniq T)  = n/a (move-only)
needs_drop(List<E>)        = true        (always frees the buffer)
needs_drop(struct S)       = ⋁ needs_drop(field)  ∨  has-custom-Drop
```

Mutually recursive types (`struct Node { next: uniq Node; }`) reach a fixpoint
because the recursion is behind `uniq`/container indirection, whose predicates are
constants (`needs_drop = true`) and don't depend on the pointee's predicate to
*terminate* the analysis. (Direct value cycles are already rejected — see
[recursive-types.md](recursive-types.md).)

When a predicate is false, the compiler **emits nothing**: no call, no loop, no
flag, no header bit. This is the property a vtable cannot provide — `vtable->drop`
is an indirect call even when `drop` is empty; monomorphized glue for `i32` *is*
the absence of code. `needs_drop` is the direct analogue of C++'s
`is_trivially_destructible`, and the optimizer treats trivial-element container
loops as dead code.

## 7. Placement vs definition

Two concerns that today are tangled get cleanly separated:

- **Definition** — *what* a type's glue is — is the trait system's job (§3–§6).
- **Placement** — *where* glue calls go — stays with the existing compiler
  machinery:
  - For **transparent** slots (locals, fields), the IR builder emits `copy_init` /
    `drop` at the precise program points, balanced across control flow by the
    current liveness machinery (the `m_ref_inc_pcs` / `m_nullify_pcs` /
    move-nulling logic, [lifetimes.md](lifetimes.md) §5). That logic is unchanged;
    it just calls *derived glue* instead of bespoke per-construct emit.
  - For **opaque** slots (container elements — storage the compiler can't address
    a write into), the container's monomorphized mutators carry the glue (§8).

The static **move checker** (use-after-move is a compile error; moved sources are
nulled) is what lets drops be guarded by a simple null check rather than a
runtime drop-flag. It is orthogonal to this design and stays as-is.

## 8. Containers stop being special

A `List<T>` / `Map<K,V>` is monomorphic, so its element lifecycle is generated glue
specialized per instantiation. Keep the hand-tuned **type-erased buffer engine**
(alloc/grow/probe over byte slots — mechanics that genuinely don't depend on `T`),
but **lift element lifecycle out of the runtime into monomorphic wrappers** the
compiler generates around the raw ops:

```
Map<K, ref P>::clear()  ⇒  for each live bucket b: ref_dec(value_at(b));   roxy_map_clear_raw(self)
Map<K, i32>::clear()    ⇒  roxy_map_clear_raw(self)            // element glue empty → just the raw clear
List<ref P>::push(r)    ⇒  ref_inc(r);  roxy_list_push_raw(self, r)
List<i32>::push(v)      ⇒  roxy_list_push_raw(self, v)
```

Consequences:

- The **`value_is_ref` flag disappears** — replaced by "the monomorphized
  `clear`/`remove`/`insert` for a `ref`-valued map inlines `ref_dec`/`ref_inc`; for
  a trivial-valued map it inlines nothing."
- `List`'s IR-level acquire and `Map`'s runtime-flag acquire **converge** on one
  mechanism (generated glue), removing today's inconsistency.
- The **`uniq`-value `remove`/`clear` leak closes for free**: the element's derived
  `drop` glue is `typed_delete`, so a generated `remove`/`clear` runs it.
- The earlier "`clear` can't be unrolled in the IR" objection dissolves — the glue
  loop *is* codegen (the same kind already emitted for the C backend's noncopyable
  map cleanup, the `__map_iter_*` internals).

(If the containers were instead written **in Roxy** as generic types, the glue
would be fully automatic with no wrappers — a cleaner end state, but it requires the
language to express the buffer engine. Start with the wrapper approach.)

## 9. One runtime pointer survives — and it is not a vtable

Monomorphization can't specialize a site whose static type is genuinely erased:

- a **closure environment** dropped by `type_id` (already the case —
  [closures.md](closures.md)),
- a future **`dyn Trait`** object or other type-erased value.

For these, store **a single `drop_glue` function pointer** in the object header
(where the closure env's `type_id`→destructor dispatch already lives). That pointer
is the monomorphized whole-value drop glue for the concrete type, generated once.
It is **one pointer for one operation at a true erasure boundary** — not a
per-operation method table consulted on the hot path. Static dispatch everywhere it
can be; one fn-pointer only where the type is actually unknown.

## 10. Lowering — and backend unification

Each backend keeps the *execution* mechanism that's efficient for it; what unifies
them is the *derivation* (one description of "what is T's drop", §10a):

- **AOT/C:** emit a C glue function per type (`roxy_drop__<T>`; structs already use
  `$$delete`); the C compiler inlines and ICF-folds them. Native code, generated
  per monomorphic type. ✅ implemented (container glue; structs already glue).
- **VM:** **keep** the native, data-driven `delete_value` walk over `BCDeleteDesc`
  — that *is* the VM's drop-glue executor. Every `Delete` already routes through
  this one native function; nothing is inlined per site, so there is nothing to
  "factor out." Crucially, the VM cannot emit *native* glue (no C compiler), so
  literal bytecode glue would run a drop loop through the interpreter dispatch —
  **slower** than the native walk it replaces, plus the cost of synthesizing a
  `BCFunction` per type and the bytecode bloat. So the VM is already at this step's
  end state.

> **Correction (found during implementation):** an earlier draft said the VM would
> "emit a bytecode glue function per type … no longer interprets `BCDeleteDesc` …
> strictly faster." That is wrong: interpreted bytecode glue is *slower* than the
> native descriptor walk. The native `delete_value`/`BCDeleteDesc` executor is the
> correct, permanent VM mechanism. `BCDeleteDesc` is therefore **not eliminated**;
> the goal becomes making it (and the C glue) consume one shared *derivation*.

## 10a. The real unification: one derivation, two executions

The actual special-casing was the **dual derivation**: `build_delete_desc` (VM) and
`emit_typed_delete` (C) each independently re-derived "what does T's drop consist
of" from `Type`. That is exactly why the `Map<_, ref V>` cases needed parallel
fixes in both. **✅ Implemented:** a single backend-agnostic
`compute_drop_plan(Type) -> DropPlan` (`types.{hpp,cpp}`) makes that decision once
— `DropKind` (None/CallDtor/WalkFields/List/Map/Closure/RefDec) + `free_obj` +
the involved types — and both backends lower it:

- VM (`build_delete_desc`) lowers the plan to `BCDeleteDesc` (resolving a dtor to
  its bytecode fn index, recursing into element/field types) and executes it
  natively in `delete_value`.
- C (`emit_typed_delete`) lowers the plan to a `roxy_drop__<T>` glue call or a
  `$$delete` call (resolving a dtor to its C symbol). For C, `WalkFields` and
  `CallDtor` are identical — both call the struct's synthetic `$$delete`; only the
  VM honors the distinction (inline field-walk vs function call).

The divergence-prone container-member condition is likewise shared
(`member_needs_drop`). Both backends keep their efficient execution;
neither re-derives. This — not eliminating the descriptor — is the genuine
"containers/structs stop being special" step.

## 11. Tradeoffs

- **Code bloat** — one glue function per monomorphic type. Mitigations: (1)
  trivial-elimination means most types emit nothing; (2) **dedup by glue shape** —
  every `List<uniq *>` has structurally identical drop glue, so emit once and share;
  the C linker's identical-code-folding handles the residue; (3) inline tiny glue
  (a single `ref_dec`) at the call site instead of emitting a function.
- **Compile-time analysis** — the `needs_drop`/`is_trivial` fixpoint is cheap
  (structural, one pass to fixpoint) and is needed anyway to drive elimination.
- **Where containers live** — native engine + generated glue wrappers (less churn)
  vs containers-in-Roxy (cleanest). Recommend the former first.
- **Custom `Drop` + derived field-drop ordering** — must run the user body, then
  derived field-drops, matching today's `delete` semantics; documented so a custom
  `drop` can't strand a `uniq`/`ref` field.

## 12. Migration path from `BCDeleteDesc`

Incremental, each step independently testable:

1. **Introduce the predicates.** Add `is_trivial` / `needs_drop` / `needs_retain`
   to `Type` (or a side analysis). No behavior change; assert they agree with
   `Type::noncopyable()` and the current descriptor's emptiness.
2. **Generate drop glue (C); keep the VM's native executor.** ✅ C backend: container
   drops factored into per-type `roxy_drop__<T>` functions, `emit_typed_delete`
   gated by `needs_drop()`. VM: **no change** — `delete_value`/`BCDeleteDesc` is the
   VM's drop-glue executor and stays (bytecode glue would be slower; see §10).
   *(The cross-check assert from step 1 keeps `needs_drop()` consistent with the
   descriptor.)*
2a. ✅ **Unify the derivation (the real de-special-casing).** `compute_drop_plan(Type)
   -> DropPlan` is consumed by both `build_delete_desc` (VM) and `emit_typed_delete`
   (C); `member_needs_drop` shares the element condition. "What is T's
   drop" is derived once (§10a) — removing the dual derivation that made
   `Map<_, ref V>` need parallel fixes. Both `WalkFields` and `CallDtor` retained.
3. ✅ **`ref` struct fields — move-only counted borrow.** Lifted the language
   restriction ("'ref' types cannot be used in struct fields"); a struct holding a
   `ref` field is now move-only (like `List<ref T>`) and counts the borrow:
   construction `ref_inc`s, drop `ref_dec`s, overwrite rebalances, a move transfers.
   The synthetic-destructor pass (now driven by `member_needs_drop`, the
   non-recursive member-cleanup predicate — `noncopyable() || ref`) makes such a
   struct move-only and gives it field-walk cleanup; the struct field-walk includes
   `ref` fields; the struct literal and field-assignment paths emit the counting.
   Both backends. **Deviation from the original §3 plan (deliberate):** we chose
   *move-only* over *copyable + retain glue*. Move-only reuses the proven
   noncopyable machinery, so copy/retain vanishes — only construct/overwrite/drop
   count, and a move just transfers. It's consistent with how `List<ref>`/`Map<_,ref>`
   already work. The `copy_init`/retain machinery (and the `needs_retain` predicate)
   stay forward-looking; the `Copy`-marker rename below is unblocked but not yet done.
4. ✅ (partial) **`Map.remove`/`Map.clear` `uniq`-value leak closed.** Instead of a
   runtime per-value callback (a VM trampoline), the value cleanup is emitted as
   ordinary IR at the call site, where the value type is statically known — so both
   backends get it with no new runtime, natives, or header fields: `m.remove(k)` →
   contains-guarded `delete m[k]` before the raw remove; `m.clear()` → a
   bucket-iteration delete-loop (reusing the pre-existing `__map_iter_*` natives)
   before the raw clear. `ref` values keep the `value_is_ref` runtime path.
   *Remaining:* `m.insert(k,v)` replace still leaks (the call machinery consumes the
   value arg before method-lowering, so the contains-guard branch would strand it;
   `m[k]=v` is the clean replace). The broader unification (fold `value_is_ref` and
   `List`'s IR-level `RefInc` into one generated element-glue mechanism) is still
   future work.
5. **Delete `BCDeleteDesc`** and the descriptor interpreter once both backends are
   on glue.
6. **Erased fallback.** Generalize the closure-env `type_id`→dtor pointer into the
   single `drop_glue` header pointer for all erased values.

Existing tests (`test_lifetimes.cpp`, `test_raii.cpp`, `test_recursive_types.cpp`,
the container suites) are the conformance suite; behavior must match at every step.

## 13. Relationship to existing traits

This reuses, rather than duplicates, the trait infrastructure: builtin-trait
registration and `for Trait` impls ([traits.md](traits.md)), monomorphization
([generics.md](generics.md)), the destructor (`delete`,
[constructors.md](constructors.md)), and the counted-borrow model
([lifetimes.md](lifetimes.md)). `Drop`/`Clone`/`Copy` join `Printable`/`Hash`/
`Eq`/`Exception` as builtin traits with structural auto-derivation.

## 14. Files (when implemented)

- `compiler/types.{hpp,cpp}` — `is_trivial`/`needs_drop`/`needs_retain` predicates;
  `Copy` replacing `noncopyable()`.
- `compiler/semantic.cpp` — resolve/derive `Drop`/`Clone`/`Copy`; bounds checking.
- `compiler/ir_builder.cpp` — emit `copy_init`/`drop`/`move_init` glue at transparent
  slots and around container ops (replacing the bespoke per-construct emit).
- `compiler/lowering.cpp` — remove `build_delete_desc`/`BCDeleteDesc`; generate VM
  glue functions.
- `vm/interpreter.cpp` — drop `delete_value`'s descriptor walk; call glue.
- `compiler/c_emitter.cpp` — drive `emit_typed_delete`/clone from trait resolution.
- `rt/roxy_rt.{h,cpp}` — split container mutators into raw-engine ops; remove
  `value_is_ref`; keep the buffer engine.
