#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "roxy/core/doctest/doctest.h"

#include "roxy/vm/vm.hpp"
#include "roxy/vm/interpreter.hpp"
#include "roxy/vm/bytecode.hpp"
#include "roxy/vm/value.hpp"

using namespace rx;

// Helper to create a simple function that returns a constant integer
BCFunction* create_return_int_func(const char* name, i64 value) {
    BCFunction* func = new BCFunction();
    func->name = StringView(name);
    func->param_count = 0;
    func->register_count = 1;

    if (value >= -32768 && value <= 32767) {
        func->code.push_back(encode_abi(Opcode::LOAD_INT, 0, static_cast<u16>(static_cast<i16>(value))));
    } else {
        func->constants.push_back(BCConstant::make_int(value));
        func->code.push_back(encode_abi(Opcode::LOAD_CONST, 0, 0));
    }
    func->code.push_back(encode_abc(Opcode::RET, 0, 0, 0));

    return func;
}

// Helper to create a function that adds two parameters
BCFunction* create_add_func(const char* name) {
    BCFunction* func = new BCFunction();
    func->name = StringView(name);
    func->param_count = 2;
    func->register_count = 3;

    // R0 = param 1, R1 = param 2
    // R2 = R0 + R1
    func->code.push_back(encode_abc(Opcode::ADD_I, 2, 0, 1));
    func->code.push_back(encode_abc(Opcode::RET, 2, 0, 0));

    return func;
}

TEST_CASE("VM initialization and destruction") {
    RoxyVM vm;

    SUBCASE("Default config") {
        CHECK(vm_init(&vm));
        CHECK(vm.register_file != nullptr);
        CHECK(vm.register_file_size > 0);
        CHECK(vm.running == false);
        CHECK(vm.error == nullptr);
        vm_destroy(&vm);
    }

    SUBCASE("Custom config") {
        VMConfig config;
        config.register_file_size = 1024;
        config.max_call_depth = 64;

        CHECK(vm_init(&vm, config));
        CHECK(vm.register_file_size == 1024);
        vm_destroy(&vm);
    }
}

TEST_CASE("Value operations") {
    SUBCASE("Create values") {
        Value null_val = Value::make_null();
        CHECK(null_val.is_null());
        CHECK(!null_val.is_truthy());

        Value true_val = Value::make_bool(true);
        CHECK(true_val.is_bool());
        CHECK(true_val.as_bool == true);
        CHECK(true_val.is_truthy());

        Value false_val = Value::make_bool(false);
        CHECK(false_val.is_bool());
        CHECK(false_val.as_bool == false);
        CHECK(!false_val.is_truthy());

        Value int_val = Value::make_int(42);
        CHECK(int_val.is_int());
        CHECK(int_val.as_int == 42);
        CHECK(int_val.is_truthy());

        Value zero_val = Value::make_int(0);
        CHECK(!zero_val.is_truthy());

        Value float_val = Value::make_float(3.14);
        CHECK(float_val.is_float());
        CHECK(float_val.as_float == doctest::Approx(3.14));
    }

    SUBCASE("Value equality") {
        Value a = Value::make_int(42);
        Value b = Value::make_int(42);
        Value c = Value::make_int(0);

        CHECK(a == b);
        CHECK(a != c);
    }
}

TEST_CASE("Execute return literal") {
    RoxyVM vm;
    vm_init(&vm);

    BCModule* module = new BCModule();
    module->name = StringView("test");
    module->functions.push_back(create_return_int_func("main", 42));

    vm_load_module(&vm, module);

    SUBCASE("Return small integer") {
        CHECK(vm_call(&vm, StringView("main"), {}));
        Value result = vm_get_result(&vm);
        CHECK(result.is_int());
        CHECK(result.as_int == 42);
    }

    vm_destroy(&vm);
    delete module;
}

TEST_CASE("Execute return large integer from constant pool") {
    RoxyVM vm;
    vm_init(&vm);

    BCModule* module = new BCModule();
    module->name = StringView("test");
    module->functions.push_back(create_return_int_func("main", 1000000));

    vm_load_module(&vm, module);

    CHECK(vm_call(&vm, StringView("main"), {}));
    Value result = vm_get_result(&vm);
    CHECK(result.is_int());
    CHECK(result.as_int == 1000000);

    vm_destroy(&vm);
    delete module;
}

