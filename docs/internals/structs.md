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

## Limitations (Future Work)

- **No struct literals**: `Point { x = 1, y = 2 }` syntax not yet supported
- **No pass-by-value**: Structs cannot be passed to functions by value
- **No return structs**: Functions cannot return struct values
- **No nested structs**: Struct fields cannot be other structs
- **No heap allocation**: `new`/`uniq`/`ref`/`weak` for structs not implemented

## Files

- `include/roxy/compiler/types.hpp` - FieldInfo, StructTypeInfo definitions
- `src/roxy/compiler/semantic.cpp` - Slot count computation
- `src/roxy/compiler/ir_builder.cpp` - StackAlloc emission
- `src/roxy/compiler/lowering.cpp` - Stack slot allocation, field access lowering
- `src/roxy/vm/interpreter.cpp` - Field access opcodes
