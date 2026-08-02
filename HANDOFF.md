# Handoff: separating Drop from Copy

Working notes for finishing the value-lifecycle separation. **Delete this file
when the work lands** — it is session scaffolding, not a reference.

**The design is not here.** It lives in
[`docs/internals/lifetimes.md`](docs/internals/lifetimes.md) → "The value
lifecycle" and "PLANNED: separating Drop from Copy", with the task summary in
`TODO.md` under High Priority. Read those first. This file is the *operational*
half: what is already done, what will bite you, and how to check yourself.

Keeping the design in one place is deliberate — a `string` field silently
leaking went unnoticed for months because `strings.md` and `lifetimes.md` each
held half the story and disagreed.

---

## The one-paragraph summary

`noncopyable()` on a struct means literally *"has a default destructor"*, so
**Drop and Move-only are the same bit**. That is wrong in both directions: a
`string` field earns no drop and leaks, while a `ref` field forces the whole
struct move-only. Fixing it means giving a copyable type non-trivial drop *and*
retain glue, which is four changes, of which two have landed.

---

## State

| Step | Status | Commit |
|---|---|---|
| Drop gate derives from `compute_drop_plan` | ✅ | `3e589fc` |
| `compute_retain_plan` (the retain derivation) | ✅ | `039d346` |
| Cleanup tracking keyed on drop glue, not move-only-ness | ✅ | `0dbcdf9` |
| **Clone glue at every duplication site** | ❌ | — |
| **Structural move-only + flip the `StrRelease` gate** | ❌ | — |

The two landed code steps are **behaviour-neutral by construction** and were
verified as such, not assumed. Nothing user-visible has changed yet; the leak is
still there.

Everything else in this thread (container borrows, the return-path fixes, the
teardown leak check) is unrelated to this work and already landed.

## What is left, and why it must land as one change

Steps 4 and 5 **cannot be split**. Each alone is unbalanced:

| Landed alone | Result |
|---|---|
| clone glue | retains with no matching release — imbalance grows |
| flip the gate | string-bearing structs become move-only → Lox stops compiling |
| structural move-only | `ref`-bearing structs become copyable with nothing balancing their counts → **use-after-free** |

`lifetimes.md` → "The ordering constraint" has the full table.

---

## Traps

Things that cost time in the previous session. Each is a real measurement, not a
worry.

**1. The obvious shortcut is ruled out — don't re-derive it.**
Enabling `StrRelease` alone fixes the leak in one line and is trivially safe (a
move-only value is never duplicated). The whole suite passes except four
`Structured Gen` seeds. But `examples/lox` **stops compiling**: `Token` holds a
`lexeme: string`, and the parser reads tokens out of a `List<Token>` by value in
four places, which becomes *"cannot move a noncopyable value out of a container
element"*. A `string` field is far too common for move-only.

**2. Retain-without-release is invisible to the leak checker.**
The teardown census counts *objects*, not counts. Adding retains with no
matching release leaves the same object alive, so `--check-leaks` reports the
same number and looks fine. Do not use it as evidence that the glue is balanced.
The detector for that direction is the *under*-retain side (below).

**3. Two duplication classes have no IR op to attach glue to.**
A copyable value struct is duplicated at six places. Only the first goes through
`IROp::StructCopy`:

1. the 10 `emit_struct_copy` calls in the IR builder;
2. **a by-value struct argument** — the caller packs the struct's slots into
   argument registers during *bytecode lowering*;
3. **a small (≤ 4 slot) struct return** — returned in registers, likewise;
4. reading a struct element out of a container (`list[i]` on `List<Box>`);
5. reading a struct-typed field out of another struct;
6. a closure capture by copy.

Missing one turns the leak into a use-after-free the moment the gate flips. This
is why the recommendation is to route duplication through an op that *carries*
the obligation (a `clone` flag on `StructCopy`, or a distinct `StructClone`) and
have `IRValidator` check completeness — rather than patching sites and hoping the
list is complete. Sites 2 and 3 probably have to move into the IR to be covered
at all.

**4. `noncopyable()` has 85 call sites.** Changing what it means for structs is
the widest-blast-radius edit in the plan. The structural replacement should be a
precomputed `StructTypeInfo::is_move_only` flag filled by the existing
synthetic-destructor fixpoint, not a recursive predicate — a recursive one has to
handle value cycles, which are rejected elsewhere but not here.

