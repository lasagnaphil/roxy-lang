# Structs

Roxy structs are stack-allocated value types with a packed, slot-based memory layout. They pass and return by value (small structs in registers, large structs by hidden pointer), and reuse the VM's local stack for storage — there is no struct-specific heap object.

## Memory Model

```
┌─────────────────────────────────────────────┐
│ RoxyVM                                      │
├─────────────────────────────────────────────┤
│ register_file: [u64][u64][u64]...          │  ← Untyped 8-byte slots
│ local_stack:   [u32][u32][u32]...          │  ← 4-byte granularity for structs
└─────────────────────────────────────────────┘
```

- **Registers are untyped 8-byte values** (`u64`); no per-register type info is kept at runtime.
- **The local stack uses 4-byte (`u32`) slots** — most values fit this granularity.
- **64-bit values** (`i64`, `f64`, pointers) occupy 2 consecutive slots.
- **Small types** (`i8`, `i16`) are widened to 4 bytes (1 slot).
- **Function frames are 16-byte aligned** — `local_stack_base` is aligned to 4 slots for C++ interop.

## Slot Layout

| Type | Slots | Bytes |
|------|-------|-------|
| `bool`, `i8`, `u8`, `i16`, `u16`, `i32`, `u32`, `f32` | 1 | 4 |
| `i64`, `u64`, `f64`, pointers | 2 | 8 |
| Struct | Sum of field slots | Variable |

Fields are laid out in declaration order, each at the next free slot offset:

```
struct Point { x: i32; y: i32; }
→ x: slot_offset=0, slot_count=1
→ y: slot_offset=1, slot_count=1
→ Total: 2 slots (8 bytes)

struct Data { a: i32; b: i64; }
→ a: slot_offset=0, slot_count=1
→ b: slot_offset=1, slot_count=2
→ Total: 3 slots (12 bytes)
```

Slot counts come from `get_type_slot_count()`; per-field `slot_offset`/`slot_count` are assigned during type resolution (`FieldInfo` / `StructTypeInfo` in `types.hpp`).

## Compilation Pipeline

A struct local is an IR `StackAlloc` (yields a pointer into the local stack); `GetField`/`SetField` access fields through it by `slot_offset`/`slot_count`. Lowering bump-allocates stack slots per function and emits `STACK_ADDR` / `GET_FIELD` / `SET_FIELD` (the field ops are two-word: ABC + 16-bit offset, moving 1 or 2 slots). Each `BCFunction` records `local_stack_slots`; on `CALL` the interpreter aligns `local_stack_base` up to 16 bytes and advances the stack top by that count, and `RET` pops back.

## Struct Literals

Struct literals initialize a struct inline with named fields. Fields with default values may be omitted; field order is irrelevant; all non-defaulted fields must be provided.

```roxy
struct Config { width: i32 = 800; height: i32 = 600; fullscreen: i32 = 0; }

fun main(): i32 {
    var p = Point { x = 10, y = 20 };   // all fields required
    var c = Config { width = 1920 };    // defaults for height, fullscreen
    var d = Config {};                  // all defaults
    var q = Point { y = 5, x = 3 };     // order doesn't matter
    return p.x + c.width;
}
```

Grammar:

```
struct_literal  -> Identifier "{" field_init_list? "}"
field_init_list -> field_init ("," field_init)*
field_init      -> Identifier "=" expression
```

Unknown or duplicate field names, missing non-defaulted fields, and non-assignable values are errors. An omitted field's default expression is evaluated at each literal.

## Struct Parameters and Returns

Structs pass and return by value. Small structs travel in registers; large structs pass by hidden pointer with the callee copying.

| Struct Size | Slots | Passing Mode | Mechanism |
|-------------|-------|--------------|-----------|
| 1–8 bytes   | 1–2   | By value     | 1 register |
| 9–16 bytes  | 3–4   | By value     | 2 registers |
| >16 bytes   | >4    | By reference | Pointer (callee copies) |

Small structs (≤16 bytes) pack into 1–2 registers via `STRUCT_LOAD_REGS` at the call site and unpack via `STRUCT_STORE_REGS` in the callee; small returns use `RET_STRUCT_SMALL` (caller unpacks into the destination struct).

The callee always receives a copy — mutations don't affect the caller:

```roxy
fun modify(p: Point): i32 {
    p.x = 100;      // modifies local copy only
    return p.x;
}

fun main(): i32 {
    var pt = Point { x = 5, y = 10 };
    modify(pt);     // pt.x is still 5
    return pt.x;    // returns 5
}
```

## Out/Inout Parameters

To mutate the caller's struct, use `inout` (read-write) or `out` (write-only); these pass by pointer:

```roxy
fun double_point(p: inout Point) {
    p.x = p.x * 2;
    p.y = p.y * 2;
}

fun main(): i32 {
    var pt = Point { x = 10, y = 20 };
    double_point(inout pt);  // pt is now {20, 40}
    return pt.x + pt.y;      // returns 60
}
```
