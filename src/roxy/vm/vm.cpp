#include "roxy/vm/vm.hpp"
#include "roxy/vm/interpreter.hpp"
#include "roxy/vm/object.hpp"
#include "roxy/rt/slab_allocator.hpp"
#include "roxy/vm/string.hpp"
#include "roxy/vm/string_intern.hpp"

#include <cstdlib>
#include <cstring>
#include <new>

namespace rx {

// RoxyVM constructor/destructor defined here where SlabAllocator is complete
RoxyVM::RoxyVM()
    : module(nullptr)
    , register_file_size(0)
    , register_top(0)
    , local_stack_size(0)
    , local_stack_top(0)
    , call_stack_size(0)
    , call_stack_capacity(0)
    , function_ptrs(nullptr)
    , function_count(0)
    , running(false)
    , error(nullptr)
    , in_flight_exception(nullptr)
    , in_flight_exception_type_id(0)
    , in_flight_message_fn_idx(UINT32_MAX)
{}

RoxyVM::~RoxyVM() {
    // Clean up function pointer cache
    delete[] function_ptrs;
    function_ptrs = nullptr;

    // Drop the intern table before the slab allocator shuts down (its stored
    // StringView keys point into slab-backed string data that's about to go
    // away). Explicit reset — UniquePtr's declaration order would still put
    // `string_intern` after `allocator` for destruction.
    string_intern.reset();

    // UniquePtr members are automatically cleaned up
    // But we need to call shutdown() on allocator first if it exists
    if (allocator) {
        allocator->shutdown();
    }
}

bool vm_init(RoxyVM* vm, const VMConfig& config) {
    init_type_registry();

    roxy_ctx_init(&vm->ctx);

    vm->module = nullptr;
    vm->running = false;
    vm->error = nullptr;

    // Initialize register file (untyped 8-byte slots)
    vm->register_file_size = config.register_file_size;
    vm->register_file = UniquePtr<u64[]>(new (std::nothrow) u64[config.register_file_size]);
    if (!vm->register_file) {
        return false;
    }
    vm->register_top = 0;

    // Initialize all registers to zero
    memset(vm->register_file.get(), 0, config.register_file_size * sizeof(u64));

    // Initialize local stack (4-byte slots for struct data)
    vm->local_stack_size = config.local_stack_size;
    vm->local_stack = UniquePtr<u32[]>(new (std::nothrow) u32[config.local_stack_size]);
    if (!vm->local_stack) {
        vm->register_file.reset();
        return false;
    }
    vm->local_stack_top = 0;

    // Initialize slab allocator for heap objects
    vm->allocator = UniquePtr<SlabAllocator>(new (std::nothrow) SlabAllocator());
    if (!vm->allocator) {
        vm->register_file.reset();
        vm->local_stack.reset();
        return false;
    }
    if (!vm->allocator->init()) {
        vm->allocator.reset();
        vm->register_file.reset();
        vm->local_stack.reset();
        return false;
    }

    // Initialize the string intern table. Used by string_alloc to dedup
    // heap strings — same content = same StringObject pointer.
    vm->string_intern = UniquePtr<StringInternTable>(new (std::nothrow) StringInternTable());
    if (!vm->string_intern) {
        vm->allocator.reset();
        vm->register_file.reset();
        vm->local_stack.reset();
        return false;
    }

    // Pre-allocate fixed-size call stack
    vm->call_stack_capacity = config.max_call_depth;
    vm->call_stack = UniquePtr<CallFrame[]>(new (std::nothrow) CallFrame[config.max_call_depth]);
    if (!vm->call_stack) {
        vm->allocator.reset();
        vm->register_file.reset();
        vm->local_stack.reset();
        return false;
    }
    vm->call_stack_size = 0;

    vm->function_ptrs = nullptr;
    vm->function_count = 0;

    return true;
}

void vm_destroy(RoxyVM* vm) {
#if ROXY_PROFILE_BYTECODE
    bc_profile_dump(stderr);
#endif

    vm->register_file.reset();
    vm->register_file_size = 0;
    vm->register_top = 0;

    vm->local_stack.reset();
    vm->local_stack_size = 0;
    vm->local_stack_top = 0;

    // Drop the intern table before shutting down the slab allocator, since
    // the string pointers the table holds become invalid once slabs are freed.
    vm->string_intern.reset();

    // Destroy slab allocator
    if (vm->allocator) {
        vm->allocator->shutdown();
        vm->allocator.reset();
    }

    vm->call_stack.reset();
    vm->call_stack_size = 0;
    vm->call_stack_capacity = 0;

    delete[] vm->function_ptrs;
    vm->function_ptrs = nullptr;
    vm->function_count = 0;

    vm->module = nullptr;
    vm->running = false;
    vm->error = nullptr;

    roxy_ctx_destroy(&vm->ctx);
}

bool vm_load_module(RoxyVM* vm, BCModule* module) {
    vm->module = module;

    // Register types from module for heap allocation
    // Store the global type IDs so NEW_OBJ can find them
    module->type_ids.clear();
    for (const BCTypeInfo& type_info : module->types) {
        u32 type_id = register_object_type(type_info.name.data(), type_info.size_bytes, nullptr);
        module->type_ids.push_back(type_id);
    }

    // Build flat function pointer cache
    delete[] vm->function_ptrs;
    vm->function_count = static_cast<u32>(module->functions.size());
    vm->function_ptrs = new (std::nothrow) const BCFunction*[vm->function_count];
    if (!vm->function_ptrs) {
        vm->function_count = 0;
        return false;
    }
    for (u32 i = 0; i < vm->function_count; i++) {
        vm->function_ptrs[i] = module->functions[i].get();
    }

    // Pre-intern every string constant once at load time. Each BCConstant
    // caches the resulting StringObject* so the LOAD_CONST opcode can return
    // it directly — no per-execution hash, no per-execution probe. The
    // interning call also populates the VM's intern table, so runtime-created
    // strings with the same content will dedup to the same object.
    for (u32 fi = 0; fi < vm->function_count; fi++) {
        BCFunction* func = module->functions[fi].get();
        for (u32 ci = 0; ci < func->constants.size(); ci++) {
            BCConstant& c = func->constants[ci];
            if (c.type == BCConstant::String && c.as_string.obj == nullptr) {
                c.as_string.obj = string_alloc(vm, c.as_string.data, c.as_string.length);
            }
        }
    }

    return true;
}

bool vm_call(RoxyVM* vm, StringView func_name, Span<Value> args) {
    if (vm->module == nullptr) {
        vm->error = "No module loaded";
        return false;
    }

    i32 func_index = vm->module->find_function(func_name);
    if (func_index < 0) {
        vm->error = "Function not found";
        return false;
    }

    return vm_call_index(vm, static_cast<u32>(func_index), args);
}

bool vm_call_index(RoxyVM* vm, u32 func_index, Span<Value> args) {
    if (vm->module == nullptr) {
        vm->error = "No module loaded";
        return false;
    }

    if (func_index >= vm->module->functions.size()) {
        vm->error = "Invalid function index";
        return false;
    }

    const BCFunction* func = vm->module->functions[func_index].get();

    // Check argument count
    if (args.size() != func->param_count) {
        vm->error = "Wrong number of arguments";
        return false;
    }

    // Check register space
    if (vm->register_top + func->register_count > vm->register_file_size) {
        vm->error = "Register file overflow";
        return false;
    }

    // Allocate registers for this call
    u64* registers = &vm->register_file[vm->register_top];
    vm->register_top += func->register_count;

    // Clear registers (debug only — SSA guarantees write-before-read)
#ifndef NDEBUG
    memset(registers, 0, func->register_count * sizeof(u64));
#endif

    // Copy arguments to registers R0, R1, ...
    for (u32 i = 0; i < args.size(); i++) {
        registers[i] = args[i].as_u64();
    }

    // Allocate local stack space for this function (16-byte aligned)
    u32 local_stack_base = (vm->local_stack_top + 3) & ~3u;  // Align to 4 slots (16 bytes)
    if (local_stack_base + func->local_stack_slots > vm->local_stack_size) {
        vm->error = "Local stack overflow";
        return false;
    }
    vm->local_stack_top = local_stack_base + func->local_stack_slots;

    // Push call frame
    // For top-level call, return_reg is 0 (result goes to R0 of this frame)
    vm->call_stack[vm->call_stack_size++] = CallFrame(func, func->code.data(), registers, 0, local_stack_base);

    // Activate this VM's context for the duration of the call so native
    // functions and runtime helpers can fetch it via `roxy_get_ctx()`. The
    // RAII guard restores the previous (typically null) context on return.
    roxy::ScopedContext ctx_guard(&vm->ctx);

    // Execute
    vm->running = true;
    bool success = interpret(vm);
    vm->running = false;

    // If we still have a frame on the stack, it means we returned normally
    // The result should be in registers[0]

    return success;
}

Value vm_get_result(RoxyVM* vm) {
    // Result is in the first register after all frames have been popped
    if (vm->register_file && vm->register_file_size > 0) {
        return Value::from_u64(vm->register_file[0]);
    }
    return Value::make_null();
}

const char* vm_get_error(RoxyVM* vm) {
    return vm->error;
}

void vm_clear_error(RoxyVM* vm) {
    vm->error = nullptr;
}

void vm_register_native(RoxyVM* vm, StringView name, NativeFunction func, u32 param_count) {
    if (vm->module == nullptr) {
        return;
    }

    BCNativeFunction native;
    native.name = name;
    native.func = func;
    native.param_count = param_count;
    vm->module->native_functions.push_back(native);
}

}
