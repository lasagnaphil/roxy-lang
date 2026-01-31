# Structs

Roxy supports stack-allocated value-type structs with a packed slot-based memory layout.

## Memory Model

```
┌─────────────────────────────────────────────┐
│ RoxyVM                                      │
├─────────────────────────────────────────────┤
│ register_file: [u64][u64][u64]...          │  ← Untyped 8-byte slots
│ local_stack:   [u32][u32][u32]...          │  ← 4-byte granularity for structs
└─────────────────────────────────────────────┘
```

**Design decisions:**
- **Registers are untyped 8-byte values** (`u64`) - type info stored separately for debug mode only
- **Local stack uses 4-byte (`u32`) slots** - most values fit in this granularity
- **64-bit values** (`i64`, `f64`, pointers) use 2 consecutive slots
- **Small types** (`i8`, `i16`) are widened to 4 bytes (1 slot)
- **16-byte aligned function frames** - `local_stack_base` aligned to 4 slots for C++ interop

## Slot Layout

| Type | Slots | Bytes |
|------|-------|-------|
| `bool`, `i8`, `u8`, `i16`, `u16`, `i32`, `u32`, `f32` | 1 | 4 |
| `i64`, `u64`, `f64`, pointers | 2 | 8 |
| Struct | Sum of field slots | Variable |

Example:
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

## Type System Extensions

```cpp
struct FieldInfo {
    StringView name;
    Type* type;
    bool is_pub;
    u32 index;
    u32 slot_offset;  // Offset in u32 slots from struct start
    u32 slot_count;   // Number of u32 slots this field occupies
};

struct StructTypeInfo {
    StringView name;
    Decl* decl;
    Type* parent;
    Span<FieldInfo> fields;
    u32 slot_count;   // Total slots needed for struct
};
```

## Semantic Analysis

The `get_type_slot_count()` helper computes slot counts:

```cpp
u32 get_type_slot_count(Type* type) {
    switch (type->kind) {
        case TypeKind::Bool:
        case TypeKind::I8: case TypeKind::U8:
        case TypeKind::I16: case TypeKind::U16:
        case TypeKind::I32: case TypeKind::U32:
        case TypeKind::F32:
            return 1;  // 4 bytes
        
        case TypeKind::I64: case TypeKind::U64:
        case TypeKind::F64:
            return 2;  // 8 bytes
        
        case TypeKind::Struct:
            return type->struct_info.slot_count;
        
        default: return 0;
    }
}
```

Field slot offsets are computed during type resolution:
```cpp
u32 current_slot = 0;
for (auto& field : fields) {
    field.slot_count = get_type_slot_count(field.type);
    field.slot_offset = current_slot;
    current_slot += field.slot_count;
}
type->struct_info.slot_count = current_slot;
```

## SSA IR

New IR instruction for struct allocation:

```cpp
enum class IROp {
    // ...
    StackAlloc,   // Allocate space on local stack, return pointer
    GetField,     // Read field from struct pointer
    SetField,     // Write field to struct pointer
};

struct StackAllocData {
    u32 slot_count;  // Number of u32 slots to allocate
};

struct FieldData {
    ValueId object;
    StringView field_name;
    u32 slot_offset;   // Offset in u32 slots
    u32 slot_count;    // 1 for 32-bit, 2 for 64-bit
};
```

IR output example:
```
fn main() -> i32 {
b0 [entry]:
    v0 = stack_alloc 2       // Point struct (2 slots)
    v1 = const_int 10
    v2 = set_field v0.x <- v1
    v3 = const_int 20
    v4 = set_field v0.y <- v3
    v5 = get_field v0.x
    v6 = get_field v0.y
    v7 = add_i v5, v6
    return v7
}
```

## Bytecode

New opcode and function metadata:

```cpp
enum class Opcode : u8 {
    // ...
    STACK_ADDR  = 0xB2,  // dst = &local_stack[local_stack_base + imm16]
    GET_FIELD   = 0xB0,  // dst = *(obj + slot_offset), uses slot_count
    SET_FIELD   = 0xB1,  // *(obj + slot_offset) = val, uses slot_count
};

struct BCFunction {
    StringView name;
    u32 param_count;
    u32 register_count;
    u32 local_stack_slots;  // Slots needed for local structs
    Vector<u32> code;
    Vector<BCConstant> constants;
};
```

**Instruction encoding:**
- `STACK_ADDR`: `[STACK_ADDR dst][slot_offset:16]` - ABI format
- `GET_FIELD`: `[GET_FIELD dst obj slot_count][slot_offset:16]` - ABC + extra word
- `SET_FIELD`: `[SET_FIELD obj val slot_count][slot_offset:16]` - ABC + extra word

## Lowering

The BytecodeBuilder tracks stack slot allocation:

```cpp
class BytecodeBuilder {
    u32 m_next_stack_slot = 0;
    tsl::robin_map<u32, u32> m_value_to_stack_slot;  // ValueId -> stack offset
};
```

Lowering `StackAlloc`:
```cpp
case IROp::StackAlloc: {
    u32 slot_count = inst->stack_alloc.slot_count;
    u32 slot_offset = m_next_stack_slot;
    m_next_stack_slot += slot_count;
    m_value_to_stack_slot[inst->result.id] = slot_offset;
    emit_abi(Opcode::STACK_ADDR, dst, static_cast<u16>(slot_offset));
    break;
}
```

## Interpreter

The interpreter manages the local stack during function calls:

