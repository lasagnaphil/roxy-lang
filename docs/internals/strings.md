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
```

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

String literals are interned. On a `LOAD_CONST` for a string constant, the interpreter calls `string_alloc()` (a thin shim over `roxy_string_from_literal`), which probes the active context's intern table: a hit returns the existing pointer; a miss allocates a new string object and inserts it. Repeated `LOAD_CONST` of the same literal therefore returns the same pointer. The intern table lives in `roxy_ctx.string_intern` — populated by VM mode at `vm_init`, optional in AOT mode.

## Memory Management

Strings are managed through the unified runtime. They are allocated via `roxy_alloc()` (with `ROXY_TYPEID_STRING`), which dispatches through `roxy_ctx.allocator` — the slab in both VM and AOT modes (after `roxy_rt_init`) — and reference-counted via their `ObjectHeader`.

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
