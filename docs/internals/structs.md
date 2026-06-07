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

- **Registers are untyped 8-byte values** (`u64`); type info is stored separately for debug mode only.
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

`get_type_slot_count()` (see `types.hpp`) maps each type to its slot count; semantic analysis assigns `slot_offset`/`slot_count` per field and the total `slot_count` per struct during type resolution. These live on `FieldInfo` and `StructTypeInfo` in `types.hpp`.

## Compilation Pipeline

- **SSA IR** — `StackAlloc` allocates local-stack space and yields a pointer; `GetField`/`SetField` read and write fields through that pointer using the field's `slot_offset`/`slot_count`. (`ssa_ir.hpp`.)

  ```
  v0 = stack_alloc 2       // Point struct (2 slots)
  v1 = const_int 10
  v2 = set_field v0.x <- v1
  v3 = const_int 20
  v4 = set_field v0.y <- v3
  v5 = get_field v0.x
  v6 = get_field v0.y
  v7 = add_i v5, v6
  return v7
  ```

- **Bytecode** — three opcodes back struct access. `STACK_ADDR` (0xB2, ABI format) computes a field/struct base address from a stack slot offset; `GET_FIELD` (0xB0) and `SET_FIELD` (0xB1) read/write 1 or 2 slots, each using an ABC word plus a second word holding the 16-bit `slot_offset`. `BCFunction` carries a `local_stack_slots` count sized to hold all local structs. (`bytecode.hpp`.)

- **Lowering** — `BytecodeBuilder` bump-allocates stack slots via `m_next_stack_slot` and maps each `StackAlloc` result ValueId to its stack offset, emitting `STACK_ADDR`. Field ops lower to `GET_FIELD`/`SET_FIELD` with the recorded offset/count. (`lowering.cpp`.)

- **Interpreter** — on `CALL` the frame's `local_stack_base` is aligned up to 4 slots (16 bytes) and `local_stack_top` advances by the callee's `local_stack_slots`; `RET` pops the local stack back to `local_stack_base`. `STACK_ADDR`/`GET_FIELD`/`SET_FIELD` compute `local_stack + base + slot_offset` and move 1 or 2 slots. (`interpreter.cpp`.)

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

`analyze_struct_literal_expr()` checks that the type name resolves to a struct, every named field exists, there are no duplicates, all non-defaulted fields are supplied, and each value is assignable to its field type. IR generation emits `StackAlloc` then one `SetField` per field; omitted fields with defaults evaluate the default expression. The AST nodes are `FieldInit` / `StructLiteralExpr` in `ast.hpp`.

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

## Files

| File | Purpose |
|------|---------|
| `include/roxy/compiler/ast.hpp` | `FieldInit`, `StructLiteralExpr` AST nodes |
| `include/roxy/compiler/types.hpp` | `FieldInfo`, `StructTypeInfo`, `get_type_slot_count()` |
| `src/roxy/compiler/parser.cpp` | Struct literal parsing |
| `src/roxy/compiler/semantic.cpp` | Slot-count computation, struct literal validation |
| `src/roxy/compiler/ir_builder.cpp` | `StackAlloc` and struct literal IR emission |
| `src/roxy/compiler/lowering.cpp` | Stack slot allocation, field access lowering |
| `src/roxy/vm/interpreter.cpp` | `STACK_ADDR` / `GET_FIELD` / `SET_FIELD`, frame local-stack management |
