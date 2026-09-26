# Strings

Strings in Roxy are heap-allocated, immutable objects. They support literals with escape sequences (grammar in `docs/grammar.md`), f-string interpolation (`f"hello {expr}"`), `+` concatenation, `==`/`!=` comparison, and printing. All operations are natives (`str_*` in `register_builtin_natives`, `src/roxy/vm/natives.cpp`); `+` and `==`/`!=` on string operands are rewritten to `str_concat` / `str_eq` / `str_ne` calls in `gen_binary_expr`. VM and AOT share one representation (`rt/roxy_rt.{h,cpp}`).

## Memory Layout

```
┌─────────────────┬─────────────────┬─────────────────────────┐
│  ObjectHeader   │  StringHeader   │  Character Data + '\0'  │
│    (16 bytes)   │    (8 bytes)    │     (length + 1)        │
└─────────────────┴─────────────────┴─────────────────────────┘
```

`roxy_string_header` is `{u32 length, u32 hash}`. Because strings are immutable, capacity is always `length + 1` and isn't stored — that slot instead caches the low 32 bits of `XXH3_64bits(chars, length)`, computed at allocation, which `Map<string, V>` reads directly to avoid re-hashing on every probe. Character data is always null-terminated for C interop.

## F-String Interpolation

Any expression is allowed inside `{}`, including struct literals — the lexer tracks brace nesting depth, so `f"point: {Point { x = 1, y = 2 }}"` parses. Pipeline: the lexer emits `FStringBegin` / `FStringMid` / `FStringEnd` tokens; the parser builds an `ExprStringInterp`; sema checks each interpolated expression is `Printable`; the IR builder converts each non-string part with `to_string` and left-folds with `str_concat`:

```
f"x = {x}, y = {y}"
  → str_concat(str_concat(str_concat("x = ", i32$$to_string(x)), ", y = "), i32$$to_string(y))
```

### Printable

`Printable` is a builtin trait requiring `to_string(): string`. Primitives have registered `$$to_string` natives; user structs implement it with `fun T.to_string(): string for Printable`; containers get a synthesized per-instantiation `to_string` ([list.md](list.md), [maps.md](maps.md)). All per-type dispatch is centralized in the IR builder's `emit_to_string_value`. `print` is an overload set whose per-primitive natives match the `to_string` formats, so `print(x)` and `print(f"{x}")` always agree ([overloading.md](overloading.md)).

A **`uniq` or `ref` prints as its pointee**: both are statically-live pointers with the same representation as the value they name, so the pointer is handed straight to the pointee's `to_string`. This is what lets a function taking `ref List<i32>` still use `f"{items}"`.

**`weak` is deliberately excluded**: it can dangle, so `to_string` would read freed memory. Printing one needs an explicit liveness check first.

## String Interning

Only **literals** are interned. `vm_load_module` interns each String constant once (`roxy_string_from_literal` probes `roxy_ctx.string_intern`) and caches the pointer in the constant, so `LOAD_CONST` is a pointer load and every load of a literal yields the same pointer. **Dynamically created strings** (concat, f-string, `substr`, `to_string`, `read_file`) are **not** interned — they are fresh, uniquely-owned objects (`roxy_string_new_owned`), so freeing one never has to evict an intern entry. The intern table is always present in VM mode, optional in AOT mode.

## Memory Management

Strings are **reference-counted**. The `ObjectHeader.ref_count` is repurposed as an **owner count** (strings are never `ref`-borrowed, so there is no clash with the borrow free-trap): a copy retains, a drop releases, the last release frees. Interned **literals are immortal** — sentinel `ref_count == ROXY_STR_IMMORTAL` (`0xFFFFFFFF`) — so retain/release no-op on them and the intern pool never dangles. Dynamic strings start at count 1.

`string` is the copyable-with-nontrivial-drop shape: `is_copy()`, yet it needs a retain on every store (`compute_retain_plan`) and a `StrRelease` on every drop (`compute_drop_plan`), lowered to `STR_RETAIN`/`STR_RELEASE` in the VM and `roxy_string_retain`/`roxy_string_release` in C. It is not move-only because its drop has an exact inverse — see [lifetimes.md → The value lifecycle](lifetimes.md#the-value-lifecycle). Standalone strings, container elements, and struct fields are all reclaimed.
