Here's a comprehensive summary of the Roxy VM design we've discussed:

---

# Roxy Language Implementation Summary

## Overview

Roxy is an embeddable scripting language for game engines with:
- Static typing
- Value semantics by default
- Memory management via `uniq`/`ref`/`weak` references (no GC)
- Fast C++ interop
- Future AOT compilation to C

---

## Compiler Pipeline

```
Source → Lexer → Parser → AST → SSA IR (block arguments) → Register Bytecode → VM
```

### Key Design Decisions

| Stage | Approach | Rationale |
|-------|----------|-----------|
| Parsing | Separate compiler/LSP parsers, shared lexer | Compiler can fail-fast; LSP needs error recovery |
| IR | SSA with block arguments (not phi nodes) | Cleaner dataflow, easier lowering |
| Bytecode | Register-based, 32-bit fixed-width | Easy C transpilation, natural SSA lowering |
| Memory | Arena allocation (compiler), NodeID maps (LSP) | Fast batch compile; incremental LSP updates |

---

## Memory Model

### Three Reference Types

| Type | Owns? | Nullable? | On dangling |
|------|-------|-----------|-------------|
| `uniq` | Yes | No | N/A (is owner) |
| `ref` | No | No | Assert/crash |
| `weak` | No | Yes | Returns null or asserts |

### Critical Rule: No `ref` in Fields

To prevent reference cycles, `ref` can only be used for:
- Function parameters
- Local variables

Struct fields must use `uniq` (ownership) or `weak` (back-references).

### Object Header

**Status: ✅ Implemented** (`include/roxy/vm/object.hpp`, `src/roxy/vm/object.cpp`)

```cpp
struct ObjectHeader {
    u32 ref_count;          // Number of active 'ref' pointers
    u32 weak_generation;    // Bumped on delete to invalidate weak refs
    u32 type_id;            // Type identifier for runtime type info
    u32 size;               // Total size including header

    void* data();           // Get pointer to object data (after header)
};

// Reference counting operations
void ref_inc(void* data);
bool ref_dec(RoxyVM* vm, void* data);  // Returns true if deallocated

// Weak reference operations
u32 weak_ref_create(void* data);
bool weak_ref_valid(void* data, u32 generation);
void weak_ref_invalidate(void* data);

// Object allocation/deallocation
void* object_alloc(RoxyVM* vm, u32 type_id, u32 data_size);
void object_free(RoxyVM* vm, void* data);
```

---

## Bytecode Format

**Status: ✅ Implemented** (`include/roxy/vm/bytecode.hpp`, `src/roxy/vm/bytecode.cpp`)

### Instruction Encoding

All instructions are 32-bit fixed-width with three formats:

```
Format A: [opcode:8][dst:8][src1:8][src2:8]    — 3-operand (arithmetic, comparisons)
Format B: [opcode:8][dst:8][imm16:16]          — immediate (constants, loads)
Format C: [opcode:8][reg:8][offset:16]         — branch/field access (jumps)
```

Helper functions for encoding/decoding:
```cpp
u32 encode_abc(Opcode op, u8 dst, u8 src1, u8 src2);
u32 encode_abi(Opcode op, u8 dst, u16 imm);
u32 encode_aoff(Opcode op, u8 reg, i16 offset);

Opcode decode_opcode(u32 instr);
u8 decode_a(u32 instr);  // dst or reg
u8 decode_b(u32 instr);  // src1
u8 decode_c(u32 instr);  // src2
u16 decode_imm16(u32 instr);
i16 decode_offset(u32 instr);
```

### Calling Convention

```
Arguments:  R0, R1, R2, R3, ... (left to right)
Return:     R0
```

Each function call allocates a new register window from the shared register file.

### Core Opcode Categories

