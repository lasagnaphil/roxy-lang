# Handoff: the last Lox leak (one list)

Working notes for the last open High Priority leak. **Delete this file when the
work lands** — it is session scaffolding, not a reference.

**The task summary lives in `TODO.md`** under High Priority, and the model it has
to obey is [`docs/internals/lifetimes.md`](docs/internals/lifetimes.md) →
"Counting mechanics" and "Applying the model". Read those first. This file is the
*operational* half: what is already done, what will bite you, and how to check
yourself.

---

## The one-paragraph summary

`examples/lox/test.roxy` is down to **1 live object** — a list — from 293 on
2026-08-02 and 33 at the start of 2026-08-08. Every string leak is gone. The one
remaining object is an owned by-value `List` parameter that the callee has to
destroy, in a function that throws out of an `if` and returns after it: the
normal-path `Delete` + `Nullify` land in the `endif` block, which is laid out
*before* the throwing block, and a cleanup record is a single linear PC interval,
so the record is truncated below the throw. This is the same family as the five
unwind-path bugs already fixed but a **different mechanism** — the record
representation itself, not which block a record ends at — and the obvious fix
does not work (see below).

---

## State

Everything below landed and is *not* part of this task — listed so a regression
here is not mistaken for one of these.

| Change | Commit |
|---|---|
| Coroutine suspended in `catch` frees the caught exception | `de097ed` |
| Leak census prints a type name by length, not to the next NUL | `890ede6` |
| A method's by-value container param is destroyed by the callee | `a5379f4` |
| Divergently-moved value dropped on the paths that did not move it | `a5379f4` |
| Coroutine state fields keyed by (name, type) | `36d7178` |
| Every `when` arm runs its scope cleanup | `a907721` |
| A variant field owns the string stored into it | `a907721` |
| A string adopted by its binding is released once, not twice, on unwind | `824759f` |
| A cleanup record's end block is where emission was, not the last block created | `824759f` |
| A local reassigned in a branch is re-anchored onto the merge param | `cfa9d31` |
| A record whose end block was dropped as unreachable spans to end-of-function | `9213e5b` |

`ExpectedLeak` still has **no users** — the suite asserts the teardown leak
invariant on every program it runs, with no opt-outs. Keep it that way; if you
must add one, name the `TODO.md` entry it pins.

---

## The remaining bug

```roxy
struct E { msg: string; }
fun new E(m: string) { self.msg = m; }
fun E.message(): string for Exception { return self.msg; }

fun take(args: List<i32>): i32 {
    if (args.len() != 0) { throw E("bad"); }
    return 0;
}

fun main(): i32 {
    var l: List<i32> = List<i32>(); l.push(1);
    try { var r: i32 = take(l); } catch (e: E) { print(e.msg); }
    return 0;
}
```

`args` is an owned by-value container parameter, so the callee destroys it.
`--dump-bc` shows the whole story:

```
  code:
    0000..0006   entry: List$$len, branch
    0007..0010   endif:  <delete R1>, RET        <- normal-path cleanup + Nullify
    0011..0020   then:   STACK_ADDR R0, stack[0]
                         LOAD_CONST R1, 0        <- R1 RECYCLED for "bad"
                         ... THROW at 0019
  cleanup records:
    [1] Delete R1 scope [0, 10) live_start 0     <- throw at 19 is outside
```

Two things are wrong at once, and the second is what makes this hard:

1. The record is a single PC interval, and the `Nullify` narrowing truncates it
   at pc 10. The throwing block is laid out *after* the normal-exit block, so it
   falls outside. Widening the interval would fix only this half.
2. **The register is already recycled.** `LOAD_CONST R1, 0` puts the string
   constant in R1 inside the throwing block, because SSA liveness considers
   `args` dead there — it has no *use* in that block. So a record covering that
   PC range would `Delete` a string as a list.

