# Strings

Strings in Roxy are heap-allocated, immutable objects managed through the object system. They support literals with escape sequences, f-string interpolation (`f"hello {expr}"`), `+` concatenation, `==`/`!=` comparison, length query, and printing. All operations are native functions; the `+` and `==`/`!=` operators are rewritten to native calls during IR building. The runtime layout is unified — VM and AOT-compiled programs share the same string representation from `roxy_rt.h`.

## Memory Layout

A string is allocated through the object system as three contiguous regions:

```
┌─────────────────┬─────────────────┬─────────────────────────┐
│  ObjectHeader   │  StringHeader   │  Character Data + '\0'  │
│    (16 bytes)   │    (8 bytes)    │     (length + 1)        │
└─────────────────┴─────────────────┴─────────────────────────┘
```

`StringHeader` (the unified `roxy_string_header` from `roxy_rt.h`) is `{u32 length, u32 hash}`: `length` excludes the null terminator, and `hash` is the low 32 bits of `XXH3_64bits(chars, length)`, cached at allocation. Because strings are immutable, capacity is always `length + 1` and isn't stored — the 8-byte slot is reused for the cached hash, which `Map<string, V>` reads directly to avoid re-hashing on every probe. The character data immediately follows the header and is always null-terminated for C interoperability.

## String Literals

Literals are enclosed in double quotes (`var s: string = "hello world";`). The parser processes these escape sequences:

| Escape | Character |
|--------|-----------|
| `\n`   | Newline (0x0A) |
| `\t`   | Tab (0x09) |
| `\r`   | Carriage return (0x0D) |
| `\\`   | Backslash |
| `\"`   | Double quote |
| `\0`   | Null character |
| `\{`   | Literal `{` (in f-strings and regular strings) |
| `\}`   | Literal `}` (in f-strings and regular strings) |

## F-String Interpolation

F-strings interpolate expressions using `f"..."` syntax: text segments between `{}` are literal content, and expressions inside `{}` are converted to strings and concatenated. Use `\{` and `\}` for literal braces.

```roxy
var name: string = "World";
var count: i32 = 3;
print(f"Hello, {name}!");        // "Hello, World!"
print(f"{name} v{count}");       // "World v3"
print(f"sum = {count + 1}");     // "sum = 4"
```

Any expression is allowed inside `{}`, including function calls and struct literals — the lexer tracks brace nesting depth, so `f"point: {Point { x = 1, y = 2 }}"` parses correctly.

### Compilation pipeline

1. **Lexer** — produces `FStringBegin` / `FStringMid` / `FStringEnd` tokens with brace-depth tracking.
2. **Parser** — builds an `ExprStringInterp` AST node holding text parts and expression sub-trees.
3. **Semantic analysis** — validates each interpolated expression implements the `Printable` trait.
4. **IR builder** — inserts `to_string()` calls for non-string expressions, then chains `str_concat` calls in a left-fold. For example, `f"x = {x}, y = {y}"` generates:

```
str_concat(str_concat(str_concat("x = ", i32$$to_string(x)), ", y = "), i32$$to_string(y))
```

### Printable trait

`Printable` is a builtin trait registered during semantic analysis, requiring one method `to_string(): string`. Primitive types have native `to_string()` implementations registered automatically:

| Type | Example | Output |
|------|---------|--------|
| `bool` | `f"{true}"` | `"true"` |
| `i32` | `f"{42}"` | `"42"` |
| `i64` | `f"{1000000l}"` | `"1000000"` |
| `f32` | `f"{3.14f}"` | `"3.14"` |
| `f64` | `f"{3.14}"` | `"3.14"` |
| `string` | `f"{'hello'}"` | `"hello"` |

User structs implement `Printable` by providing a `to_string()` method via the standard trait mechanism, after which they can be interpolated:

```roxy
fun Point.to_string(): string for Printable {
    return f"({self.x}, {self.y})";
}

var p: Point = Point { x = 1, y = 2 };
print(f"point = {p}");  // "point = (1, 2)"
print(p);               // same — print's Printable fallback (overloading.md)
```

A **`uniq` or `ref` prints as its pointee**: both are statically-live heap
pointers with the same representation as the value they name, so
`type_implements_printable` looks through them and `emit_to_string_value`
hands the pointer straight to the pointee's `to_string` — no conversion. This
is what lets a borrowing function keep `f"{items}"` when it takes
`ref List<i32>` rather than `inout List<i32>` (whose parameter type is the bare
container).

**`weak` is deliberately excluded** and still errors: it can dangle, so
`to_string` would read freed memory. Printing one needs an explicit liveness
check first.

Per-type conversion is centralized in the IR builder's `emit_to_string_value`
(String pass-through, primitives/enums via their registered `$$to_string`
natives, structs via `Struct$$to_string`, containers via the synthesized
per-instantiation `to_string` — see `list.md`/`maps.md`). `print` itself is an
overload set with direct per-primitive natives whose formats match the
`to_string` natives, so `print(x)` and `print(f"{x}")` always agree.

## String Operations

| Operation | Surface syntax | Lowers to |
|---|---|---|
| Concatenation | `a + b` | `str_concat(a, b)` |
| Equality | `a == b` | `str_eq(a, b)` |
| Inequality | `a != b` | `str_ne(a, b)` |
| Length | — | `str_len(s)` |
| Printing | — | `print(s)` |