| Range | Category | Key Opcodes |
|-------|----------|-------------|
| 0x00-0x0F | Constants, Moves | `LOAD_NULL`, `LOAD_TRUE`, `LOAD_FALSE`, `LOAD_INT`, `LOAD_CONST`, `MOV` |
| 0x10-0x1F | Integer Arithmetic | `ADD_I`, `SUB_I`, `MUL_I`, `DIV_I`, `MOD_I`, `NEG_I` |
| 0x20-0x2F | Float Arithmetic | `ADD_F`, `SUB_F`, `MUL_F`, `DIV_F`, `NEG_F` |
| 0x30-0x3F | Bitwise | `BIT_AND`, `BIT_OR`, `BIT_XOR`, `BIT_NOT`, `SHL`, `SHR`, `USHR` |
| 0x40-0x4F | Integer Comparisons | `EQ_I`, `NE_I`, `LT_I`, `LE_I`, `GT_I`, `GE_I`, `LT_U`, `LE_U`, `GT_U`, `GE_U` |
| 0x50-0x5F | Float Comparisons | `EQ_F`, `NE_F`, `LT_F`, `LE_F`, `GT_F`, `GE_F` |
| 0x60-0x6F | Logical | `NOT`, `AND`, `OR` |
| 0x80-0x8F | Type Conversions | `I2F`, `F2I`, `I2B`, `B2I` |
| 0x90-0x9F | Control Flow | `JMP`, `JMP_IF`, `JMP_IF_NOT`, `RET`, `RET_VOID` |
| 0xA0-0xAF | Function Calls | `CALL`, `CALL_NATIVE` |
| 0xB0-0xBF | Field Access | `GET_FIELD`, `SET_FIELD` |
| 0xC0-0xCF | Index Access | `GET_INDEX`, `SET_INDEX` |
| 0xD0-0xDF | Object Lifecycle | `NEW_OBJ`, `DEL_OBJ` |
| 0xE0-0xEF | Reference Counting | `REF_INC`, `REF_DEC`, `WEAK_CHECK` |
| 0xFE-0xFF | Debug/Special | `NOP`, `HALT` |

### Bytecode Structures

```cpp
// Constant pool entry
struct BCConstant {
    enum Type : u8 { Null, Bool, Int, Float, String };
    Type type;
    union {
        bool as_bool;
        i64 as_int;
        f64 as_float;
        struct { const char* data; u32 length; } as_string;
    };
};

// Bytecode function
struct BCFunction {
    StringView name;
    u32 param_count;
    u32 register_count;
    Vector<u32> code;           // Bytecode instructions
    Vector<BCConstant> constants; // Constant pool
};

// Bytecode module
struct BCModule {
    StringView name;
    Vector<BCFunction*> functions;
    Vector<BCNativeFunction> native_functions;
};
```

---

## Core Data Structures

### Value Representation

**Status: ✅ Implemented** (`include/roxy/vm/value.hpp`, `src/roxy/vm/value.cpp`)

```cpp
struct Value {
    enum Type : u8 {
        Null, Bool, Int, Float, Ptr, Weak
    };

    Type type;
    union {
        bool as_bool;
        i64 as_int;
        f64 as_float;
        void* as_ptr;
        struct { void* ptr; u32 generation; } as_weak;
    };

    // Factory methods
    static Value make_null();
    static Value make_bool(bool b);
    static Value make_int(i64 i);
    static Value make_float(f64 f);
    static Value make_ptr(void* p);
    static Value make_weak(void* p, u32 generation);

    // Type checks and utilities
    bool is_null() const;
    bool is_truthy() const;
    bool is_weak_valid() const;

    // Untyped register conversion (for VM interop)
    u64 as_u64() const;
    static Value from_u64(u64 bits);
    static Value float_from_u64(u64 bits);
};
```

**Note**: At runtime, VM registers are untyped `u64` values. The `Value` struct is used for the public API and native function interface. Type information is preserved only for debugging.

### VM State

**Status: ✅ Implemented** (`include/roxy/vm/vm.hpp`, `src/roxy/vm/vm.cpp`)