So splitting the record into the PC runs the `Nullify` does not dominate — the
tempting fix — is not just insufficient, it is actively unsafe. A real fix has
to extend the value's register liveness across its whole cleanup-record range,
which is a **register-allocator** change, not a lowering one. Check whether
anything already pins cleanup-record values (`lowering.hpp` around
`m_value_ready_pcs` is the closest existing machinery) before designing it.

Variants that are **clean**, which pin the trigger precisely:

| Variant | Result |
|---|---|
| `if (…) { throw } else { return }` — both paths terminate, no merge block | clean |
| unconditional `throw` | clean |
| free function instead of a method | still leaks (not method-specific) |

In Lox this is `Interpreter.call_native`, which throws a Lox arity error out of
exactly this shape. `test_run_clock`'s `clock(1);` case is the only thing in
`test.roxy` still leaking; bisecting the 59 test-group calls in `main` down to
one group and then to one assertion took about two minutes and is worth
repeating rather than guessing.

---

## Traps

Each is a real measurement, not a worry.

**1. Do not guess program shapes.** Roughly ten hand-written repros of the
"obvious" shape came back clean, each time, across two sessions. The method below
found each real cause in minutes. Use it first, not after.

**2. Not every one of these is a leak.** One of the fixed bugs was an
*over*-release: the object is freed early, so `--check-leaks` reports nothing and
the only symptom is wrong output (an empty line) or a later free-trap. Check
output as well as the census.

**3. Exit code 70 is ambiguous when running Lox.** `roxy --check-leaks` exits 70
on a leak, and `examples/lox/main.roxy` itself returns 70 for a Lox runtime error
(`main.roxy:59`). The printed `Leak:` line is the discriminator, not the status.

**4. Natives bypass the `vm/string.cpp` shims.** They are thin wrappers over
`roxy_rt`, and f-strings / `str_substr` / the interpreter's own string creation
go straight to `roxy_rt.cpp`. Hook `roxy_rt.cpp` — `roxy_string_alloc_impl` is
the single allocation site for both literals and dynamic strings.

**5. A fix here can double-free, and only the suite will tell you.** Run the full
suite on **both** backends before believing one. Two concrete instances: the
`when` fix in `a907721` was correct for every case body and still double-freed on
the *trapping* else of an exhaustive `when`; and the merge fix in `cfa9d31` was
correct on the VM and double-freed on the C backend, which has no PC ranges and
runs every cleanup record once from a single `__unwind` label.

**6. Only *dynamic* strings leak.** Literals are interned and immortal, so
retain/release are no-ops on them. (Moot for the list, but it is why small
hand-written string tests look clean and Lox did not.)

**7. Ordinary scope exit is not the unwind path.** `emit_implicit_destroy` looks
a local up *by name*, so it always sees the current value; a cleanup record names
a *register* fixed at declaration. Every bug in this family so far has been that
asymmetry. Removing the `throw` from a repro making it clean is the signature.

---

## The method

Two temporary hooks in the runtime, then pair retains with releases for one
object. Roughly 40 lines; delete before committing. Three of the fixed bugs were
found this way, in minutes each. It is written for strings; the remaining leak is
a *list*, so tag `roxy_list_*` allocations instead — but the bisect above already
localized it, so you may not need it at all.

**Hook points** — all in `src/roxy/rt/roxy_rt.cpp`:

- Define the hook globals **above `roxy_string_retain`** (it uses them):
  `g_str_sites` (a `std::map<void*, std::string>`), `g_str_tag_hook`,
  `g_str_rc_hook`. Plain globals, no namespace — `vm.cpp` declares them
  `extern "C"` and the symbols match.
- Tag allocations in `roxy_string_alloc_impl`, gated on `!immortal`. It is the
  single site behind both `roxy_string_from_literal` and
  `roxy_string_new_owned`.
- Log transitions in `roxy_string_retain` / `roxy_string_release`, printing the
  pointer, the new count, the string contents (`roxy_string_len` /
  `roxy_string_chars` — note it is `_len`, not `_length`), and the stack.

