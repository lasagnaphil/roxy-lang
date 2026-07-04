# Lifetime Audit — Remaining Work (handoff)

> **Status:** all nine findings are fixed on the `lifetime-regression-tests`
> branch. This file can be retired — the regression cases now live (un-decorated,
> passing) in `E2E Lifetime Regressions`, with richer per-feature coverage folded
> into `E2E Exceptions`, `E2E Strings`, and `E2E Globals`. See
> [lifetimes.md](lifetimes.md) for the model.

A 2026-07-04 audit of Roxy's constraint-reference / value-lifecycle model found
**nine** soundness holes not covered by the (then all-passing) test suite. Each is
encoded as a regression case in `tests/e2e/test_lifetime_regressions.cpp` (suite
**`E2E Lifetime Regressions`**), asserting the *correct* behavior. **All nine are
now fixed.**

## How the regression tests work

Each case asserts the sound behavior and carries `doctest::should_fail()`, so the
suite stays green while a bug is open (an expected failure counts as a pass). When
you fix a bug, its case starts genuinely passing and `should_fail` flips it to a
**red "unexpected pass"** — that is the signal to delete the decorator and update
this file. Do not delete the decorator before fixing.

```
cd build && ninja roxy_tests
./roxy_tests.exe --test-suite="E2E Lifetime Regressions"          # all
./roxy_tests.exe --test-suite="E2E Lifetime Regressions" -s --test-case="F8*"
```

Finding 9 is VM-observed; F9b uses a bespoke in-test harness (see its body)
because the naive repro can't be observed through the standard `VMBackend::run`.
(F8 no longer needs its bespoke no-teardown harness — now that the fix is in,
both sub-bugs are observable through `VMBackend::run`; see its body.)

## Status

| # | Bug | State |
|---|-----|-------|
| 1 | `inout list[i].field` silent use-after-free write | fixed `00c14e8` |
| 2 | tagged-union discriminant reassignment leaks/frees stale bytes | fixed `70d46ba` |
| 3 | ref local leaks borrow on exception unwind out of frame | fixed `3478df5` |
| 4 | coroutine ref local leaks borrow on mid-iteration destroy | fixed `3478df5` |
| 5 | `List<uniq>.copy()` shallow-copies owners → double free | fixed `4e0f3d1` |
| 6 | `List<ref>.copy()` / `Map<_,ref>.values()` under-count borrows | fixed `4e0f3d1` |
| 7 | `weak` deref emits no `WeakCheck` (silent dangling read) | fixed `3cc07a4` |
| 8 | global `ref`/`weak` uncounted; global `uniq` double-delete at shutdown | fixed (this branch) |
| 9a | caught-exception object never freed | fixed (this branch) |
| 9b | string temporaries never freed | fixed — strings reference-counted (this branch) |

All fixes verified on the VM and the C backend, with no regression: the full
compiler-free suite (`--test-case-exclude="*<C>*" --test-suite-exclude="E2E C
Backend"`) → 1257 cases, 0 failed; all `*<C>*` cases → 613, 0 failed; `E2E C
Backend` → 137, 0 failed.

Severity note: findings 1–2 were memory-unsafe (silent corruption / crash) and 3–8
were loud traps or under-counts (8 added a loud double-delete tripwire on the
shutdown path). **9 was the mildest class — memory-safe leaks.** None corrupted
memory.

9b scope: standalone strings and container (List/Map) string elements are
reference-counted and reclaimed; strings in *struct fields* are retained-on-store
(never dangle) but not released on struct drop, so they remain a bounded leak
(structs stay copyable/trivial). Fully reference-counting struct-embedded strings
is a documented follow-on — see [strings.md](strings.md) "Memory Management".

---

## Finding 8 — FIXED — globals now participate in the constraint-reference model

Two related sub-bugs around module-level globals, both fixed entirely in the
shared init/shutdown IR (`IRBuilder::build_module_init` /
`build_module_shutdown` / `gen_delete_stmt`), so **both backends** inherit the fix
by lowering the same synthesized `__module_init` / `__module_shutdown` functions —
no `c_emitter.cpp` changes were needed.

**8a — a global `ref` held no count.** `var gr: ref T = gu;` never RefInc'd the
pointee, so `delete gu` didn't trap while `gr` borrowed it. **Fix:** in
`build_module_init`, after the initializer is stored, a `ref` global increments its
pointee (`emit_ref_borrow_inc`, skipping the handoff-source/adopt case like a `ref`
local); in `build_module_shutdown`, a `ref` global RefDecs it. Because init runs
before `main` and shutdown after, the count is held for the whole VM lifetime, and
reverse-declaration shutdown order releases the borrow *before* the owner's
`Delete`. `weak` globals stay uncounted (generational). `build_module_shutdown` now
also generates when the only lifecycle-relevant global is a (copyable) `ref`.