```cpp
struct RoxyVM {
    BCModule* module;

    u64* register_file;            // Untyped 8-byte register slots
    u32 register_file_size;
    u32 register_top;              // Current top of register allocation

    u32* local_stack;              // 4-byte slot array for struct data
    u32 local_stack_size;          // Capacity in slots
    u32 local_stack_top;           // Current top in slots

    Vector<CallFrame> call_stack;

    bool running;
    const char* error;
};

struct CallFrame {
    const BCFunction* func;
    const u32* pc;                 // Program counter (pointer into code)
    u64* registers;                // Window into register_file
    u8 return_reg;                 // Register for return value in caller
    u32 local_stack_base;          // Base slot index in local_stack
};

// VM API
bool vm_init(RoxyVM* vm, const VMConfig& config = VMConfig());
void vm_destroy(RoxyVM* vm);
bool vm_load_module(RoxyVM* vm, BCModule* module);
bool vm_call(RoxyVM* vm, StringView func_name, Span<Value> args);
Value vm_get_result(RoxyVM* vm);
```

### Type Descriptor

```cpp
struct TypeDescriptor {
    const char* name;
    uint32_t size;
    uint32_t alignment;
    std::vector<FieldDescriptor> fields;
    std::vector<MethodDescriptor> methods;
    
    // For ref counting during destruction
    std::vector<uint16_t> ref_field_offsets;
    std::vector<uint16_t> weak_field_offsets;
};

struct FieldDescriptor {
    const char* name;
    FieldType type;       // Int, Float, Bool, Struct, Ref, Weak, Uniq
    uint16_t offset;
    uint16_t size;
    TypeId nested_type;
};
```

---

## Interpreter Loop

**Status: ✅ Implemented** (`include/roxy/vm/interpreter.hpp`, `src/roxy/vm/interpreter.cpp`)

The interpreter uses a switch-based dispatch loop for portability (works with MSVC/clang-cl on Windows):

```cpp
bool interpret(RoxyVM* vm) {
    CallFrame* frame = &vm->call_stack.back();
    const BCFunction* func = frame->func;
    const u32* pc = frame->pc;
    Value* regs = frame->registers;

    while (vm->running) {
        u32 instr = *pc++;
        Opcode op = decode_opcode(instr);
        u8 a = decode_a(instr);
        u8 b = decode_b(instr);
        u8 c = decode_c(instr);

        switch (op) {
            case Opcode::LOAD_NULL:
                regs[a] = Value::make_null();
                break;

            case Opcode::ADD_I:
                regs[a] = Value::make_int(regs[b].as_int + regs[c].as_int);
                break;

            case Opcode::JMP:
                pc += decode_offset(instr);
                break;

            case Opcode::JMP_IF:
                if (regs[a].is_truthy()) {
                    pc += decode_offset(instr);
                }
                break;

            case Opcode::CALL: {
                // Save current PC, allocate callee registers
                frame->pc = pc;
                Value* callee_regs = &vm->register_file[vm->register_top];
                vm->register_top += callee->register_count;

                // Copy arguments, push new frame
                for (u8 i = 0; i < arg_count; i++) {
                    callee_regs[i] = regs[first_arg + i];
                }
                vm->call_stack.push_back(CallFrame(callee, callee->code.data(), callee_regs, dst));

                // Update cached frame pointers
                frame = &vm->call_stack.back();
                func = frame->func;
                pc = frame->pc;
                regs = frame->registers;
                break;
            }

            case Opcode::RET: {
                Value result = regs[a];
                vm->call_stack.pop_back();
                vm->register_top -= func->register_count;

                if (vm->call_stack.empty()) {
                    vm->register_file[0] = result;
                    vm->running = false;
                    return true;
                }

                // Restore caller frame
                frame = &vm->call_stack.back();
                func = frame->func;
                pc = frame->pc;
                regs = frame->registers;
                regs[frame->return_reg] = result;
                break;
            }

            // ... all other opcodes
        }
    }
    return true;
}
```

Key implementation notes:
- Division by zero is checked and returns an error
- Array bounds checking with error reporting
- All opcodes are implemented except field access (GET_FIELD/SET_FIELD have placeholders)
- Error messages are stored in `vm->error`

---

## Arrays

**Status: ✅ Implemented** (`include/roxy/vm/array.hpp`, `src/roxy/vm/array.cpp`)

### Array Layout

Arrays are stored as objects with an ArrayHeader followed by Value elements:

