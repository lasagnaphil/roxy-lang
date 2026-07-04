# Lifetime Audit — Remaining Work (handoff)

> **Status:** working handoff note for the `lifetime-regression-tests` branch.
> Delete this file once finding 9b is fixed (9a is done) and the regression suite
> is folded into the permanent suites. See [lifetimes.md](lifetimes.md) for the
> model.

A 2026-07-04 audit of Roxy's constraint-reference / value-lifecycle model found
**nine** soundness holes not covered by the (then all-passing) test suite. Each is
encoded as a `doctest::should_fail()` regression case in
`tests/e2e/test_lifetime_regressions.cpp` (suite **`E2E Lifetime Regressions`**),
asserting the *correct* behavior so it fails until fixed. **Findings 1–8 and 9a
are fixed** on this branch; **only 9b (string temporaries) remains.**

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
| **9b** | **string temporaries never freed** | **OPEN (design decision)** |

All fixes verified on the VM and (where the backend can run it) the C backend, with
no regression in the full compiler-free suite (`--test-case-exclude="*<C>*"
--test-suite-exclude="E2E C Backend"` → 1256 cases, 0 failed; the remaining 1
failed *assertion* is the F9b marker).

Severity note: findings 1–2 were memory-unsafe (silent corruption / crash) and 3–8
were loud traps or under-counts (8 added a loud double-delete tripwire on the
shutdown path). **9 is the mildest class — memory-safe leaks.** None corrupts
memory.

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

## Finding 9b — string temporaries are never reclaimed (`... / F9b ...`)

Dynamically created strings (`str_concat`, f-string interpolation, `substr`,
`to_string`) allocate heap objects that are **never freed** — they accumulate
until VM teardown. F9b measures live objects via the slab counters
(`vm.allocator->total_allocated - total_tombstoned`) after a 200-iteration
string-building loop; today ~600 objects are live, 0 tombstoned.

### Root cause

Strings are **copyable value types with no destructor**, so RAII never frees them.

- The string type registers a **null destructor**: `string.cpp:12`
  (`register_object_type("string", 0, nullptr)`).
- `roxy_string_concat` / `_substr` / `_from_code` / `*_to_string` all allocate via
  `roxy_string_from_literal` → `roxy_alloc` with `ref_count = 0`
  (`roxy_rt.cpp:311`, `:367`, `:426`, ...). Interning (`roxy_string_from_literal`
  probes the intern table) dedups identical *content* but does not bound
  distinct-content growth (e.g. `f"{i}"` per iteration).

Note: [strings.md](strings.md) calls strings "reference-counted", which is
**misleading** — nothing reference-counts or frees them. Fixing this finding
should also correct that doc (or the doc should be corrected if the design is
intentionally leak-until-teardown).

### Fix approach (design decision required)

This is the most open-ended finding — it's a design choice, not a localized bug:

1. **Reference-count strings** (make them non-trivially-copyable like `ref`):
   copy → RefInc, scope-exit / overwrite → RefDec, free at zero. This is the
   biggest change (strings become lifecycle-nontrivial everywhere) but matches
   the doc's current wording and the value-lifecycle model
   ([lifetimes.md](lifetimes.md) "Value lifecycle").
2. **Arena / generational scratch** for short-lived string temporaries, freed at
   well-defined points. Cheaper but coarser.
3. **Accept and document** as a bounded leak (interning caps duplicate growth),
   and correct [strings.md](strings.md). Then F9b becomes a design assertion to
   drop rather than a bug to fix.

Pick the direction before writing code. Option 1 composes with the existing
`compute_drop_plan` / `is_trivial()` machinery (a string would gain
`needs_drop`/`needs_retain`), so it's the "correct" fix if string churn matters
for the target workloads.

---

## Related lower-severity items the audit flagged (not in the 9)

Surfaced during the audit; not yet ticketed as regression tests. Worth folding in:

- **`roxy_list_pop` is not `borrow_count`-guarded** (`roxy_rt.cpp`, `roxy_list_pop`)
  though [lifetimes.md](lifetimes.md) and the soundness table say pop must
  mutation-trap while an element is borrowed. Low severity (pop doesn't
  realloc/free the buffer), but a real doc/impl divergence. Add the guard
  (mirror `roxy_list_push`).
- **AOT `roxy_ref_dec` swallows underflow** (`roxy_rt.cpp:275`) whereas the VM
  reports it (`object.cpp` `ref_dec`); and **release-mode `roxy_free` silently
  leaks** on a live borrow instead of reporting (`roxy_rt.cpp` `roxy_free`). Part
  of the deferred "AOT trap reporting" work.
- **`f(self)` to a `weak` param, and `[weak self]`/implicit `[ref self]` on a
  *noncopyable value-struct* stack receiver, skip the `AssertHeap` promotion
  gate.** `needs_heap_check` is set only for *copyable* structs
  (`semantic.cpp`, self-capture analysis), on the assumption noncopyable ⇒ heap —
  but a value-struct with a destructor is noncopyable yet can be a stack local.
  Now that finding 7 makes `[weak self]` a real `WeakCreate` snapshot, a stack
  receiver would snapshot a bogus "generation" from stack bytes; the promotion
  gate should trap it. Verify against intended behavior and add a case.

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