TEST_CASE("Execute arithmetic operations") {
    RoxyVM vm;
    vm_init(&vm);

    BCModule* module = new BCModule();
    module->name = StringView("test");

    SUBCASE("Addition") {
        module->functions.push_back(create_add_func("add"));
        vm_load_module(&vm, module);

        Value args[2] = {Value::make_int(10), Value::make_int(32)};
        CHECK(vm_call(&vm, StringView("add"), Span<Value>(args, 2)));
        Value result = vm_get_result(&vm);
        CHECK(result.as_int == 42);
    }

    SUBCASE("Subtraction") {
        BCFunction* func = new BCFunction();
        func->name = StringView("sub");
        func->param_count = 2;
        func->register_count = 3;
        func->code.push_back(encode_abc(Opcode::SUB_I, 2, 0, 1));
        func->code.push_back(encode_abc(Opcode::RET, 2, 0, 0));
        module->functions.push_back(func);

        vm_load_module(&vm, module);

        Value args[2] = {Value::make_int(50), Value::make_int(8)};
        CHECK(vm_call(&vm, StringView("sub"), Span<Value>(args, 2)));
        Value result = vm_get_result(&vm);
        CHECK(result.as_int == 42);
    }

    SUBCASE("Multiplication") {
        BCFunction* func = new BCFunction();
        func->name = StringView("mul");
        func->param_count = 2;
        func->register_count = 3;
        func->code.push_back(encode_abc(Opcode::MUL_I, 2, 0, 1));
        func->code.push_back(encode_abc(Opcode::RET, 2, 0, 0));
        module->functions.push_back(func);

        vm_load_module(&vm, module);

        Value args[2] = {Value::make_int(6), Value::make_int(7)};
        CHECK(vm_call(&vm, StringView("mul"), Span<Value>(args, 2)));
        Value result = vm_get_result(&vm);
        CHECK(result.as_int == 42);
    }

    SUBCASE("Division") {
        BCFunction* func = new BCFunction();
        func->name = StringView("div");
        func->param_count = 2;
        func->register_count = 3;
        func->code.push_back(encode_abc(Opcode::DIV_I, 2, 0, 1));
        func->code.push_back(encode_abc(Opcode::RET, 2, 0, 0));
        module->functions.push_back(func);

        vm_load_module(&vm, module);

        Value args[2] = {Value::make_int(84), Value::make_int(2)};
        CHECK(vm_call(&vm, StringView("div"), Span<Value>(args, 2)));
        Value result = vm_get_result(&vm);
        CHECK(result.as_int == 42);
    }

    vm_destroy(&vm);
    delete module;
}

TEST_CASE("Execute float arithmetic") {
    RoxyVM vm;
    vm_init(&vm);

    BCModule* module = new BCModule();
    module->name = StringView("test");

    BCFunction* func = new BCFunction();
    func->name = StringView("addf");
    func->param_count = 2;
    func->register_count = 3;
    func->code.push_back(encode_abc(Opcode::ADD_F, 2, 0, 1));
    func->code.push_back(encode_abc(Opcode::RET, 2, 0, 0));
    module->functions.push_back(func);

    vm_load_module(&vm, module);

    Value args[2] = {Value::make_float(3.14), Value::make_float(2.86)};
    CHECK(vm_call(&vm, StringView("addf"), Span<Value>(args, 2)));
    Value result = vm_get_result(&vm);
    CHECK(result.is_float());
    CHECK(result.as_float == doctest::Approx(6.0));

    vm_destroy(&vm);
    delete module;
}

TEST_CASE("Execute comparison operations") {
    RoxyVM vm;
    vm_init(&vm);

    BCModule* module = new BCModule();
    module->name = StringView("test");

    SUBCASE("Less than (true)") {
        BCFunction* func = new BCFunction();
        func->name = StringView("lt");
        func->param_count = 2;
        func->register_count = 3;
        func->code.push_back(encode_abc(Opcode::LT_I, 2, 0, 1));
        func->code.push_back(encode_abc(Opcode::RET, 2, 0, 0));
        module->functions.push_back(func);

        vm_load_module(&vm, module);

        Value args[2] = {Value::make_int(5), Value::make_int(10)};
        CHECK(vm_call(&vm, StringView("lt"), Span<Value>(args, 2)));
        Value result = vm_get_result(&vm);
        CHECK(result.is_bool());
        CHECK(result.as_bool == true);
    }

    SUBCASE("Less than (false)") {
        module->functions.clear();
        BCFunction* func = new BCFunction();
        func->name = StringView("lt");
        func->param_count = 2;
        func->register_count = 3;
        func->code.push_back(encode_abc(Opcode::LT_I, 2, 0, 1));
        func->code.push_back(encode_abc(Opcode::RET, 2, 0, 0));
        module->functions.push_back(func);

        vm_load_module(&vm, module);

        Value args[2] = {Value::make_int(10), Value::make_int(5)};
        CHECK(vm_call(&vm, StringView("lt"), Span<Value>(args, 2)));
        Value result = vm_get_result(&vm);
        CHECK(result.is_bool());
        CHECK(result.as_bool == false);
    }

    vm_destroy(&vm);
    delete module;
}

