# Lifetime Audit — Remaining Work (handoff)

> **Status:** working handoff note for the `lifetime-regression-tests` branch.
> Delete this file once findings 8–9 are fixed and the regression suite is folded
> into the permanent suites. See [lifetimes.md](lifetimes.md) for the model.

A 2026-07-04 audit of Roxy's constraint-reference / value-lifecycle model found
**nine** soundness holes not covered by the (then all-passing) test suite. Each is
encoded as a `doctest::should_fail()` regression case in
`tests/e2e/test_lifetime_regressions.cpp` (suite **`E2E Lifetime Regressions`**),
asserting the *correct* behavior so it fails until fixed. **Findings 1–7 are
fixed** on this branch; **8 and 9 remain.**

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

Findings 8/9 are VM-observed; F8 and F9b use bespoke in-test harnesses (see their
bodies) because the naive repro aborts or can't be observed through the standard
`VMBackend::run`.

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
| **8** | **global `ref`/`weak` uncounted; global `uniq` double-delete at shutdown** | **OPEN** |
| **9** | **caught-exception object and string temporaries never freed** | **OPEN** |

All fixes verified on the VM and (where the backend can run it) the C backend, with
no regression in the full compiler-free suite (`--test-case-exclude="*<C>*"
--test-suite-exclude="E2E C Backend"` → 1249 cases, 0 failed; the remaining 3
failed *assertions* are the F8/F9a/F9b markers).

Severity note: findings 1–2 were memory-unsafe (silent corruption / crash) and 3–7
were loud traps or under-counts. **8 and 9 are the mildest class — memory-safe
leaks** (plus, for 8, one loud double-delete tripwire). Neither corrupts memory.

---

## Finding 8 — global `ref`/`weak` is uncounted (`E2E Lifetime Regressions` / `F8 ...`)

Two related sub-bugs around module-level globals.

**8a — a global `ref`/`weak` holds no count.** A global `var gr: ref T = gu;`
borrowing a global `uniq T` never RefIncs the pointee, so `delete gu` does *not*
trap while `gr` still borrows it — the borrow dangles.

```roxy
struct Owner { val: i32; }
var gu: uniq Owner = uniq Owner { val = 3 };
var gr: ref Owner = gu;
fun main(): i32 { delete gu; return 0; }   // should trap; currently succeeds
```

**8b — deleting a `uniq` global double-frees at shutdown.** `delete gu` inside
`main` frees the global, but the global isn't marked moved/deleted, so
`__module_shutdown` deletes it again → the debug double-delete tripwire aborts
(`interpreter.cpp:294`; in release it's a real double free). F8 avoids this by
*not* calling `vm_destroy` (so it can observe 8a cleanly) — see the test body.

### Root cause

Globals bypass the function-local ownership machinery entirely. They are not
entered into `m_owned_locals` / `m_move_states`, get no create-time inc and no
scope-exit dec, and `delete gu` is not tracked as a move.

- `SemanticAnalyzer::resolve_global_var` (`semantic.cpp:651`) resolves a global's
  type and initializer but applies **no** lifetime rule — a global `ref`/`weak`
  is accepted with no counting obligation.
- `IRBuilder::collect_globals` / `build_module_init` / `build_module_shutdown`
  (`ir_builder.cpp:379` / `:421` / `:467`) synthesize storage, init, and teardown.
  `build_module_init` evaluates initializers; `build_module_shutdown` destroys
  noncopyable globals in reverse. Neither RefIncs a `ref` global's pointee nor
  RefDecs it, and neither guards against a global already deleted in user code.
- The VM drives `__module_shutdown` at `vm.cpp:147` (gated on `!vm->error`).

See [globals.md](globals.md) for the globals model, and the note there that
constraint references were expected to "just work" for globals — they don't for
borrows.

### Fix approach

- **8a:** treat a global `ref`/`weak` like a `ref`/`weak` *field* of a
  persistent struct. In `build_module_init`, after storing a `ref` global's
  initializer, RefInc the pointee; in `build_module_shutdown`, RefDec it (weak
  globals need no counting — they're generational, so only 8a's `ref` case
  matters for the free-trap). Because init runs before `main` and shutdown after,
  the count is held for the whole VM lifetime, so `delete gu` traps while `gr`
  lives. Mirror the `ref`-field inc/dec sites (`emit_ref_inc`/`emit_ref_dec`,
  `emit_field_cleanup`).
- **8b:** make `delete <global>` mark the global as deleted so shutdown skips it.
  Cleanest: null the global slot on an explicit `delete` (the shutdown `Delete` is
  already null-guarded on both backends — cf. the coroutine `$$delete` and the
  C-backend typed delete), so a nulled global is a safe no-op at teardown. The
  write path is `gen_assign_local`'s global branch (`gen_global_read` /
  `GlobalAddr` machinery) and `gen_delete_stmt`.

