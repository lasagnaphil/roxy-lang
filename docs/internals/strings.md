# Strings

This document describes the runtime string implementation in Roxy.

## Overview

Strings in Roxy are heap-allocated objects managed through the object system. They support:
- String literals with escape sequences
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

Example:
```
var msg: string = "Line 1\nLine 2\tTabbed";
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
| `src/roxy/vm/natives.cpp` | Native function wrappers and registration |
| `src/roxy/compiler/ir_builder.cpp` | String operator rewriting |
| `src/roxy/compiler/parser.cpp` | String literal parsing and escape processing |

## Example

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