```cpp
struct ArrayHeader {
    u32 length;    // Number of elements
    u32 capacity;  // Allocated capacity (for future growth)
};

// Memory layout: [ObjectHeader][ArrayHeader][Value * length]
```

### Array Operations

```cpp
// Allocate an array with given length (capacity = length)
void* array_alloc(RoxyVM* vm, u32 length);

// Get array length
u32 array_length(void* data);

// Bounds-checked element access
bool array_get(void* data, i64 index, Value& out, const char** error);
bool array_set(void* data, i64 index, Value value, const char** error);
```

### Array Opcodes

| Opcode | Format | Description |
|--------|--------|-------------|
| `GET_INDEX` (0xC0) | ABC | `dst = src1[src2]` - Load array element |
| `SET_INDEX` (0xC1) | ABC | `dst[src1] = src2` - Store array element |

Both opcodes perform null checks and bounds checking, setting `vm->error` on failure.

---

## Structs

**Status: ✅ Implemented** (Types: `types.hpp`, Semantic: `semantic.cpp`, IR: `ir_builder.cpp`, Lowering: `lowering.cpp`, VM: `interpreter.cpp`)

Roxy supports stack-allocated value-type structs with a packed slot-based memory layout.

### Memory Model

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

### Slot Layout

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

### Type System Extensions

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

### Semantic Analysis

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

### SSA IR

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

### Bytecode

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

### Lowering

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

### Interpreter

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

### Limitations (Future Work)

- **No struct literals**: `Point { x = 1, y = 2 }` syntax not yet supported
- **No pass-by-value**: Structs cannot be passed to functions by value
- **No return structs**: Functions cannot return struct values
- **No nested structs**: Struct fields cannot be other structs
- **No heap allocation**: `new`/`uniq`/`ref`/`weak` for structs not implemented

---

## Native Functions

**Status: ✅ Implemented** (`include/roxy/vm/natives.hpp`, `src/roxy/vm/natives.cpp`)

### Native Function Infrastructure

Native functions are registered with the bytecode module and called via `CALL_NATIVE`:

```cpp
// Native function signature
typedef void (*NativeFunction)(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg);

// Register a native function
void vm_register_native(BCModule* module, StringView name, NativeFunction func);
```

### Built-in Array Functions

| Function | Signature | Description |
|----------|-----------|-------------|
| `array_new_int` | `(size: i32) -> i32[]` | Allocate int array initialized to 0 |
| `array_len` | `(arr: i32[]) -> i32` | Return array length |

### Calling Convention

```
CALL_NATIVE dst, func_idx, argc

Arguments:  dst+1, dst+2, ... (consecutive registers)
Return:     dst
```

The native function reads arguments from `vm->call_stack.back().registers[first_arg + i]` and writes the result to `registers[dst]`.

### Semantic Integration

Built-in functions are registered during semantic analysis initialization:

```cpp
void SemanticAnalyzer::register_builtins() {
    Type* i32_type = m_types.i32_type();
    Type* i32_array = m_types.array_type(i32_type);

    declare_native("array_new_int", {i32_type}, i32_array);
    declare_native("array_len", {i32_array}, i32_type);
}
```

The IR builder detects calls to these functions and emits `CallNative` IR instead of regular `Call` IR.

---

## C++ Interop

**Status: ✅ Implemented** (`include/roxy/vm/binding/`)

The interop system provides type-safe C++ function binding with automatic wrapper generation.

### Core Components

| File | Purpose |
|------|---------|
| `type_traits.hpp` | `RoxyType<T>` - Maps C++ types to Roxy types |
| `function_traits.hpp` | `FunctionTraits` - Compile-time signature extraction |
| `binder.hpp` | `FunctionBinder` - Automatic wrapper generation |
| `registry.hpp` | `NativeRegistry` - Unified registration system |

### Type Traits

```cpp
// Maps C++ types to Roxy types at compile time
template<> struct RoxyType<i32> {
    static Type* get(TypeCache& tc) { return tc.i32_type(); }
    static i32 from_value(const Value& v) { return static_cast<i32>(v.as_int); }
    static Value to_value(i32 val) { return Value::make_int(val); }
};
// Specializations for: void, bool, i8-i64, u8-u64, f32, f64
```