**8b — deleting a `uniq` global double-freed at shutdown.** `delete gu` in `main`
freed the object but left the slot pointing at it, so `__module_shutdown` freed it
again (debug double-delete tripwire; real double-free in release). **Fix:**
`gen_delete_stmt` now nulls a deleted `uniq` global's slot (`GlobalAddr` +
`StorePtr` of `ConstNull`), and the shutdown `Delete` is already null-guarded on
both backends, so it no-ops.

**Tests:** `F8*` in `E2E Lifetime Regressions` (VM: the 8a trap, the 8b
delete-once, and a global-`weak` sanity pair — no-block-on-delete + WeakCheck trap
on dangling deref) and a `TEST_CASE_TEMPLATE` in `E2E Globals`
("global ref is counted; deleted uniq global isn't double-freed") that runs the
non-trap paths on **both** backends. The delete-while-borrowed *trap* stays VM-only
(the C backend renders that trap as a raw abort, per the AOT-trap-reporting gap
below).

> Note: the old handoff advice to mirror inc/dec in `c_emitter.cpp`'s `main()`
> driver was unnecessary — the C backend emits `__module_init`/`__module_shutdown`
> from their IR, so the RefInc/RefDec/null-store ops flow through automatically.
> The [globals.md](globals.md) note that constraint references "just work" for
> globals is now true for borrows too.

---

## Finding 9a — FIXED — a caught exception object is now freed

