#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/vector.hpp"
#include "roxy/core/unique_ptr.hpp"
#include "roxy/vm/bytecode.hpp"
#include "roxy/vm/value.hpp"

namespace rx {

// Forward declaration
struct SlabAllocator;

// Call frame - represents an active function call
struct CallFrame {
    const BCFunction* func;     // Current function
    const u32* pc;              // Program counter (pointer into code)
    u64* registers;             // Register window base (untyped 8-byte slots)
    u8 return_reg;              // Register to store return value in caller
    u32 local_stack_base;       // Base slot index in local_stack for this frame

    CallFrame()
        : func(nullptr), pc(nullptr), registers(nullptr), return_reg(0), local_stack_base(0) {}

    CallFrame(const BCFunction* f, const u32* p, u64* r, u8 ret, u32 stack_base)
        : func(f), pc(p), registers(r), return_reg(ret), local_stack_base(stack_base) {}
};

// VM configuration
struct VMConfig {
    u32 register_file_size;     // Maximum number of registers (8-byte slots)
    u32 local_stack_size;       // Maximum local stack size (4-byte slots)
    u32 max_call_depth;         // Maximum call stack depth

    VMConfig()
        : register_file_size(65536)
        , local_stack_size(262144)  // 256K slots = 1MB
        , max_call_depth(1024)
    {}
};

// Roxy Virtual Machine
struct RoxyVM {
    BCModule* module;               // Loaded module
    UniquePtr<u64[]> register_file; // Register file (untyped 8-byte slots)
    u32 register_file_size;         // Total register capacity
    u32 register_top;               // Current top of register allocation

    UniquePtr<u32[]> local_stack;   // Local stack for struct data (4-byte slots)
    u32 local_stack_size;           // Total local stack capacity in slots
    u32 local_stack_top;            // Current top of local stack allocation

    UniquePtr<SlabAllocator> allocator;  // Slab allocator for heap objects

    Vector<CallFrame> call_stack;   // Call stack
    bool running;                   // Execution state
    const char* error;              // Error message (null if no error)

    // Exception handling state
    void* in_flight_exception;          // Exception object being propagated (nullptr if none)
    u32 in_flight_exception_type_id;    // type_id from ObjectHeader
    u32 in_flight_message_fn_idx;       // Function index for message() method (UINT32_MAX = none)

    // Constructor and destructor declared here, defined in vm.cpp
    // (destructor must be out-of-line for UniquePtr<SlabAllocator> with forward-declared type)
    RoxyVM();
    ~RoxyVM();
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
