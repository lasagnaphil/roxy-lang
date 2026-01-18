# Virtual Machine

The Roxy VM is a register-based virtual machine with a shared register file and call frame stack.

## VM State

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
```

## VM API

```cpp
bool vm_init(RoxyVM* vm, const VMConfig& config = VMConfig());
void vm_destroy(RoxyVM* vm);
bool vm_load_module(RoxyVM* vm, BCModule* module);
bool vm_call(RoxyVM* vm, StringView func_name, Span<Value> args);
Value vm_get_result(RoxyVM* vm);
```

## Value Representation

The `Value` struct is used for the public API and native function interface:

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

## Interpreter Loop

The interpreter uses a switch-based dispatch loop for portability (works with MSVC/clang-cl on Windows):

```cpp
bool interpret(RoxyVM* vm) {
    CallFrame* frame = &vm->call_stack.back();
    const BCFunction* func = frame->func;
    const u32* pc = frame->pc;
    u64* regs = frame->registers;

    while (vm->running) {
        u32 instr = *pc++;
        Opcode op = decode_opcode(instr);
        u8 a = decode_a(instr);
        u8 b = decode_b(instr);
        u8 c = decode_c(instr);

        switch (op) {
            case Opcode::LOAD_NULL:
                regs[a] = 0;
                break;

            case Opcode::ADD_I:
                regs[a] = regs[b] + regs[c];
                break;

            case Opcode::JMP:
                pc += decode_offset(instr);
                break;

            case Opcode::JMP_IF:
                if (regs[a] != 0) {
                    pc += decode_offset(instr);
                }
                break;

            case Opcode::CALL: {
                // Save current PC, allocate callee registers
                frame->pc = pc;
                u64* callee_regs = &vm->register_file[vm->register_top];
                vm->register_top += callee->register_count;

                // Allocate local stack (aligned to 4 slots)
                u32 local_base = (vm->local_stack_top + 3) & ~3u;
                vm->local_stack_top = local_base + callee->local_stack_slots;

                // Copy arguments, push new frame
                for (u8 i = 0; i < arg_count; i++) {
                    callee_regs[i] = regs[first_arg + i];
                }
                vm->call_stack.push_back(CallFrame{
                    callee, callee->code.data(), callee_regs, dst, local_base
                });

                // Update cached frame pointers
                frame = &vm->call_stack.back();
                func = frame->func;
                pc = frame->pc;
                regs = frame->registers;
                break;
            }

            case Opcode::RET: {
                u64 result = regs[a];
                vm->local_stack_top = frame->local_stack_base;  // Pop local stack
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
- Error messages are stored in `vm->error`

## Files

- `include/roxy/vm/vm.hpp` - VM state and API declarations
- `src/roxy/vm/vm.cpp` - VM initialization and execution
- `include/roxy/vm/value.hpp` - Value representation
- `src/roxy/vm/value.cpp` - Value operations
- `include/roxy/vm/interpreter.hpp` - Interpreter declarations
- `src/roxy/vm/interpreter.cpp` - Interpreter loop implementation
