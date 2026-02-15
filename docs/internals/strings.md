# Strings

This document describes the runtime string implementation in Roxy.

## Overview

Strings in Roxy are heap-allocated objects managed through the object system. They support:
- String literals with escape sequences
- F-string interpolation (`f"hello {expr}"`)
- Concatenation via `+` operator or `str_concat()` function
- Equality/inequality comparison via `==`/`!=` operators
- Length query via `str_len()` function
- Printing via `print_str()` function

## Memory Layout

Strings are allocated through the object system with the following memory layout:

```
┌─────────────────┬─────────────────┬─────────────────────────┐
│  ObjectHeader   │  StringHeader   │  Character Data + '\0'  │
│    (16 bytes)   │    (8 bytes)    │     (length + 1)        │
└─────────────────┴─────────────────┴─────────────────────────┘
```

### StringHeader

```cpp
struct StringHeader {
    u32 length;    // String length (excluding null terminator)
    u32 capacity;  // Allocated capacity (including null terminator)
};
```

The character data immediately follows the header and is always null-terminated for C interoperability.

## String Literals

String literals in source code are enclosed in double quotes:

```
var s: string = "hello world";
```

### Escape Sequences

The parser processes the following escape sequences:

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

Example:
```
var msg: string = "Line 1\nLine 2\tTabbed";
```

## F-String Interpolation

F-strings provide string interpolation using the `f"..."` syntax. Expressions inside `{}` are converted to strings and concatenated:

```
var name: string = "World";
var greeting: string = f"Hello, {name}!";  // "Hello, World!"
```

### Syntax

```
f"text {expression} more text {expression} end"
```

- Text segments between `{}` are literal string content
- Expressions inside `{}` can be any valid expression: variables, arithmetic, function calls, etc.
- Use `\{` and `\}` to include literal braces

### Supported Types

Any type that implements the builtin `Printable` trait can be interpolated. The following primitive types implement `Printable` automatically:

| Type | Example | Output |
|------|---------|--------|
| `bool` | `f"{true}"` | `"true"` |
| `i32` | `f"{42}"` | `"42"` |
| `i64` | `f"{1000000l}"` | `"1000000"` |
| `f32` | `f"{3.14f}"` | `"3.14"` |
| `f64` | `f"{3.14}"` | `"3.14"` |
| `string` | `f"{'hello'}"` | `"hello"` |

Structs that implement the `Printable` trait (by providing a `to_string(): string` method) can also be interpolated.

### Expressions in Interpolations

Arbitrary expressions are supported inside `{}`:

```
var a: i32 = 3;
var b: i32 = 4;
print_str(f"{a} + {b} = {a + b}");  // "3 + 4 = 7"

fun double_it(x: i32): i32 { return x * 2; }
print_str(f"result: {double_it(5)}");  // "result: 10"
```

Struct literals with braces work correctly because the lexer tracks brace nesting depth:

```
print_str(f"point: {Point { x = 1, y = 2 }}");
```

### Compilation Pipeline

F-strings are processed through each stage of the compiler:

1. **Lexer**: Produces `FStringBegin`, `FStringMid`, `FStringEnd` tokens with brace depth tracking
2. **Parser**: Builds an `ExprStringInterp` AST node containing text parts and expression sub-trees
3. **Semantic analysis**: Validates each interpolated expression implements the `Printable` trait
4. **IR builder**: Generates `to_string()` calls for non-string expressions, then chains `str_concat` calls in a left-fold

For example, `f"x = {x}, y = {y}"` generates IR equivalent to:

```
str_concat(str_concat(str_concat("x = ", i32$$to_string(x)), ", y = "), i32$$to_string(y))
```

### Printable Trait

The `Printable` trait is a builtin trait registered during semantic analysis. It requires one method:

```
// Trait definition (built-in, not user-declared):
// trait Printable;
// fun Printable.to_string(): string;
```

Primitive types have their `to_string()` implementations registered as native functions. User-defined structs can implement `Printable` by providing a `to_string()` method via the standard trait mechanism:

```
trait Printable;
fun Printable.to_string(): string;

struct Point {
    x: i32;
    y: i32;
}

fun Point.to_string(): string for Printable {
    return f"({self.x}, {self.y})";
}

// Now Point can be interpolated:
var p: Point = Point { x = 1, y = 2 };
print_str(f"point = {p}");  // "point = (1, 2)"
```

## String Operations

### Concatenation

String concatenation can be done via the `+` operator or the `str_concat()` function:

```
var a: string = "hello";
var b: string = " world";
var c: string = a + b;           // Using + operator
var d: string = str_concat(a, b); // Using function
```

The `+` operator is rewritten by the IR builder to a `str_concat` native function call.

### Equality Comparison

Strings can be compared for equality using `==` and `!=`:

```
if (name == "admin") {
    // ...
}
if (password != "") {
    // ...
}
```

These operators are rewritten by the IR builder to `str_eq` and `str_ne` native function calls respectively. Comparison is done by:
1. Checking if pointers are identical (fast path)
2. Comparing lengths
3. Using `memcmp` for character-by-character comparison

### Length

Get the length of a string using `str_len()`:

```
var s: string = "hello";
var len: i32 = str_len(s);  // Returns 5
```

### Printing

Print a string to stdout using `print_str()`:

```
print_str("Hello, World!");
```

## Native Functions

All string operations are implemented as native functions:

| Function | Signature | Description |
|----------|-----------|-------------|
| `str_concat` | `(string, string) -> string` | Concatenate two strings |
| `str_eq` | `(string, string) -> bool` | Test equality |
| `str_ne` | `(string, string) -> bool` | Test inequality |
| `str_len` | `(string) -> i32` | Get string length |
| `print_str` | `(string) -> void` | Print string to stdout |
| `bool$$to_string` | `(bool) -> string` | Convert bool to `"true"`/`"false"` |
| `i32$$to_string` | `(i32) -> string` | Convert i32 to decimal string |
| `i64$$to_string` | `(i64) -> string` | Convert i64 to decimal string |
| `f32$$to_string` | `(f32) -> string` | Convert f32 to string (via `%g`) |
| `f64$$to_string` | `(f64) -> string` | Convert f64 to string (via `%g`) |
| `string$$to_string` | `(string) -> string` | Identity (returns input) |

## Implementation Details

### Constant Loading

When the interpreter encounters a `LOAD_CONST` instruction for a string constant, it:
1. Reads the string data and length from the constant pool
2. Calls `string_alloc()` to create a new StringObject
3. Stores the pointer in the destination register

This means each string constant load creates a new string object (no string interning).

### IR Builder Rewriting

The IR builder detects binary operations on string operands and rewrites them:

```
// Source code
var result: string = a + b;

// Becomes equivalent to
var result: string = str_concat(a, b);
```

This is done in `gen_binary_expr()` by checking if `left_type->kind == TypeKind::String`.

### Memory Management

Strings are managed through the object system:
- Allocated via `object_alloc()` with the registered string type ID
- Reference counted via `ObjectHeader`
- Memory layout includes `ObjectHeader` for ref counting support

## Files

| File | Purpose |
|------|---------|
| `include/roxy/vm/string.hpp` | StringHeader struct, function declarations |
| `src/roxy/vm/string.cpp` | string_alloc, string_concat, string_equals |
| `src/roxy/vm/natives.cpp` | Native function wrappers and registration (incl. `to_string`) |
| `src/roxy/compiler/ir_builder.cpp` | String operator rewriting, f-string IR generation |
| `src/roxy/compiler/parser.cpp` | String literal and f-string parsing, escape processing |
| `src/roxy/shared/lexer.cpp` | F-string tokenization with brace depth tracking |
| `include/roxy/compiler/ast.hpp` | `ExprStringInterp` AST node |
| `src/roxy/compiler/semantic.cpp` | Printable trait registration, f-string type checking |
| `include/roxy/compiler/types.hpp` | Primitive method/trait tables in `TypeCache` |

## Examples

### String Concatenation

```
fun greet(name: string): string {
    return "Hello, " + name + "!";
}

fun main(): i32 {
    var greeting: string = greet("World");
    print_str(greeting);

    if (greeting == "Hello, World!") {
        return str_len(greeting);  // Returns 13
    }
    return 0;
}
```

### F-String Interpolation

```
fun main(): i32 {
    var name: string = "World";
    var count: i32 = 3;
    var score: f64 = 9.5;

    // Basic interpolation
    print_str(f"Hello, {name}!");           // "Hello, World!"

    // Multiple types
    print_str(f"{name} v{count}: {score}"); // "World v3: 9.5"

    // Expressions in braces
    var a: i32 = 3;
    var b: i32 = 4;
    print_str(f"{a} + {b} = {a + b}");     // "3 + 4 = 7"

    // Escaped braces
    print_str(f"use \{ and \}");            // "use { and }"

    return 0;
}
```