### Gotchas

- Multi-module globals already share one `__module_init`/`__module_shutdown` name
  (a documented limitation in [globals.md](globals.md)); keep the fix single-module
  and don't regress that.
- Weak globals: `weak` needs no counting, but a global `weak` deref must still hit
  the finding-7 `WeakCheck` (it does — that's type-driven in `gen_get_expr`, not
  global-specific). Add a global-weak sanity case.
- The C backend emits real C globals (`g_<name>`); the init/shutdown bracketing in
  the generated `main()` must gain the same inc/dec (`c_emitter.cpp`
  `emit_global_definitions` / the `main()` driver).

---

## Finding 9a — a caught exception object is never freed (`... / F9a ...`)

`throw` heap-allocates the exception; after a `catch` handler completes normally,
nothing frees it → one leak per caught exception (bounded by VM teardown).

```roxy
struct E { code: i32; }
fun E.message(): string for Exception { return "boom"; }
fun delete E() { print("E dtor"); }              // never runs today
fun main(): i32 {
    try { throw E { code = 1 }; } catch (e: E) { print("caught"); }
    return 0;                                     // E should be freed here
}
```

F9a asserts the exception's destructor runs (`stdout` contains `"E dtor"`);
currently it does not.

### Root cause

- `throw` heap-allocates the exception struct: `IRBuilder::gen_throw_stmt`
  (`ir_builder.cpp:2749`) does `emit_new` + struct-copy + `Throw ptr`.
- The catch variable is bound as a **non-owning `ref`** (see `gen_try_stmt`,
  `ir_builder.cpp:2833`, and the exception-param typing), deliberately *not*
  counted — [lifetimes.md](lifetimes.md) "Applying the model → Coroutines" notes
  the catch param is excluded from ref counting.
- On the handled path, the unwinder only stores the pointer into the handler
  register and clears the in-flight slot — no free: `interpreter.cpp:2201–2208`.
  A free happens *only* on the unhandled path (`interpreter.cpp:2225`) and the
  double-throw path (`:2159`). The C backend mirrors this: `roxy_exception_take`
  (`roxy_rt.cpp` around `:67`) hands the raw pointer to the handler and never
  frees.

### Fix approach

Free the caught exception object when the catch clause finishes — on **every**
exit from the catch body (normal fall-through, `return`, `break`, `continue`, and
a re-throw from within the catch). Treat the caught exception like a scope-owned
value of the catch block: emit a typed `Delete` of the exception pointer at catch
exits, using the concrete caught type (or a type-erased free for the catch-all
`ExceptionRef`). Watch for:

- **Re-throw / nested throw inside a catch:** if the catch body throws, the
  original exception must still be freed (or its ownership handed to the new
  unwind) exactly once — don't double-free or leak on that path.
- **`finally`:** `finally` is realized by duplication per exit path; the free must
  sit on the catch path, not the finally, or it duplicates.
- **Catch-all (`ExceptionRef`):** no concrete type — use a type-erased free
  (`emit_typed_delete`'s type-erased path / `roxy_free` after running the header's
  `type_id`-driven destructor, as the unhandled path already does).
- The C backend's checked-return model (per-try `__dispatch_<id>` labels) needs the
  matching free after a caught dispatch; reuse `emit_typed_delete`.

This is the trickier of the two — get the re-throw and finally paths right.

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