**5. `.copy()` is not Retain.** `.copy()` is an explicit deep duplication for
move-only containers. Retain is implicit and shallow. They are different
operations; don't let the names blur.

**6. Only *dynamic* strings leak.** Literals are interned and immortal
(`ROXY_STR_IMMORTAL`), so retain/release are no-ops on them and a struct holding
only literal strings is already clean. The leak comes from `str_substr` /
`str_concat` / f-string / `to_string` results. That is why small hand-written
tests look fine and Lox does not.

---

## How to check yourself

```bash
ninja -C build                                  # -O0; asserts ON

# Fast inner loop (sandbox-safe, no system compiler)
./build/roxy_tests --test-case-exclude="*<C>*" --test-suite-exclude="E2E C Backend"

# Full, both backends — needs the system C++ compiler
./build/roxy_tests

# The lifecycle derivations specifically
./build/roxy_tests --test-suite="Lifecycle Predicates"

# Leak census on any program
./build/roxy --check-leaks prog.roxy            # exit 70 if anything leaked
```

**Both error directions have detectors — use both.**

| Direction | Detector |
|---|---|
| under-retain → premature free | VM double-delete assert, `release: reference count already zero` |
| over-retain / missing drop → leak | `--check-leaks`, and the E2E harness asserts it on **every** program it runs |

Two tests currently opt out of the leak assertion via `ExpectedLeak` (both in
`E2E Coroutines`, pinning a separate known bug). If you add one, name the
`TODO.md` entry it pins.

**The equivalence trick.** Both landed steps were proven behaviour-neutral by
temporarily asserting the old and new predicates agree, then running the whole
suite:

```cpp
assert(member_needs_drop(t) == t->noncopyable() && "TEMP equivalence probe");
```

It never fired across 2460 cases on both backends plus every example. Reuse this
pattern for any step claimed to be a no-op — green tests alone do not prove it.

---

## Reproductions

Save these; the previous session's scratch copies are gone.

**The leak** — expect `Leak: 1 object(s) ... 1 string`:

```roxy
struct Box { s: string; }
fun main(): i32 {
    var a: string = "a";
    var dyn: string = a + "x";       // dynamic, not interned
    var b: Box = Box { s = dyn };
    print(b.s);
    return 0;
}
```

**The over-restriction** — expect *"use of moved value 'h'"*, which should
compile once the work lands:

```roxy
struct Point { x: i32 = 1; }
struct Holder { r: ref Point; }
fun main(): i32 {
    var p: uniq Point = uniq Point();
    var h: Holder = Holder { r = p };
    var h2: Holder = h;              // a copy of a borrow is just another borrow
    print(f"{h.r.x} {h2.r.x}");
    return 0;
}
```

**The integration check** — `examples/lox`. Currently reports 38 strings + 177
lists for a small script (`fun f(n) {...} print f(10);`). The 38 strings are this
bug; **the 177 lists are a separate, unresolved leak** (see `TODO.md`) — do not
expect them to disappear, and do not let them mask progress. Track the string
count.

```bash
./build/roxy --check-leaks examples/lox/main.roxy script.lox
```

Lox compiling at all is the guard against the move-only regression.

---

## Suggested order

1. **Clone-carrying IR op.** Add the flag/op, route the 10 `emit_struct_copy`
   sites, add `IRValidator` coverage. Emit nothing yet — or emit and accept the
   temporary imbalance, since it is invisible either way (trap 2). Then handle
   duplication classes 2–6, which is the hard part and may mean moving argument
   and return duplication into the IR.
2. **Structural move-only + gate flip, together.** One commit. Expect the four
   `Structured Gen` seeds to need looking at, and expect Lox to be the real test.

Step 1 is where essentially all the risk is. It is worth writing the
`IRValidator` check *before* the glue, so completeness is enforced rather than
reviewed.

---

## Related open items (not this work)

`TODO.md` High Priority also carries two findings from the same session that are
independent of this plan:

- a coroutine suspended inside a `catch` leaks the caught exception when
  destroyed undrained (pinned by the two `ExpectedLeak` uses);
- the Lox per-call `List` leak mentioned above.