`throw` heap-allocates the exception and the handled path handed the raw pointer to
the catch without freeing it → one leak per caught exception. **Fix:** the caught
exception is registered as an **owned local of the catch scope** (`gen_try_stmt`),
so the ordinary scope-cleanup machinery frees it exactly once on every catch exit
(fall-through, `return`, `break`, `continue`, and a *new* throw unwinding out of the
catch). A typed `catch (e: E)` frees it as `uniq E` (runs `E`'s destructor); a
catch-all `catch (e)` frees the memory type-erased via `emit_implicit_destroy`'s
`ExceptionRef` path (the caught type's `fun delete` does not run — same as the
unhandled path).

The re-throw / finally hazards the audit flagged are handled by an **in-flight
guard** rather than move-state bookkeeping: a re-throw (`throw e`, or a nested throw
while one is in flight) hands the object to the unwind machinery, and the free paths
refuse to free the in-flight object — VM: `object_free` / `delete_value` skip
`vm->in_flight_exception`; C backend: `emit_cleanup_records`' dispatch delete skips
`roxy_exception_current()`. So the catch scope's cleanup record firing during a
re-throw's unwind is a no-op for the re-thrown object; the eventual handler frees it
once. This makes `throw e`, `throw new` (frees old, unwinds new), conditional
re-throw, `finally`, and catch-all all correct with no per-path bookkeeping.

**Verified on both backends.** VM: F9a (the marker) plus `E2E Exceptions`
lifecycle cases (dtor-once, re-throw hand-off, new-throw, return, finally,
catch-all reclamation via slab counters). C backend: the same `E2E Exceptions`
`TEST_CASE_TEMPLATE`s. See [exceptions.md](exceptions.md) "Exception object
lifetime".

## Finding 9b — FIXED — strings are reference-counted

Dynamically created strings (concat / f-string / `substr` / `to_string`) used to
accumulate until VM teardown (F9b: ~600 live after a 200-iteration loop). **Fix
(chose option 1 — reference-count strings):** the header `ref_count` is repurposed
as a string OWNER count (strings are never `ref`-borrowed, so no clash with the
borrow free-trap); a copy retains, a drop releases, the last release frees. Pooled
literals are **immortal** (`ref_count == ROXY_STR_IMMORTAL`), because `LOAD_CONST`
/ AOT `roxy_string_from_literal` returns a persistent interned object — retain/
release are no-ops on them. Dynamic producers now allocate un-interned, owned
(count 1) via `roxy_string_new_owned`. `string` gains `needs_drop`/`needs_retain`
and `compute_drop_plan → DropKind::StrRelease`; `StrRetain`/`StrRelease` IR ops
(→ `STR_RETAIN`/`STR_RELEASE` opcodes / `roxy_string_*` C calls) are emitted at the
copy/drop sites, mirroring the `ref` machinery on both backends. See
[strings.md](strings.md) "Memory Management" and [lifetimes.md](lifetimes.md)
"Value lifecycle".

**Scope:** standalone strings and container (List/Map) string elements are
reclaimed; strings in *struct fields* are retained-on-store (never dangle) but not
released on struct drop, so they stay a bounded leak — structs remain copyable and
trivial (a synthesized destructor for a string field would make every string-bearing
struct move-only, breaking value semantics). Fully reference-counting struct-embedded
strings (the copyable-aggregate drop/retain glue) is a documented follow-on.

**Tests:** F9b (VM slab-counter) un-decorated + a `List<string>` reclamation
counter; behavioral copy/reassign/return/`List<string>` soundness cases in
`E2E Strings` on both backends; the string unit predicate in
`tests/unit/test_lifecycle_predicates.cpp`.

---

## Related lower-severity items the audit flagged (not in the 9) — all FIXED

Surfaced during the audit; all three are now fixed.

- **`roxy_list_pop` is not `borrow_count`-guarded — FIXED.** `roxy_list_pop`
  (`roxy_rt.cpp`) now returns `nullptr` when `list_mutation_blocked`, mirroring
  `roxy_list_push`, so a `pop` while an element is borrowed (`inout`/`out`) traps
  instead of dropping/reusing the borrowed slot — matching the
  [lifetimes.md](lifetimes.md) soundness table. Test: "mid-call pop of a borrowed
  List traps" in `E2E Lifetimes` (VM, next to the push-trap case).
- **AOT `roxy_ref_dec` swallowed underflow / release `roxy_free` leaked
  silently — FIXED (recording).** Both now route through the fatal trap channel
  (`roxy_runtime_error_set`) for parity with the VM's `object.cpp` reports:
  `roxy_ref_dec` asserts loudly in debug and records "ref_dec: reference count
  already zero" in release; `roxy_free` records "Cannot delete: object has active
  borrows" while still refusing the free (leak, not UAF). *Surfacing* this channel
  on the AOT path (checking it after each dec/free) remains the deferred
  "AOT trap reporting" work — the recording is its foundation and is inert under
  complete balancing (verified: 0 firings across the full C-backend suite).
- **`f(self)` to a `weak` param, and `[weak self]`/implicit `[ref self]` on a
  *noncopyable value-struct* stack receiver, skipped the `AssertHeap` promotion
  gate — FIXED.** The `needs_heap_check` computation (`semantic.cpp` self-capture
  analysis) assumed "noncopyable ⇒ heap"; a value-struct with a `fun delete` is
  noncopyable yet stack-capable, so the gate was skipped and a stack receiver
  would snapshot a bogus generation / borrow a bogus header. The gate now keys off
  *whether the source is a bare `self`* (outermost capture / non-nested), not
  copyability: implicit `[ref self]` uses `needs_heap_check = (target_idx == 0)`
  and `[weak self]` uses `!nested`. Separately, the `f(self)`-to-`weak`-param
  call-site gate in `ir_builder.cpp` (which previously fired only for `ref`
  params) now also fires for `weak` params, checking the raw `self` pointer
  *before* the `maybe_wrap_weak` `WeakCreate`. Probing the rest of the self→weak
  promotion family (not in the audit's example list) turned up three more ungated
  binding sites with the identical root cause — `var w: weak T = self`, `w = self`,
  and `Box { w = self }` all snapshot a bare `self` on a stack receiver without a
  gate. Fixed uniformly by teaching `maybe_wrap_weak` to take the source `Expr*`
  and emit the `AssertHeap` when it `is_bare_self` (call-arg and capture sites pass
  `nullptr` — they gate separately); `return self`-as-weak was already gated. Tests:
  "self promotion heap gate on noncopyable stack receivers" and "self to weak
  binding-site heap gate on noncopyable stack receivers" in `E2E Closures` (trap
  cases + uniq-receiver positive controls; the capture/param traps verified
  load-bearing — they silently succeed without the fix).

---

## Workflow notes for a fresh session

- **Branch:** `lifetime-regression-tests` (6 fix commits + the regression-suite
  commit on top of `main`). `git log --oneline main..HEAD`.
- **Build:** clang-cl + Ninja under `build/`. `cd build && ninja roxy_tests`. The
  `roxy` CLI target is `ninja roxy`.
- **Do NOT trust the `roxy.exe` CLI for runtime results** — it drives the
  multi-module `Compiler` linking path, which diverges from the test harness
  (single-module `compile()`): it segfaults on *passing* coroutine tests and
  showed a spurious `List<ref>.copy()` underflow that the harness and C backend
  run clean. Verify language behavior through the doctest harness
  (`VMBackend::run`), not the CLI. (The CLI has a real, separate, pre-existing
  linking bug worth its own investigation.)
- **Ad-hoc verification recipe:** add a temp `tests/e2e/test_*.cpp` with
  `TEST_CASE`s using `VMBackend::run` / `CBackend::run` (or `compile()` for
  compile-error checks), add its path to the `roxy_tests` target in
  `CMakeLists.txt`, `cmake . && ninja roxy_tests`, run with a `--test-suite=` /
  `--test-case=` filter (one process per case if any might abort), then revert the
  file + CMake line. Watch the `List<...>=List<...>` gotcha: `>=` lexes as one
  token — always write `List<T> = List<T>()` with spaces.
- **C-backend tests** invoke the system compiler → run outside the sandbox
  (`dangerouslyDisableSandbox: true`). Filter `--test-case="*<C>*"`. ASAN is
  disabled (see CLAUDE.md).
- **Sanity full run** (in-sandbox, compiler-free):
  `./roxy_tests.exe --test-case-exclude="*<C>*" --test-suite-exclude="E2E C Backend"`.
