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

`noncopyable()` on a struct used to mean literally *"has a default destructor"*,
so **Drop and Move-only were the same bit**. That is wrong in both directions: a
`string` field earns no drop and leaks, while a `ref` field forces the whole
struct move-only. The `ref` half is now fixed. The `string` half needs one more
piece: `Map` must acquire a count for the keys and values it holds, because
opening the gate is what makes map teardown release them.

---

## State

| Step | Status | Commit |
|---|---|---|
| Drop gate derives from `compute_drop_plan` | ✅ | `3e589fc` |
| `compute_retain_plan` (the retain derivation) | ✅ | `039d346` |
| Cleanup tracking keyed on drop glue, not move-only-ness | ✅ | `0dbcdf9` |
| Clone glue at every struct duplication site | ✅ | `40460fd` |
| (independent) else-if chain condition scoping | ✅ | `9d1cd1b` |
| Structural move-only (`is_move_only`) | ✅ | see below |
| **`Map` key/value acquisition** | ❌ | — |
| **Flip the `StrRelease` gate** | ❌ | — |

Everything through structural move-only is verified, not assumed — see
"How to check yourself".

**The `ref`-bearing-struct over-restriction is fixed.** `struct Holder { r: ref
Point; }` is now copyable, and each copy counts its own borrow.

**The `string`-field leak is still there.** `leak_box` below still reports one
leaked string.

---

## What is left

Only the container side. The gate flip makes containers release `string` keys and
counted values on teardown, and `Map` acquires nothing.

`List` is already done — `push` acquires a count for any counted element through
`emit_value_retain`, generalized from the `string`-only case.

`Map` needs, at the `insert` call site in `gen_call_member`:

- a **value** retain (unconditional — the map's slot owns the value either way);
- a **key** retain **only when the key is new**, because `roxy_map_insert`
  replaces in place and keeps the existing key;
- the **old value released** on the replace path, for copyable-with-drop values
  (`emit_map_value_delete_if_present` does this today but skips copyable ones);
- `remove` releasing the key to match, and `clear` likewise.

The `contains`-guarded branch machinery for exactly this already exists in
`emit_map_value_delete_if_present`. One `contains` call can drive both the
old-value destroy and the new-key retain.

Then flip the one line in `member_needs_drop` (`types.hpp`) that excludes
`DropKind::StrRelease`, and work through whatever the repros below surface.

`m[k] = v` (`gen_assign_index`) needs the same treatment as `insert`; it has
explicit `ref`/`string` element handling already, so follow that shape.

---

## Traps

Things that cost time in previous sessions. Each is a real measurement, not a
worry.

**1. The obvious shortcut is ruled out — don't re-derive it.**
Enabling `StrRelease` alone fixes the leak in one line and is trivially safe (a
move-only value is never duplicated). But before structural move-only landed it
made `examples/lox` **stop compiling**: `Token` holds a `lexeme: string`, and the
parser reads tokens out of a `List<Token>` by value. That specific breakage is
now gone, but the shortcut's *premise* — accept move-only for string-bearing
structs — remains unacceptable, and the whole point of the clone glue is that it
is no longer needed.

**2. Retain-without-release is invisible to the leak checker.**
The teardown census counts *objects*, not counts. Adding retains with no
matching release leaves the same object alive, so `--check-leaks` reports the
same number and looks fine. Do not use it as evidence that the glue is balanced.
The detector for that direction is the *under*-retain side (below).

**3. `noncopyable()` asserts the flag has been derived — trust that assert.**
It is what found both ordering gaps in the structural-move-only change
(`resolve_global_var` asking before the derivation ran; native structs never
deriving at all). If you add a new way to create a struct type, call
`derive_struct_move_only` once its fields are final, or the assert will tell you.

**4. `.copy()` is not Retain.** `.copy()` is an explicit deep duplication for
move-only containers. Retain is implicit and shallow. They are different
operations; don't let the names blur.

**5. Only *dynamic* strings leak.** Literals are interned and immortal
(`ROXY_STR_IMMORTAL`), so retain/release are no-ops on them and a struct holding
only literal strings is already clean. The leak comes from `str_substr` /
`str_concat` / f-string / `to_string` results. That is why small hand-written
tests look fine and Lox does not — and why the repros below all build their
strings dynamically.

**6. A `Map<string, _>` with a dynamic key is already broken, gate or no gate.**
`m.insert(f"k{i}", v)` inside a loop stores a key the map does not retain; the
key is freed at the iteration's scope exit and every later lookup misses
(`Unhandled exception` from the `KeyError` throw). Reproduce with `mp2` below on
any commit. Don't mistake it for something the flip introduced — but do note the
flip turns it from "missing key" into "double free", so it has to be fixed either
way.

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
| under-retain → premature free | slab `states[slot] == ALIVE` assert (true double free), `release: reference count already zero` |
| over-retain / missing drop → leak | `--check-leaks`, and the E2E harness asserts it on **every** program it runs |

Two tests opt out of the leak assertion via `ExpectedLeak` (both in
`E2E Coroutines`, pinning a separate known bug). If you add one, name the
`TODO.md` entry it pins.

**The equivalence trick.** Every step claimed to be a no-op was proven so by
temporarily asserting the old and new predicates agree, then running the whole
suite:

```cpp
assert(member_needs_drop(t) == t->noncopyable() && "TEMP equivalence probe");
```

It has never fired across 2460 cases on both backends plus every example. Reuse
this pattern for any step claimed to be a no-op — green tests alone do not prove
it. For the clone glue, the probe was `assert(false)` inside `emit_member_retain`
(the glue emits nothing while the gate is closed).

**When something breaks, name it.** Two diagnostics paid for themselves and are
worth re-adding temporarily rather than reasoning from first principles:

- `lookup_local`'s error with the variable name and `m_current_func->name`
  interpolated — it turned "undefined variable in IR generation" into a
  three-minute diagnosis;
- in `SlabAllocator::free_in_slab`, printing the header before the assert, plus
  preserving `type_id` across the free's `memset` so the *second* free can still
  say what it was freeing.

---

## Reproductions

Keep these; they cover each direction.

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

**The over-restriction** — *fixed*; now prints `1 1` and exits clean:

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

**`mp2` — the map key that dangles** (broken today, independent of the gate):

```roxy
fun main(): i32 {
    var m: Map<string, i32> = Map<string, i32>();
    var i: i32 = 0;
    while (i < 3) { m.insert(f"k{i}", i); i = i + 1; }
    print(f"{m["k1"]}");             // Runtime error: Unhandled exception
    return 0;
}
```

**`mp3` — the map value double free** (with the gate open):

```roxy
struct S { a: string; n: i32; }
fun mk(i: i32): S { return S { a = f"a{i}", n = i }; }
fun main(): i32 {
    var m: Map<i32, S> = Map<i32, S>();
    var i: i32 = 0;
    while (i < 3) { m.insert(i, mk(i)); i = i + 1; }
    var v: S = m[1];
    print(v.a);
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

Lox compiling *and running* at all is the guard against regressions. Note that a
bare `print 1;` exercises far less than a script with a function call — use one
with a call.

---

## Related open items (not this work)

`TODO.md` High Priority also carries two findings independent of this plan:

- a coroutine suspended inside a `catch` leaks the caught exception when
  destroyed undrained (pinned by the two `ExpectedLeak` uses);
- the Lox per-call `List` leak mentioned above.