### Function Binder

```cpp
// Automatically generates native wrapper for any C++ function
template<auto FnPtr>
struct FunctionBinder {
    static void invoke(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
        Value* regs = vm->call_stack.back().registers;
        // Extract args from registers, call function, store result
        auto result = call_with_args(FnPtr, regs, first_arg);
        regs[dst] = RoxyType<decltype(result)>::to_value(result);
    }
    static NativeFunction get() { return &invoke; }
};
```

### NativeRegistry

```cpp
class NativeRegistry {
public:
    // Automatic binding - generates wrapper from C++ function
    template<auto FnPtr>
    void bind(const char* name);

    // Manual binding - for functions needing VM access (arrays, etc.)
    void bind_native(const char* name, NativeFunction func,
                     std::initializer_list<NativeTypeKind> params,
                     NativeTypeKind return_type);

    // Apply to compiler (semantic analysis)
    void apply_to_symbols(SymbolTable& symbols);

    // Apply to runtime (VM execution)
    void apply_to_module(BCModule* module);

    // Lookup for IR builder
    i32 get_index(StringView name) const;
    bool is_native(StringView name) const;
};
```

### Usage Example

```cpp
// Simple C++ functions
i32 my_add(i32 a, i32 b) { return a + b; }
f64 my_sqrt(f64 x) { return std::sqrt(x); }

// Setup
BumpAllocator allocator(8192);
TypeCache types(allocator);
NativeRegistry registry(allocator, types);

// Automatic binding - wrapper generated at compile time
registry.bind<my_add>("add");
registry.bind<my_sqrt>("sqrt");

// Register built-in natives (array_new_int, array_len, print)
register_builtin_natives(registry);

// Use in compilation
SemanticAnalyzer analyzer(allocator, &registry);
IRBuilder ir_builder(allocator, types, &registry);

// Apply to bytecode module for runtime
registry.apply_to_module(module);
```

### NativeTypeKind

For functions needing VM access (like array operations), use `bind_native` with type kinds:

```cpp
enum class NativeTypeKind : u8 {
    Void, Bool,
    I8, I16, I32, I64,
    U8, U16, U32, U64,
    F32, F64,
    ArrayI32,  // i32[]
};

// Example: array_new_int(size: i32) -> i32[]
registry.bind_native("array_new_int", native_array_new_int,
                     {NativeTypeKind::I32}, NativeTypeKind::ArrayI32);
```

---

## Reference Counting Operations

```cpp
inline void ref_inc(void* obj) {
    get_header(obj)->ref_count++;
}

inline void ref_dec(void* obj) {
    ObjectHeader* header = get_header(obj);
    assert(header->ref_count > 0);
    header->ref_count--;
}

inline bool weak_is_valid(void* ptr, uint32_t generation) {
    if (!ptr) return false;
    return get_header(ptr)->weak_generation == generation;
}

void dealloc(void* obj) {
    ObjectHeader* header = get_header(obj);
    
    if (header->ref_count > 0) {
        panic("Cannot delete object with active refs");
    }
    
    header->weak_generation++;  // Invalidate weak refs
    release_field_refs(obj, header->type_id);
    std::free(header);
}
```

---

## SSA IR with Block Arguments

**Status: ✅ Implemented** (SSA IR: `include/roxy/compiler/ssa_ir.hpp`, Lowering: `include/roxy/compiler/lowering.hpp`)

```
entry:
    goto loop(0, 1)              // initial values

loop(sum, i):                    // block parameters
    t0 = i <= n
    if t0 goto body else exit(sum)

body:
    sum2 = sum + i
    i2 = i + 1
    goto loop(sum2, i2)          // pass values to successor

exit(result):
    return result
```

### Lowering to Bytecode

**Status: ✅ Implemented** (`include/roxy/compiler/lowering.hpp`, `src/roxy/compiler/lowering.cpp`)

Block arguments become MOV instructions at jump sites:

```
entry:
    LOAD_INT  R1, 0              // sum = 0
    LOAD_INT  R2, 1              // i = 1

loop:
    LE_I      R3, R2, R0         // t0 = i <= n
    JMP_IF_NOT R3, exit

body:
    ADD_I     R1, R1, R2         // sum = sum + i
    ADD_I     R2, R2, 1          // i = i + 1
    JMP       loop

exit:
    MOV       R0, R1             // result = sum
    RET
```

### BytecodeBuilder

```cpp
class BytecodeBuilder {
public:
    BCModule* build(IRModule* ir_module);
    BCFunction* build_function(IRFunction* ir_func);

private:
    // Register allocation: ValueId -> register number
    u8 allocate_register(ValueId value);
    u8 get_register(ValueId value);

    // Constant pool management
    u16 add_constant(const BCConstant& c);

    // Block lowering
    void lower_block(IRBlock* block);
    void lower_instruction(IRInst* inst);
    void lower_terminator(IRBlock* block);

    // Block argument handling
    void emit_block_args(const JumpTarget& target);

    // Two-pass jump resolution
    void patch_jumps();
};
```

Key lowering decisions:
- **Simple register allocation**: Uses SSA value ID as register number (max 255 values per function)
- **Two-pass emission**: First pass records block offsets, second pass patches jump targets
- **Constant pool**: Values that don't fit in 16-bit immediate go to constant pool
- **Block arguments**: Lowered to MOV instructions before each jump

---

## Frontend Architecture (Compiler + LSP)

### Shared Components

- **Lexer** — Token definitions, lexing rules, trivia handling
- **Token/Syntax kinds** — Single source of truth for grammar
- **Type system** — Type definitions, compatibility rules
- **Semantic rules** — Shared validation logic

### Separate Components

| Compiler | LSP |
|----------|-----|
| Fail-fast parser | Error-recovering parser |
| AST (lossy) | CST (lossless, preserves trivia) |
| Batch processing | Incremental, lazy analysis |
| Arena allocation | NodeID + hash map |

### LSP Parser Approach

```cpp
// Always returns a node, never nullptr
FunctionDeclSyntax* parse_function() {
    Token fun_kw = expect_or_synthetic(TokenKind::KW_fun);
    Token name = expect_or_synthetic(TokenKind::Identifier);
    // ... always produces tree, may contain ErrorNode children
}
```

### LSP Node Storage

```cpp
class SyntaxTree {
    std::unordered_map<NodeId, std::unique_ptr<SyntaxNode>> nodes;
    
    void replace(NodeId id, std::unique_ptr<SyntaxNode> new_node) {
        nodes[id] = std::move(new_node);  // Easy incremental update
    }
};
```

---

## File Structure Suggestion

```
roxy/
├── shared/
│   ├── token_kinds.h         // Token definitions
│   ├── syntax_kinds.h        // AST node types
│   ├── operators.h           // Precedence tables
│   ├── lexer.h/.cpp          // Shared lexer
│   ├── types.h/.cpp          // Type system
│   └── semantic_rules.h      // Shared validation
│
├── compiler/
│   ├── parser.h/.cpp         // Fail-fast parser
│   ├── semantic.h/.cpp       // Type checking
│   ├── ssa_ir.h/.cpp         // SSA with block arguments
│   ├── lowering.h/.cpp       // SSA -> bytecode
│   └── codegen.h/.cpp        // Bytecode emission
│
├── vm/
│   ├── bytecode.h            // Opcode definitions
│   ├── value.h               // Value representation
│   ├── vm.h/.cpp             // VM state, memory management
│   ├── interpreter.cpp       // Main dispatch loop
│   └── natives.h/.cpp        // Built-in functions
│
├── vm/binding/
│   ├── type_traits.hpp       // RoxyType<T> mappings
│   ├── function_traits.hpp   // Compile-time signature extraction
│   ├── binder.hpp            // Automatic wrapper generation
│   ├── registry.hpp          // NativeRegistry class
│   └── interop.hpp           // Convenience header
│
├── lsp/
│   ├── parser.h/.cpp         // Error-recovering parser
│   ├── syntax_tree.h/.cpp    // Lossless CST with NodeIDs
│   ├── semantic_model.h/.cpp // Lazy analysis
│   └── server.h/.cpp         // LSP protocol handling
│
└── tests/
    ├── lexer_test.cpp
    ├── parser_test.cpp
    ├── semantic_test.cpp
    ├── ssa_ir_test.cpp
    ├── bytecode_test.cpp
    ├── vm_test.cpp
    ├── lowering_test.cpp
    ├── e2e_test.cpp
    ├── interop_test.cpp      // C++ interop and function binding
    └── consistency_test.cpp  // Verify compiler/LSP agree (planned)
```

