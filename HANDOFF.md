# Handoff: scope cleanup on the exception-unwind path

Working notes for the last open High Priority leak. **Delete this file when the
work lands** — it is session scaffolding, not a reference.

**The task summary lives in `TODO.md`** under High Priority, and the model it has
to obey is [`docs/internals/lifetimes.md`](docs/internals/lifetimes.md) →
"Counting mechanics" and "Applying the model". Read those first. This file is the
*operational* half: what is already done, what will bite you, and how to check
yourself.

---

## The one-paragraph summary

Cleanup on the **exception-unwind** path is wrong in both directions at once. A
struct temporary that was moved into the heap exception object is destroyed
anyway while unwinding (over-release → the handler reads a freed value), and a
value-struct local that owns a counted member is not destroyed at all
(under-release → leak). Ordinary scope exit is correct in both shapes; remove
the `throw` from either repro below and it is clean. This is most of what
`examples/lox` still leaks, because Lox implements `return` by throwing a
`ReturnException`, so **every Lox function return unwinds**.

---

## State

Everything below landed 2026-08-08 and is *not* part of this task — listed so a
regression here is not mistaken for one of these.

| Change | Commit |
|---|---|
| Coroutine suspended in `catch` frees the caught exception | `de097ed` |
| Leak census prints a type name by length, not to the next NUL | `890ede6` |
| A method's by-value container param is destroyed by the callee | `a5379f4` |
| Divergently-moved value dropped on the paths that did not move it | `a5379f4` |
| Coroutine state fields keyed by (name, type) | `36d7178` |
| Every `when` arm runs its scope cleanup | `a907721` |
| A variant field owns the string stored into it | `a907721` |

Result so far: `examples/lox/test.roxy` went **293 → 33** leaked objects (31
strings + 2 lists), and every simple `.lox` script is clean.

`ExpectedLeak` currently has **no users** — the suite asserts the teardown leak
invariant on every program it runs, with no opt-outs. Keep it that way; if you
must add one, name the `TODO.md` entry it pins.

---

## The two bugs

Both are in `TODO.md` too. Repros are verified against `03ed945`.

### 1. Over-release — a moved-out struct temporary is destroyed anyway

The temporary a struct literal builds is copied into the heap exception object
and marked moved (`nullify`), but its cleanup record still fires during unwind
and releases the string field the heap object now owns. The count reaches zero
while the handler is still using it.

```roxy
struct E { v: string; }
fun E.message(): string for Exception { return "e"; }
fun f(a: string) { var s: string = a + "!"; throw E { v = s }; }
fun main(): i32 {
    var x: string = "dyn";
    try { f(x); } catch (e: E) { print(e.v); }   // prints an EMPTY line
    return 0;
}
```

Expected: `dyn!`. The IR for `f` shows the retain (`str_retain`) and no
balancing release; the two releases both happen during unwind.

### 2. Under-release — a value-struct local is not destroyed at all

Same shape, but the local is a value struct owning a counted member. Nothing
releases it while unwinding.

```roxy
enum K { Num, Str }
struct V { when kind: K { case Num: n: i32; case Str: s: string; } }
fun new V.of_str(x: string) { self.kind = K::Str; self.s = x; }
struct E { v: V; }
fun E.message(): string for Exception { return "e"; }
fun f(a: string) { var val: V = V.of_str(a + "!"); throw E { v = val }; }
fun main(): i32 {
    var x: string = "dyn";
    try { f(x); } catch (e: E) { print(e.v.s); }
    return 0;
}
```

Expected: `dyn!` and no leak. Actual: correct output, `Leak: 1 ... 1 string`.

### Where to start reading

`IRBuilder::record_scope_cleanup_records` (`ir_builder_lifetime.cpp`) records
**every** owned local in the scope — it never consults `info.is_moved` — and the
`Nullify` annotation is what is supposed to narrow a record's live range for a
value that was moved. Bug 1 looks like that narrowing not covering a moved
*struct temporary*; bug 2 looks like a value-struct local not producing a firing
record at all. Verify that reading before acting on it — it is inference from
the IR, not something confirmed in the lowering.

---

## Traps

Each is a real measurement from the session that found these, not a worry.

**1. Do not guess program shapes.** Roughly ten hand-written repros of the
"obvious" shape all came back clean while the real one differed in a detail
nobody would guess (which `when` arm ran). The method below found the cause in
minutes each time. Use it first, not after.

**2. `--check-leaks` cannot see the over-release bug.** The object is *freed* —
early — so the census reports nothing. Its only detector is wrong output (an
empty line) or a later free-trap. Run both repros; they fail in different ways.

**3. Natives bypass the `vm/string.cpp` shims.** `native_str_concat` calls
`string_concat(vm, …)`, but f-strings, `str_substr` and the interpreter's own
string creation go straight to `roxy_rt.cpp`. Hooking the shim tags almost
nothing and looks like "no allocations here". Hook `roxy_rt.cpp`.

**4. A fix here can double-free, and only the suite will tell you.** The `when`
fix in `a907721` was correct for every case body and still double-freed on the
*trapping* else of an exhaustive `when`, which restores state but runs no body.
Expect the analogous asymmetry (an unwind edge is not an ordinary path) and run
the full suite on **both** backends before believing a fix.

**5. Only *dynamic* strings leak.** Literals are interned and immortal, so
retain/release are no-ops on them. That is why small hand-written tests look
clean and Lox does not — the leaks come from `str_concat` / `str_substr` /
f-string / `to_string` results.

