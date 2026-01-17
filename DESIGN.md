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

```cpp
struct ObjectHeader {
    uint32_t ref_count;        // Number of active 'ref' pointers
    uint32_t weak_generation;  // Bumped on delete to invalidate weak refs
    uint32_t type_id;
    uint32_t size;
};
```

---

## Bytecode Format

### Instruction Encoding

```
Format A: [opcode:8][dst:8][src1:8][src2:8]    — 3-operand
Format B: [opcode:8][dst:8][imm16:16]          — immediate
Format C: [opcode:8][reg:8][offset:16]         — branch/field access
```

### Calling Convention

```
Arguments:  R0, R1, R2, R3, ... (left to right)
Return:     R0
```

### Core Opcode Categories

| Range | Category |
|-------|----------|
| 0x00-0x0F | Constants, Moves |
| 0x10-0x1F | Integer Arithmetic |
| 0x20-0x2F | Float Arithmetic |
| 0x30-0x3F | Bitwise |
| 0x40-0x5F | Comparisons |
| 0x60-0x7F | Logical |
| 0x80-0x8F | Type Conversions |
| 0x90-0x9F | Control Flow |
| 0xA0-0xAF | Function Calls |
| 0xB0-0xCF | Field Access |
| 0xD0-0xDF | Object Lifecycle |
| 0xE0-0xEF | Reference Counting |
| 0xF0-0xF7 | Containers |
| 0xF8-0xFF | Debug/Special |

---

## Core Data Structures

### Value Representation

```cpp
struct Value {
    enum class Type : uint8_t { 
        Null, Bool, Int, Float, Ptr, Weak 
    };
    
    Type type;
    union {
        bool as_bool;
        int64_t as_int;
        double as_float;
        void* as_ptr;
        struct { void* ptr; uint32_t generation; } as_weak;
    };
};
```

### VM State

```cpp
struct RoxyVM {
    Module* module;
    
    Value* register_file;          // Shared register window
    uint32_t register_file_size;
    uint32_t register_top;
    
    std::vector<CallFrame> call_stack;
    
    bool running;
    const char* error;
};

struct CallFrame {
    const Function* func;
    uint32_t* pc;
    Value* registers;              // Window into register_file
    uint32_t return_reg;
};
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

## Interpreter Loop (Computed Goto)

```cpp
ExecResult execute(RoxyVM& vm) {
    static const void* dispatch[] = {
        [OP_LOAD_NULL] = &&op_load_null,
        [OP_ADD_I]     = &&op_add_i,
        // ... all opcodes
    };
    
    CallFrame* frame = &vm.call_stack.back();
    uint32_t* pc = frame->pc;
    Value* regs = frame->registers;
    uint32_t inst;
    
    #define DISPATCH() do { inst = *pc++; goto *dispatch[OPCODE(inst)]; } while(0)
    
    DISPATCH();
    
op_load_null:
    regs[DST(inst)] = Value::null_val();
    DISPATCH();

op_add_i:
    regs[DST(inst)] = Value::from_int(
        regs[SRC1(inst)].as_int + regs[SRC2(inst)].as_int
    );
    DISPATCH();

op_call: {
    uint16_t func_idx = IMM16(inst);
    const Function& callee = vm.module->functions[func_idx];
    
    frame->pc = pc;
    
    Value* callee_regs = vm.register_file + vm.register_top;
    vm.register_top += callee.register_count;
    
    for (int i = 0; i < callee.param_count; i++) {
        callee_regs[i] = regs[i];
    }
    
    vm.call_stack.push_back({&callee, callee.code, callee_regs, 0});
    
    frame = &vm.call_stack.back();
    pc = frame->pc;
    regs = frame->registers;
    DISPATCH();
}

op_ret: {
    Value result = regs[0];
    vm.register_top -= frame->func->register_count;
    vm.call_stack.pop_back();
    
    if (vm.call_stack.empty()) {
        vm.register_file[0] = result;
        return ExecResult::Ok;
    }
    
    frame = &vm.call_stack.back();
    pc = frame->pc;
    regs = frame->registers;
    regs[0] = result;
    DISPATCH();
}

// ... remaining opcodes
}
```

---

## C++ Interop

### Type Registration (Builder Pattern)

```cpp
vm.register_type(
    type<Vec3>("Vec3")
        .field("x", &Vec3::x)
        .field("y", &Vec3::y)
        .field("z", &Vec3::z)
        .method("length", ROXY_METHOD(&Vec3::length))
        .method("normalize", ROXY_METHOD(&Vec3::normalize))
        .build()
);
```

### Native Function Binding

```cpp
// Automatic wrapper via templates
#define ROXY_BIND(fn) decltype(make_binder(fn))::invoke<fn>

void register_math(RoxyVM& vm) {
    vm.register_native("vec3_add", ROXY_BIND(vec3_add), 6, 3);
    vm.register_native("sin", ROXY_BIND(std::sin), 1, 1);
}
```

### Method Binding

```cpp
#define ROXY_METHOD(method) \
    decltype(method_binder(method))::invoke<method>

vm.register_type(
    type<Entity>("Entity")
        .method("take_damage", ROXY_METHOD(&Entity::take_damage))
        .method("is_alive", ROXY_METHOD(&Entity::is_alive))
        .build()
);
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

Block arguments become MOV instructions:

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
├── interop/
│   ├── type_builder.h        // C++ type registration
│   ├── function_binder.h     // Native function binding
│   └── method_binder.h       // Method binding templates
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
    ├── vm_test.cpp
    ├── interop_test.cpp
    └── consistency_test.cpp  // Verify compiler/LSP agree
```

---

## Next Steps

1. **Lexer** — Shared between compiler and LSP
2. **Compiler parser** — Simple, fail-fast
3. **SSA IR** — Block arguments representation
4. **Bytecode lowering** — Register allocation, instruction selection
5. **VM interpreter** — Computed goto dispatch loop
6. **C++ interop** — Type registration, native binding
7. **LSP parser** — Error recovery, lossless CST
8. **LSP features** — Completion, hover, go-to-definition

---

This should give you a solid foundation to start implementing. Want me to create any specific files to get you started?