TEST_CASE("Execute control flow") {
    RoxyVM vm;
    vm_init(&vm);

    BCModule* module = new BCModule();
    module->name = StringView("test");

    SUBCASE("Conditional jump (condition true)") {
        // if (R0) return 1 else return 0
        BCFunction* func = new BCFunction();
        func->name = StringView("cond");
        func->param_count = 1;
        func->register_count = 2;

        // Bytecode layout:
        // 0: JMP_IF R0, +2 (if true, jump to instruction 3)
        // 1: LOAD_INT R1, 0 (else clause)
        // 2: JMP +1 (skip then clause, jump to instruction 4)
        // 3: LOAD_INT R1, 1 (then clause)
        // 4: RET R1
        func->code.push_back(encode_aoff(Opcode::JMP_IF, 0, 2));
        func->code.push_back(encode_abi(Opcode::LOAD_INT, 1, 0));
        func->code.push_back(encode_aoff(Opcode::JMP, 0, 1));
        func->code.push_back(encode_abi(Opcode::LOAD_INT, 1, 1));
        func->code.push_back(encode_abc(Opcode::RET, 1, 0, 0));

        module->functions.push_back(func);
        vm_load_module(&vm, module);

        Value args[1] = {Value::make_bool(true)};
        CHECK(vm_call(&vm, StringView("cond"), Span<Value>(args, 1)));
        Value result = vm_get_result(&vm);
        CHECK(result.as_int == 1);
    }

    SUBCASE("Conditional jump (condition false)") {
        module->functions.clear();
        // Same function as above
        BCFunction* func = new BCFunction();
        func->name = StringView("cond");
        func->param_count = 1;
        func->register_count = 2;

        // Same layout as condition true case
        func->code.push_back(encode_aoff(Opcode::JMP_IF, 0, 2));
        func->code.push_back(encode_abi(Opcode::LOAD_INT, 1, 0));
        func->code.push_back(encode_aoff(Opcode::JMP, 0, 1));
        func->code.push_back(encode_abi(Opcode::LOAD_INT, 1, 1));
        func->code.push_back(encode_abc(Opcode::RET, 1, 0, 0));

        module->functions.push_back(func);
        vm_load_module(&vm, module);

        Value args[1] = {Value::make_bool(false)};
        CHECK(vm_call(&vm, StringView("cond"), Span<Value>(args, 1)));
        Value result = vm_get_result(&vm);
        CHECK(result.as_int == 0);
    }

    vm_destroy(&vm);
    delete module;
}

TEST_CASE("Execute type conversions") {
    RoxyVM vm;
    vm_init(&vm);

    BCModule* module = new BCModule();
    module->name = StringView("test");

    SUBCASE("Int to float") {
        BCFunction* func = new BCFunction();
        func->name = StringView("i2f");
        func->param_count = 1;
        func->register_count = 2;
        func->code.push_back(encode_abc(Opcode::I2F, 1, 0, 0));
        func->code.push_back(encode_abc(Opcode::RET, 1, 0, 0));
        module->functions.push_back(func);

        vm_load_module(&vm, module);

        Value args[1] = {Value::make_int(42)};
        CHECK(vm_call(&vm, StringView("i2f"), Span<Value>(args, 1)));
        Value result = vm_get_result(&vm);
        CHECK(result.is_float());
        CHECK(result.as_float == 42.0);
    }

    SUBCASE("Float to int") {
        module->functions.clear();
        BCFunction* func = new BCFunction();
        func->name = StringView("f2i");
        func->param_count = 1;
        func->register_count = 2;
        func->code.push_back(encode_abc(Opcode::F2I, 1, 0, 0));
        func->code.push_back(encode_abc(Opcode::RET, 1, 0, 0));
        module->functions.push_back(func);

        vm_load_module(&vm, module);

        Value args[1] = {Value::make_float(42.9)};
        CHECK(vm_call(&vm, StringView("f2i"), Span<Value>(args, 1)));
        Value result = vm_get_result(&vm);
        CHECK(result.is_int());
        CHECK(result.as_int == 42);
    }

    vm_destroy(&vm);
    delete module;
}

TEST_CASE("Error handling") {
    RoxyVM vm;
    vm_init(&vm);

    SUBCASE("Call without module") {
        CHECK(!vm_call(&vm, StringView("main"), {}));
        CHECK(vm_get_error(&vm) != nullptr);
    }

    SUBCASE("Call non-existent function") {
        BCModule* module = new BCModule();
        module->name = StringView("test");
        module->functions.push_back(create_return_int_func("main", 42));
        vm_load_module(&vm, module);

        CHECK(!vm_call(&vm, StringView("not_found"), {}));
        CHECK(vm_get_error(&vm) != nullptr);

        delete module;
    }

    SUBCASE("Division by zero") {
        BCModule* module = new BCModule();
        module->name = StringView("test");

        BCFunction* func = new BCFunction();
        func->name = StringView("div_zero");
        func->param_count = 0;
        func->register_count = 3;
        func->code.push_back(encode_abi(Opcode::LOAD_INT, 0, 10));
        func->code.push_back(encode_abi(Opcode::LOAD_INT, 1, 0));
        func->code.push_back(encode_abc(Opcode::DIV_I, 2, 0, 1));
        func->code.push_back(encode_abc(Opcode::RET, 2, 0, 0));
        module->functions.push_back(func);

        vm_load_module(&vm, module);

        CHECK(!vm_call(&vm, StringView("div_zero"), {}));
        CHECK(vm_get_error(&vm) != nullptr);
        CHECK(strstr(vm_get_error(&vm), "zero") != nullptr);

        delete module;
    }

    vm_destroy(&vm);
}