---

## Implementation Status

| Component | Status | Files |
|-----------|--------|-------|
| Lexer | ✅ Done | `include/roxy/shared/lexer.hpp` |
| Compiler Parser | ✅ Done | `include/roxy/compiler/parser.hpp` |
| AST | ✅ Done | `include/roxy/compiler/ast.hpp` |
| Type System | ✅ Done | `include/roxy/compiler/types.hpp` |
| Semantic Analysis | ✅ Done | `include/roxy/compiler/semantic.hpp` |
| SSA IR | ✅ Done | `include/roxy/compiler/ssa_ir.hpp` |
| IR Builder | ✅ Done | `include/roxy/compiler/ir_builder.hpp` |
| Bytecode Format | ✅ Done | `include/roxy/vm/bytecode.hpp` |
| Value/Object | ✅ Done | `include/roxy/vm/value.hpp`, `object.hpp` |
| VM Core | ✅ Done | `include/roxy/vm/vm.hpp` |
| Interpreter | ✅ Done | `include/roxy/vm/interpreter.hpp` |
| SSA Lowering | ✅ Done | `include/roxy/compiler/lowering.hpp` |
| Arrays | ✅ Done | `include/roxy/vm/array.hpp`, `natives.hpp` |
| Native Functions | ✅ Done | `include/roxy/vm/natives.hpp` |
| C++ Interop | ✅ Done | `include/roxy/vm/binding/` |
| Structs | ✅ Done | `types.hpp`, `semantic.cpp`, `ir_builder.cpp`, `lowering.cpp`, `interpreter.cpp` |
| LSP Parser | ⏳ Planned | — |
| LSP Features | ⏳ Planned | — |

## Next Steps

1. ~~**Lexer** — Shared between compiler and LSP~~ ✅
2. ~~**Compiler parser** — Simple, fail-fast~~ ✅
3. ~~**SSA IR** — Block arguments representation~~ ✅
4. ~~**Bytecode lowering** — Register allocation, instruction selection~~ ✅
5. ~~**VM interpreter** — Switch-based dispatch loop~~ ✅
6. ~~**Arrays** — GET_INDEX, SET_INDEX, native functions (array_new_int, array_len)~~ ✅
7. ~~**C++ interop** — Type-safe binding with automatic wrapper generation~~ ✅
8. ~~**Structs** — Stack-allocated value types with field access~~ ✅
9. **Struct extensions** — Literals, pass-by-value, return, nested structs
10. **Heap allocation** — `new`/`uniq`/`ref`/`weak` for structs
11. **LSP parser** — Error recovery, lossless CST
12. **LSP features** — Completion, hover, go-to-definition

---

## Test Coverage

All components have test coverage:

```
tests/
├── lexer_test.cpp      # Token scanning tests
├── parser_test.cpp     # AST construction tests
├── semantic_test.cpp   # Type checking tests
├── ssa_ir_test.cpp     # IR generation tests
├── bytecode_test.cpp   # Instruction encoding tests
├── vm_test.cpp         # VM execution tests
├── lowering_test.cpp   # SSA to bytecode tests
└── e2e_test.cpp        # End-to-end compilation and execution tests
```

Run all tests:
```bash
cd build
./lexer_test && ./parser_test && ./semantic_test && \
./ssa_ir_test && ./bytecode_test && ./vm_test && \
./lowering_test && ./e2e_test && ./interop_test
```