```cpp
case Opcode::CALL: {
    // ... existing register allocation ...
    
    // Align to 4 slots (16 bytes) for C++ interop
    u32 local_base = (vm->local_stack_top + 3) & ~3u;
    
    // Allocate local stack slots
    vm->local_stack_top = local_base + callee->local_stack_slots;
    new_frame.local_stack_base = local_base;
    // ...
}

case Opcode::RET: {
    // ... existing logic ...
    vm->local_stack_top = frame->local_stack_base;  // Pop local stack
    // ...
}
```

Field access implementation:
```cpp
case Opcode::STACK_ADDR: {
    u16 slot_offset = imm;
    u32* addr = vm->local_stack + frame->local_stack_base + slot_offset;
    regs[a] = reinterpret_cast<u64>(addr);
    break;
}

case Opcode::GET_FIELD: {
    u8 slot_count = c;
    u16 slot_offset = static_cast<u16>(*pc++);  // Second instruction word
    
    u32* base = reinterpret_cast<u32*>(regs[b]);
    u32* field = base + slot_offset;
    
    if (slot_count == 1) {
        regs[a] = static_cast<u64>(*field);
    } else {
        regs[a] = static_cast<u64>(field[0]) | (static_cast<u64>(field[1]) << 32);
    }
    break;
}

case Opcode::SET_FIELD: {
    u8 slot_count = c;
    u16 slot_offset = static_cast<u16>(*pc++);
    
    u32* base = reinterpret_cast<u32*>(regs[a]);
    u32* field = base + slot_offset;
    u64 val = regs[b];
    
    if (slot_count == 1) {
        *field = static_cast<u32>(val);
    } else {
        field[0] = static_cast<u32>(val);
        field[1] = static_cast<u32>(val >> 32);
    }
    break;
}
```

## Struct Literals

Struct literals allow inline initialization with named field syntax:

```
struct Point { x: i32; y: i32; }
struct Config { width: i32 = 800; height: i32 = 600; fullscreen: i32 = 0; }

fun main(): i32 {
    var p = Point { x = 10, y = 20 };       // All fields required
    var c = Config { width = 1920 };         // Uses defaults for height, fullscreen
    var d = Config {};                       // All defaults
    var q = Point { y = 5, x = 3 };          // Order doesn't matter
    return p.x + c.width;
}
```

### Grammar

```
struct_literal  -> Identifier "{" field_init_list? "}"
field_init_list -> field_init ("," field_init)*
field_init      -> Identifier "=" expression
```

### AST Representation

```cpp
struct FieldInit {
    StringView name;
    Expr* value;
    SourceLocation loc;
};

struct StructLiteralExpr {
    StringView type_name;
    Span<FieldInit> fields;
};
```

### Semantic Analysis

The `analyze_struct_literal_expr()` method validates:
1. Type name resolves to a struct type
2. All field names exist in the struct
3. No duplicate field initializers
4. All fields without default values are provided
5. Field value types are assignable to field types

### IR Generation

Struct literals emit `StackAlloc` followed by `SetField` for each field:

```
v0 = stack_alloc 2           // Allocate Point (2 slots)
v1 = const_int 10
v2 = set_field v0.x <- v1    // Initialize x
v3 = const_int 20
v4 = set_field v0.y <- v3    // Initialize y
// v0 is the struct pointer
```

For fields with default values that aren't provided in the literal, the default expression is evaluated and used.

## Struct Parameters and Returns

Structs can be passed to and returned from functions with automatic value semantics:

| Struct Size | Slots | Passing Mode | Mechanism |
|-------------|-------|--------------|-----------|
| 1-8 bytes   | 1-2   | By value     | 1 register |
| 9-16 bytes  | 3-4   | By value     | 2 registers |
| >16 bytes   | >4    | By reference | Pointer (callee copies) |

### Small Struct Passing (≤16 bytes)

Small structs are packed into 1-2 registers:
- `STRUCT_LOAD_REGS` - Pack struct slots into registers at call site
- `STRUCT_STORE_REGS` - Unpack registers into struct slots in callee

### Small Struct Returns

Small structs are returned in 1-2 registers:
- `RET_STRUCT_SMALL` - Pack struct and return
- Caller unpacks into destination struct

### Value Semantics

Callee always receives a copy - modifications don't affect caller:

```
fun modify(p: Point): i32 {
    p.x = 100;      // Modifies local copy
    return p.x;
}

fun main(): i32 {
    var pt = Point { x = 5, y = 10 };
    modify(pt);     // pt.x is still 5
    return pt.x;    // Returns 5
}
```

## Out/Inout Parameters

For modifying caller's struct, use `inout` or `out` modifiers:

```
fun double_point(p: inout Point) {
    p.x = p.x * 2;
    p.y = p.y * 2;
}

fun main(): i32 {
    var pt = Point { x = 10, y = 20 };
    double_point(inout pt);  // pt is now {20, 40}
    return pt.x + pt.y;      // Returns 60
}
```

## Files

- `include/roxy/compiler/ast.hpp` - FieldInit, StructLiteralExpr AST nodes
- `include/roxy/compiler/types.hpp` - FieldInfo, StructTypeInfo definitions
- `src/roxy/compiler/parser.cpp` - Struct literal parsing
- `src/roxy/compiler/semantic.cpp` - Slot count computation, struct literal validation
- `src/roxy/compiler/ir_builder.cpp` - StackAlloc and struct literal IR emission
- `src/roxy/compiler/lowering.cpp` - Stack slot allocation, field access lowering
- `src/roxy/vm/interpreter.cpp` - Field access opcodes
