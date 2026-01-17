#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/vector.hpp"
#include "roxy/vm/bytecode.hpp"
#include "roxy/vm/value.hpp"

namespace rx {

// Call frame - represents an active function call
struct CallFrame {
    const BCFunction* func;     // Current function
    const u32* pc;              // Program counter (pointer into code)
    Value* registers;           // Register window base
    u8 return_reg;              // Register to store return value in caller

    CallFrame()
        : func(nullptr), pc(nullptr), registers(nullptr), return_reg(0) {}

    CallFrame(const BCFunction* f, const u32* p, Value* r, u8 ret)
        : func(f), pc(p), registers(r), return_reg(ret) {}
};

// VM configuration
struct VMConfig {
    u32 register_file_size;     // Maximum number of registers
    u32 max_call_depth;         // Maximum call stack depth

    VMConfig()
        : register_file_size(65536)
        , max_call_depth(1024)
    {}
};

// Roxy Virtual Machine
struct RoxyVM {
    BCModule* module;               // Loaded module
    Value* register_file;           // Register file
    u32 register_file_size;         // Total register capacity
    u32 register_top;               // Current top of register allocation
    Vector<CallFrame> call_stack;   // Call stack
    bool running;                   // Execution state
    const char* error;              // Error message (null if no error)

    RoxyVM()
        : module(nullptr)
        , register_file(nullptr)
        , register_file_size(0)
        , register_top(0)
        , running(false)
        , error(nullptr)
    {}
};

// Initialize VM with configuration
bool vm_init(RoxyVM* vm, const VMConfig& config = VMConfig());

// Destroy VM and free resources
void vm_destroy(RoxyVM* vm);

// Load a module into the VM
bool vm_load_module(RoxyVM* vm, BCModule* module);

// Call a function by name
// Result is stored in the first register (R0) after return
bool vm_call(RoxyVM* vm, StringView func_name, Span<Value> args);

// Call a function by index
bool vm_call_index(RoxyVM* vm, u32 func_index, Span<Value> args);

// Get the result of the last call (value in R0)
Value vm_get_result(RoxyVM* vm);

// Get error message (or nullptr if no error)
const char* vm_get_error(RoxyVM* vm);

// Clear error state
void vm_clear_error(RoxyVM* vm);

// Register a native function
void vm_register_native(RoxyVM* vm, StringView name, NativeFunction func, u32 param_count);

}
