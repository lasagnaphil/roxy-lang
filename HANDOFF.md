# Handoff: the last Lox leak (a Lox runtime error)

Working notes for the last open High Priority leak. **Delete this file when the
work lands** — it is session scaffolding, not a reference.

**The task summary lives in `TODO.md`** under High Priority, and the model it has
to obey is [`docs/internals/lifetimes.md`](docs/internals/lifetimes.md) →
"Counting mechanics" and "Applying the model". Read those first. This file is the
*operational* half: what is already done, what will bite you, and how to check
yourself.

---

## The one-paragraph summary

`examples/lox/test.roxy` is down to **6 live objects** (5 strings + 1 list) from
293 on 2026-08-02 and 33 at the start of 2026-08-08. Every one of the 5 strings
is a Lox string *literal* in an expression whose evaluation raises a Lox
**runtime** error — one per case in `test_eval_runtime_errors`. Every other alloc
site in the program is clean. The three unwind-path bugs that used to dominate
this number are fixed (below); this is a fourth site in the same family, not yet
diagnosed.

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

`ExpectedLeak` still has **no users** — the suite asserts the teardown leak
invariant on every program it runs, with no opt-outs. Keep it that way; if you
must add one, name the `TODO.md` entry it pins.

---

## The remaining bug

Five leaked strings, all `rc=1`, all allocated in `Scanner$$scan_string` and
reached only through `TestState$$expect_eval_runtime_error`:

```
"x"   from   1 + "x"
"hi"  from   -"hi"
"a"   from   "a" - 1
"a"   from   "a" < "b"
"b"   from   "a" < "b"
```

The path is `Interpreter.interpret` (`examples/lox/interpreter.roxy:693`):

```roxy
pub fun Interpreter.interpret(e: ref Expr): LoxValue {
    try {
        return self.evaluate(e);
    } catch (err: RuntimeError) {
        self.had_runtime_error = true;
        self.error_msg = err.message();
    }
    return LoxValue.of_nil();
}
```

A `LoxValue` holding the string is in flight when `evaluate` throws, and its
count is never released. Note the shape differs from the three already fixed:
the throw happens inside the *argument of a `return`*, and the catch does not
read the thrown value at all.

Repro it the same way, which is cheap:

```bash
printf 'print 1 + "x";\n' > /tmp/e.lox
./build/roxy --check-leaks examples/lox/main.roxy /tmp/e.lox
```

The **1 list** is a separate residual, specific to `test.roxy`, not scaling with
interpreter calls, and never diagnosed. Do not expect a fix here to remove it,
and do not let it mask progress.

---

## Traps

Each is a real measurement, not a worry.

**1. Do not guess program shapes.** Roughly ten hand-written repros of the
"obvious" shape came back clean, each time, in two separate sessions. The method
below found each real cause in minutes. Use it first, not after.

**2. Not every one of these is a leak.** One of the three fixed bugs was an
*over*-release: the object is freed early, so `--check-leaks` reports nothing and
the only symptom is wrong output (an empty line) or a later free-trap. Check
output as well as the census.

**3. Natives bypass the `vm/string.cpp` shims.** They are thin wrappers over
`roxy_rt`, and f-strings / `str_substr` / the interpreter's own string creation
go straight to `roxy_rt.cpp`. Hook `roxy_rt.cpp` — `roxy_string_alloc_impl` is
the single allocation site for both literals and dynamic strings.

**4. A fix here can double-free, and only the suite will tell you.** Run the full
suite on **both** backends before believing one. Two concrete instances: the
`when` fix in `a907721` was correct for every case body and still double-freed on
the *trapping* else of an exhaustive `when`; and the merge fix in `cfa9d31` was
correct on the VM and double-freed on the C backend, which has no PC ranges and
runs every cleanup record once from a single `__unwind` label.

**5. Only *dynamic* strings leak.** Literals are interned and immortal, so
retain/release are no-ops on them. That is why small hand-written tests look
clean and Lox does not.

**6. Ordinary scope exit is not the unwind path.** `emit_implicit_destroy` looks
a local up *by name*, so it always sees the current value; a cleanup record names
a *register* fixed at declaration. Every bug in this family so far has been that
asymmetry. Removing the `throw` from a repro making it clean is the signature.

---

## The method (use this before anything else)

Two temporary hooks in the runtime, then pair retains with releases for one
object. Roughly 40 lines; delete before committing. `824759f` and `cfa9d31` were
both found this way, in minutes each.

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
  what pinned the merge bug. The catch: the interpreter keeps `pc` in a local and
  only syncs `frame->pc` at calls, so add `if (g_str_rc_hook) frame->pc = pc;` to
  the `STR_RETAIN` / `STR_RELEASE` handlers in `interpreter.cpp` or the top
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

```bash
ROXY_RC_TRACE=1 ./build/roxy --check-leaks examples/lox/main.roxy prog.lox 2>trace.txt
grep '"the-string"' trace.txt
```

Once you have a function and a PC, `./build/roxy --dump-bc prog.roxy` prints the
**exception handler table and the cleanup records** (added `824759f`) alongside
the code, so you can read off which records cover the throwing PC and which do
not. That listing is what made both fixed bugs obvious.

Finally, shrink the Lox repro to a Roxy one by *bisecting the real code*, not by
inventing a shape: take the Lox function the trace names and remove one construct
at a time. The merge bug came out as "the `if` is what matters, the `when` arm is
not", which no amount of guessing had produced.

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
./build/roxy --check-leaks examples/lox/test.roxy    # 6 objects: 5 string, 1 list
./build/roxy --check-leaks examples/lox/main.roxy script.lox
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
that do not compile, and a tagged union with a pointer-sized variant field is
struct-copied with the 4-byte slot model's size, which halves the pointer.