**Driving them** — in `src/roxy/vm/vm.cpp`:

- Install both hooks in `vm_load_module`, gated on `getenv("ROXY_LEAK_SITES")` /
  `getenv("ROXY_RC_TRACE")`, capturing the `RoxyVM*` in a file-static.
- The stack is `vm->call_stack[i].func->name` walked from `call_stack_size - 1`;
  three frames is enough to read, five to disambiguate.
- **Print the PC too** — `frame.pc - frame.func->code.data()`. It turns "some
  retain in `call_fun`" into a line you can look up in `--dump-bc`, and it is
  what pinned two of the fixes. The catch: the interpreter keeps `pc` in a local
  and only syncs `frame->pc` at calls, so add `if (g_str_rc_hook) frame->pc = pc;`
  to the `STR_RETAIN` / `STR_RELEASE` handlers in `interpreter.cpp` or the top
  frame's PC is stale.
- Dump the survivors in `vm_destroy`, **before** `vm->global_slots.reset()` and
  the slab shutdown — that is the only moment the census is meaningful.
- Enumerating survivors needs a temporary
  `SlabAllocator::live_objects_of_type(u32)`: copy the slot walk out of
  `live_object_stats()` and filter on `header->type_id`. **Return
  `slot_ptr + sizeof(roxy_object_header)`** — the slot starts at the header, but
  every hook keys on the object *data* pointer `roxy_alloc` handed out. Getting
  this wrong yields an empty report with no error.
- Skip pointers absent from `g_str_sites`; those are immortal literals and the
  CLI's own argv strings, which allocate before any frame exists.

**Reading the result.** Group the leak sites first to pick a target:

```bash
ROXY_LEAK_SITES=1 ./build/roxy --check-leaks examples/lox/test.roxy 2>&1 \
  | grep "LEAKED STRING" | sed 's/ site: /|/' | awk -F'|' '{print $2}' \
  | sort | uniq -c | sort -rn
```

Then trace one object and **pair every retain with its release**, assigning each
pair to an owner (a local, a field, a container element). The retain with no
matching release names the owner that fails to clean up.

Once you have a function and a PC, `./build/roxy --dump-bc prog.roxy` prints the
**exception handler table and the cleanup records** (added `824759f`) alongside
the code, so you can read off which records cover the throwing PC and which do
not. That listing is what made every one of these obvious, including the register
recycling above.

Finally, shrink a Lox repro to a Roxy one by *bisecting the real code*, not by
inventing a shape: take the Lox function the trace names and remove one construct
at a time. That is what produced "the `if` is what matters, the `when` arm is
not", which no amount of guessing had.

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

# Leak census on any program (exit 70 if anything leaked — but see trap 3)
./build/roxy --check-leaks prog.roxy
```

| Direction | Detector |
|---|---|
| under-release → leak | `--check-leaks`; the E2E harness asserts it on every program |
| over-release → premature free | wrong output (silent), `ref_dec: reference count already zero`, or the VM's double-delete assert |

**Integration check.** Track the number, not just pass/fail:

```bash
./build/roxy --check-leaks examples/lox/test.roxy    # 1 object: 1 list
```

---

## Related open items (not this work)

`TODO.md` Medium Priority carries two lifetime findings that are independent:

- a discarded borrow is held to the end of its enclosing scope rather than its
  statement, so `delete` in the same scope trips the free-trap;
- rebinding a `ref` binding to a fresh owner compiles and then traps at runtime.

Two C-backend gaps were found and recorded (not fixed) while doing this work —
both pre-existing, both in `docs/internals/c-backend.md` → "Known C-backend
gaps": a cleanup record naming a by-value struct emits pointer-shaped null guards
that do not compile (this one blocks three of the new regression tests from
running on C), and a tagged union with a pointer-sized variant field is
struct-copied with the 4-byte slot model's size, which halves the pointer.
