#include "roxy/vm/vm.hpp"
#include "roxy/vm/interpreter.hpp"
#include "roxy/vm/object.hpp"
#include "roxy/vm/slab_allocator.hpp"

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
    , running(false)
    , error(nullptr)
    , in_flight_exception(nullptr)
    , in_flight_exception_type_id(0)
    , in_flight_message_fn_idx(UINT32_MAX)
{}

RoxyVM::~RoxyVM() {
    // UniquePtr members are automatically cleaned up
    // But we need to call shutdown() on allocator first if it exists
    if (allocator) {
        allocator->shutdown();
    }
}

bool vm_init(RoxyVM* vm, const VMConfig& config) {
    init_type_registry();

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
    for (u32 i = 0; i < config.register_file_size; i++) {
        vm->register_file[i] = 0;
    }

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

    vm->call_stack.reserve(config.max_call_depth);

    return true;
}

void vm_destroy(RoxyVM* vm) {
    vm->register_file.reset();
    vm->register_file_size = 0;
    vm->register_top = 0;

    vm->local_stack.reset();
    vm->local_stack_size = 0;
    vm->local_stack_top = 0;

    // Destroy slab allocator
    if (vm->allocator) {
        vm->allocator->shutdown();
        vm->allocator.reset();
    }

    vm->call_stack.clear();
    vm->module = nullptr;
    vm->running = false;
    vm->error = nullptr;
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

    // Clear registers
    for (u32 i = 0; i < func->register_count; i++) {
        registers[i] = 0;
    }

    // Copy arguments to registers R0, R1, ...
    // Convert Value to u64 (store raw bits)
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
    CallFrame frame(func, func->code.data(), registers, 0, local_stack_base);
    vm->call_stack.push_back(frame);

    // Execute
    vm->running = true;
    bool success = interpret(vm);

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