Operator rewriting happens in `gen_binary_expr()`, which detects string operands (`left_type->kind == TypeKind::String`) and substitutes the matching native call. Equality first compares pointers (fast path), then lengths, then uses `memcmp`.

### Native functions

| Function | Signature | Description |
|----------|-----------|-------------|
| `str_concat` | `(string, string) -> string` | Concatenate two strings |
| `str_eq` | `(string, string) -> bool` | Test equality |
| `str_ne` | `(string, string) -> bool` | Test inequality |
| `str_len` | `(string) -> i32` | Get string length |
| `print` | `(string) -> void` | Print string to stdout |
| `bool$$to_string` | `(bool) -> string` | Convert bool to `"true"`/`"false"` |
| `i32$$to_string` | `(i32) -> string` | Convert i32 to decimal string |
| `i64$$to_string` | `(i64) -> string` | Convert i64 to decimal string |
| `f32$$to_string` | `(f32) -> string` | Convert f32 to string (via `%g`) |
| `f64$$to_string` | `(f64) -> string` | Convert f64 to string (via `%g`) |
| `string$$to_string` | `(string) -> string` | Identity (returns input) |

## String Interning

Only **literals** are interned. On a `LOAD_CONST` for a string constant, the code calls `roxy_string_from_literal`, which probes the active context's intern table: a hit returns the existing pointer; a miss allocates and inserts one. Repeated loads of the same literal therefore return the same pointer. **Dynamically-created strings** (concat / f-string / `substr` / `to_string` / `read_file`) are **not** interned — they are fresh, uniquely-owned objects allocated via `roxy_string_new_owned`, so freeing one never has to evict an intern entry. The intern table lives in `roxy_ctx.string_intern` — populated by VM mode at `vm_init`, optional in AOT mode.

## Memory Management

Strings are **reference-counted** (lifetime audit finding 9b). The `ref_count` in the `ObjectHeader` is repurposed as an **owner count** (strings are never `ref`-borrowed, so there is no clash with the borrow free-trap): a string copy retains (`roxy_string_retain`), a drop releases (`roxy_string_release`), and the last release frees. Pooled string **literals are immortal** — the sentinel `ref_count == 0xFFFFFFFF` (`ROXY_STR_IMMORTAL`) — because `LOAD_CONST` (and the AOT `roxy_string_from_literal`) returns a persistent, interned object; retain/release are no-ops on an immortal string, so literals are never freed and the pool never dangles. Dynamic strings start at count 1 and are freed at zero.

This is the copyable-with-nontrivial-drop shape that `ref` also has: `string` is `is_copy()` yet needs both a retain and a drop, with `compute_drop_plan(string) → DropKind::StrRelease`. See [lifetimes.md → The value lifecycle](lifetimes.md#the-value-lifecycle) for why such a type is *not* move-only (its drop has an exact inverse).

Retains are emitted at each store site — var decl, assignment, return, struct-field store, container-element store — all now reading one derivation (`compute_retain_plan`), and `StrRelease` on every drop (scope exit, overwrite, container destroy), on both backends (`STR_RETAIN` / `STR_RELEASE` opcodes in the VM; `roxy_string_retain` / `roxy_string_release` in the C backend).

**Scope of reclamation.** Standalone strings, container (`List`/`Map`) string elements, and strings held in **struct fields** are all reference-counted and reclaimed. The struct-field case was the last hole: the string was retained on store (so it never dangled) but never released on struct drop.

**Fixed 2026-08-02.** It was never the "bounded leak" this section once called it — the leak was per *value*, not per type, and the Lox interpreter lost a string per `Token`. The fix is the three-step separation in [lifetimes.md → Separating Drop from Copy](lifetimes.md#separating-drop-from-copy--landed-2026-08-02); Lox's census went from 38 leaked strings to 0.

## Example

```roxy
fun greet(name: string): string {
    return "Hello, " + name + "!";
}

fun main(): i32 {
    var greeting: string = greet("World");
    print(greeting);
    if (greeting == "Hello, World!") {
        return str_len(greeting);  // Returns 13
    }
    return 0;
}
```

## Files

| File | Purpose |
|------|---------|
| `include/roxy/rt/roxy_rt.h` | `roxy_string_header` (unified layout), C string API |
| `src/roxy/rt/roxy_rt.cpp` | `roxy_string_from_literal` / `roxy_string_concat` / `roxy_string_*` impls |
| `include/roxy/rt/string_intern.hpp` | `StringInternTable` definition |
| `src/roxy/rt/string_intern.cpp` | `roxy_string_intern_lookup` / `_insert` — C-callable bridges |
| `include/roxy/vm/string.hpp` | `StringHeader` typedef alias of `roxy_string_header`, VM-side helpers |
| `src/roxy/vm/string.cpp` | Thin shim — `string_alloc(vm, ...)` → `roxy_string_from_literal(...)` |
| `src/roxy/vm/natives.cpp` | Native function wrappers and registration (incl. `to_string`) |
| `src/roxy/compiler/ir_builder.cpp` | String operator rewriting, f-string IR generation |
| `src/roxy/compiler/parser.cpp` | String literal and f-string parsing, escape processing |
| `src/roxy/shared/lexer.cpp` | F-string tokenization with brace depth tracking |
| `include/roxy/compiler/ast.hpp` | `ExprStringInterp` AST node |
| `src/roxy/compiler/semantic.cpp` | Printable trait registration, f-string type checking |
| `include/roxy/compiler/types.hpp` | Primitive method/trait tables in `TypeCache` |