**6. This is not the `reconcile_divergent_moves` machinery.** That one settles
*branch merges* (`if`, `if/else if`, `when`) and landed today. The unwind path is
a different mechanism — `IRCleanupInfo` records consumed by lowering. Don't try
to reuse one for the other.

---

## The method (use this before anything else)

Two temporary hooks in the runtime, then pair retains with releases for one
object. Roughly 40 lines; delete before committing.

**Hook points** — all in `src/roxy/rt/roxy_rt.cpp`:

- Define the hook globals **above `roxy_string_retain`** (it uses them):
  `g_str_sites` (a `std::map<void*, std::string>`), `g_str_tag_hook`,
  `g_str_rc_hook`.
- Tag allocations in `roxy_string_from_literal`, `roxy_string_alloc`, and
  `roxy_string_new_owned`.
- Log transitions in `roxy_string_retain` / `roxy_string_release`, printing the
  pointer, the new count, the string contents (`roxy_string_len` /
  `roxy_string_chars` — note it is `_len`, not `_length`), and the stack.

**Driving them** — in `src/roxy/vm/vm.cpp`:

- Install both hooks in `vm_load_module`, gated on `getenv("ROXY_LEAK_SITES")` /
  `getenv("ROXY_RC_TRACE")`, capturing the `RoxyVM*` in a file-static.
- The stack is `vm->call_stack[i].func->name` walked from `call_stack_size - 1`;
  three frames is enough to read, five to disambiguate.
- Dump the survivors in `vm_destroy`, **before** `vm->global_slots.reset()` and
  the slab shutdown — that is the only moment the census is meaningful.
- Enumerating survivors needs a temporary
  `SlabAllocator::live_objects_of_type(u32)` — copy the slot walk out of
  `live_object_stats()` and filter on `header->type_id`. Object *data* is
  `slot_ptr + sizeof(ObjectHeader)`.
- Skip pointers absent from `g_str_sites`; those are immortal literals and the
  CLI's own argv strings, which allocate before any frame exists (their stack
  prints empty — that empty stack is a useful marker, not a bug).

**Reading the result.** Group the leak sites first to pick a target:

```bash
ROXY_LEAK_SITES=1 ./build/roxy --check-leaks examples/lox/test.roxy 2>&1 \
  | grep "LEAKED STRING" | sed 's/ site: /|/' | awk -F'|' '{print $2}' \
  | sort | uniq -c | sort -rn
```

Then trace one object and **pair every retain with its release**, assigning each
pair to an owner (a local, a field, a container element). The retain with no
matching release names the owner that fails to clean up. The decisive step in
the last session was tracing a leaked string against a *non*-leaked one from the
same program — the first place their traces diverge is the bug.

```bash
ROXY_RC_TRACE=1 ./build/roxy --check-leaks examples/lox/main.roxy prog.lox 2>trace.txt
grep '"the-string"' trace.txt
```

---

## How to check yourself

```bash
ninja -C build                                   # -O0; asserts ON

# Fast inner loop (sandbox-safe, no system compiler)
./build/roxy_tests --test-case-exclude="*<C>*" --test-suite-exclude="E2E C Backend"

# Both backends — needs the system C++ compiler
./build/roxy_tests --test-case="*<C>*"
./build/roxy_tests --test-suite="E2E C Backend"

# Exception paths specifically
./build/roxy_tests --test-suite="E2E Exceptions"
./build/roxy_tests --test-suite="E2E Lifetimes"

# Leak census on any program (exit 70 if anything leaked)
./build/roxy --check-leaks prog.roxy
```

| Direction | Detector |
|---|---|
| under-release → leak | `--check-leaks`; the E2E harness asserts it on every program |
| over-release → premature free | wrong output (silent), `ref_dec: reference count already zero`, or the VM's double-delete assert |

**Integration check.** Track the numbers, not just pass/fail:

```bash
./build/roxy --check-leaks examples/lox/test.roxy    # 33 objects: 31 string, 2 list
./build/roxy --check-leaks examples/lox/main.roxy script.lox
```

Remaining alloc sites in `test.roxy`, for cross-checking which ones a fix
actually removes: `call_fun` ×12, `eval_binary` ×4, `call_native` ×3, `eval_get`
×2, plus ~9 scanner strings reached only through the `expect_*_error` tests. The
**2 lists** are a separate residual, specific to `test.roxy` and no longer
scaling with interpreter calls — do not expect a fix here to remove them, and do
not let them mask progress.

---

## Suggested order

1. **Reproduce both**, and confirm the `record_scope_cleanup_records` reading
   above by looking at the emitted cleanup records and how lowering consumes
   them — including what `Nullify` actually narrows. Both bugs may be one
   mechanism seen from two sides; find out before writing a fix for either.
2. **Fix the over-release first.** It is a use-after-free, it is silent, and it
   is the one the census cannot see. A leak is the safer failure to be left
   holding while you work.
3. **Then the under-release**, and re-measure Lox. If `test.roxy` does not drop
   well below 31 strings, the remaining sites are a different cause — go back to
   the method rather than extrapolating.

---

## Related open items (not this work)

`TODO.md` Medium Priority carries two lifetime findings that are independent:

- a discarded borrow is held to the end of its enclosing scope rather than its
  statement, so `delete` in the same scope trips the free-trap;
- rebinding a `ref` binding to a fresh owner compiles and then traps at runtime.
