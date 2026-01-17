#include "roxy/vm/vm.hpp"
#include "roxy/vm/interpreter.hpp"

#include <cstdlib>
#include <cstring>

namespace rx {

bool vm_init(RoxyVM* vm, const VMConfig& config) {
    vm->module = nullptr;
    vm->register_file_size = config.register_file_size;
    vm->register_file = new Value[config.register_file_size];
    vm->register_top = 0;
    vm->call_stack.reserve(config.max_call_depth);
    vm->running = false;
    vm->error = nullptr;

    // Initialize all registers to null
    for (u32 i = 0; i < config.register_file_size; i++) {
        vm->register_file[i] = Value::make_null();
    }

    return true;
}

void vm_destroy(RoxyVM* vm) {
    if (vm->register_file) {
        delete[] vm->register_file;
        vm->register_file = nullptr;
    }
    vm->register_file_size = 0;
    vm->register_top = 0;
    vm->call_stack.clear();
    vm->module = nullptr;
    vm->running = false;
    vm->error = nullptr;
}

bool vm_load_module(RoxyVM* vm, BCModule* module) {
    vm->module = module;
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

    const BCFunction* func = vm->module->functions[func_index];

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
    Value* registers = &vm->register_file[vm->register_top];
    vm->register_top += func->register_count;

    // Clear registers
    for (u32 i = 0; i < func->register_count; i++) {
        registers[i] = Value::make_null();
    }

    // Copy arguments to registers R0, R1, ...
    for (u32 i = 0; i < args.size(); i++) {
        registers[i] = args[i];
    }

    // Push call frame
    // For top-level call, return_reg is 0 (result goes to R0 of this frame)
    CallFrame frame(func, func->code.data(), registers, 0);
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
        return vm->register_file[0];
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
