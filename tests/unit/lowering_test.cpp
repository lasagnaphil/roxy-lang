#include "roxy/core/doctest/doctest.h"

#include "roxy/compiler/lowering.hpp"
#include "roxy/compiler/ssa_ir.hpp"
#include "roxy/compiler/types.hpp"
#include "roxy/vm/vm.hpp"
#include "roxy/vm/interpreter.hpp"
#include "roxy/core/bump_allocator.hpp"

using namespace rx;

// Helper to create a simple IR function that returns a constant
IRFunction* create_return_const_func(BumpAllocator& alloc, const char* name, i64 value, TypeCache& types) {
    IRFunction* func = alloc.emplace<IRFunction>();
    func->name = name;
    func->return_type = types.i64_type();

    // Create entry block
    IRBlock* entry = alloc.emplace<IRBlock>();
    entry->id = BlockId{0};
    entry->name = "entry";

    // Create constant instruction
    IRInst* const_inst = alloc.emplace<IRInst>();
    const_inst->op = IROp::ConstInt;
    const_inst->result = func->new_value();
    const_inst->type = types.i64_type();
    const_inst->const_data.int_val = value;
    entry->instructions.push_back(const_inst);

    // Create return terminator
    entry->terminator.kind = TerminatorKind::Return;
    entry->terminator.return_value = const_inst->result;

    func->blocks.push_back(entry);
    return func;
}

// Helper to create an IR function that adds two parameters
IRFunction* create_add_func(BumpAllocator& alloc, const char* name, TypeCache& types) {
    IRFunction* func = alloc.emplace<IRFunction>();
    func->name = name;
    func->return_type = types.i64_type();

    // Add parameters
    BlockParam param0;
    param0.value = func->new_value();
    param0.type = types.i64_type();
    param0.name = "a";
    func->params.push_back(param0);

    BlockParam param1;
    param1.value = func->new_value();
    param1.type = types.i64_type();
    param1.name = "b";
    func->params.push_back(param1);

    // Create entry block
    IRBlock* entry = alloc.emplace<IRBlock>();
    entry->id = BlockId{0};
    entry->name = "entry";

    // Create add instruction: result = a + b
    IRInst* add_inst = alloc.emplace<IRInst>();
    add_inst->op = IROp::AddI;
    add_inst->result = func->new_value();
    add_inst->type = types.i64_type();
    add_inst->binary.left = param0.value;
    add_inst->binary.right = param1.value;
    entry->instructions.push_back(add_inst);

    // Create return terminator
    entry->terminator.kind = TerminatorKind::Return;
    entry->terminator.return_value = add_inst->result;

    func->blocks.push_back(entry);
    return func;
}

TEST_CASE("Lower simple return constant") {
    BumpAllocator alloc(4096);
    TypeCache types(alloc);

    IRModule* ir_module = alloc.emplace<IRModule>();
    ir_module->name = "test";
    ir_module->functions.push_back(create_return_const_func(alloc, "main", 42, types));

    BytecodeBuilder builder;
    BCModule* bc_module = builder.build(ir_module);

    REQUIRE(bc_module != nullptr);
    CHECK(bc_module->functions.size() == 1);

    BCFunction* func = bc_module->functions[0];
    CHECK(func->name == "main");
    CHECK(func->param_count == 0);
    CHECK(func->code.size() >= 2);  // At least LOAD_INT and RET

    // Execute to verify correctness
    RoxyVM vm;
    vm_init(&vm);
    vm_load_module(&vm, bc_module);

    CHECK(vm_call(&vm, "main", {}));
    Value result = vm_get_result(&vm);
    CHECK(result.is_int());
    CHECK(result.as_int == 42);

    vm_destroy(&vm);
    delete bc_module;
}

TEST_CASE("Lower add function") {
    BumpAllocator alloc(4096);
    TypeCache types(alloc);

    IRModule* ir_module = alloc.emplace<IRModule>();
    ir_module->name = "test";
    ir_module->functions.push_back(create_add_func(alloc, "add", types));

    BytecodeBuilder builder;
    BCModule* bc_module = builder.build(ir_module);

    REQUIRE(bc_module != nullptr);
    CHECK(bc_module->functions.size() == 1);

    BCFunction* func = bc_module->functions[0];
    CHECK(func->name == "add");
    CHECK(func->param_count == 2);

    // Execute to verify correctness
    RoxyVM vm;
    vm_init(&vm);
    vm_load_module(&vm, bc_module);

    Value args[2] = {Value::make_int(10), Value::make_int(32)};
    CHECK(vm_call(&vm, "add", Span<Value>(args, 2)));
    Value result = vm_get_result(&vm);
    CHECK(result.is_int());
    CHECK(result.as_int == 42);

    vm_destroy(&vm);
    delete bc_module;
}

TEST_CASE("Lower arithmetic operations") {
    BumpAllocator alloc(4096);
    TypeCache types(alloc);

    SUBCASE("Subtraction") {
        IRFunction* func = alloc.emplace<IRFunction>();
            func->name = "sub";
        func->return_type = types.i64_type();

        // Parameters
        BlockParam param0, param1;
        param0.value = func->new_value();
        param0.type = types.i64_type();
        param1.value = func->new_value();
        param1.type = types.i64_type();
        func->params.push_back(param0);
        func->params.push_back(param1);

        // Entry block
        IRBlock* entry = alloc.emplace<IRBlock>();
            entry->id = BlockId{0};

        // Sub instruction
        IRInst* sub = alloc.emplace<IRInst>();
        sub->op = IROp::SubI;
        sub->result = func->new_value();
        sub->type = types.i64_type();
        sub->binary.left = param0.value;
        sub->binary.right = param1.value;
        entry->instructions.push_back(sub);

        entry->terminator.kind = TerminatorKind::Return;
        entry->terminator.return_value = sub->result;
        func->blocks.push_back(entry);

        IRModule* ir_module = alloc.emplace<IRModule>();
            ir_module->name = "test";
        ir_module->functions.push_back(func);

        BytecodeBuilder builder;
        BCModule* bc_module = builder.build(ir_module);

        RoxyVM vm;
        vm_init(&vm);
        vm_load_module(&vm, bc_module);

        Value args[2] = {Value::make_int(50), Value::make_int(8)};
        CHECK(vm_call(&vm, "sub", Span<Value>(args, 2)));
        Value result = vm_get_result(&vm);
        CHECK(result.as_int == 42);

        vm_destroy(&vm);
        delete bc_module;
    }

    SUBCASE("Multiplication") {
        IRFunction* func = alloc.emplace<IRFunction>();
            func->name = "mul";
        func->return_type = types.i64_type();

        BlockParam param0, param1;
        param0.value = func->new_value();
        param0.type = types.i64_type();
        param1.value = func->new_value();
        param1.type = types.i64_type();
        func->params.push_back(param0);
        func->params.push_back(param1);

        IRBlock* entry = alloc.emplace<IRBlock>();
            entry->id = BlockId{0};

        IRInst* mul = alloc.emplace<IRInst>();
        mul->op = IROp::MulI;
        mul->result = func->new_value();
        mul->type = types.i64_type();
        mul->binary.left = param0.value;
        mul->binary.right = param1.value;
        entry->instructions.push_back(mul);

        entry->terminator.kind = TerminatorKind::Return;
        entry->terminator.return_value = mul->result;
        func->blocks.push_back(entry);

        IRModule* ir_module = alloc.emplace<IRModule>();
            ir_module->name = "test";
        ir_module->functions.push_back(func);

        BytecodeBuilder builder;
        BCModule* bc_module = builder.build(ir_module);

        RoxyVM vm;
        vm_init(&vm);
        vm_load_module(&vm, bc_module);

        Value args[2] = {Value::make_int(6), Value::make_int(7)};
        CHECK(vm_call(&vm, "mul", Span<Value>(args, 2)));
        Value result = vm_get_result(&vm);
        CHECK(result.as_int == 42);

        vm_destroy(&vm);
        delete bc_module;
    }
}

TEST_CASE("Lower constant pool usage") {
    BumpAllocator alloc(4096);
    TypeCache types(alloc);

    // Test large integer that doesn't fit in 16-bit immediate
    IRFunction* func = alloc.emplace<IRFunction>();
    func->name = "main";
    func->return_type = types.i64_type();

    IRBlock* entry = alloc.emplace<IRBlock>();
    entry->id = BlockId{0};

    IRInst* const_inst = alloc.emplace<IRInst>();
    const_inst->op = IROp::ConstInt;
    const_inst->result = func->new_value();
    const_inst->type = types.i64_type();
    const_inst->const_data.int_val = 1000000;
    entry->instructions.push_back(const_inst);

    entry->terminator.kind = TerminatorKind::Return;
    entry->terminator.return_value = const_inst->result;
    func->blocks.push_back(entry);

    IRModule* ir_module = alloc.emplace<IRModule>();
    ir_module->name = "test";
    ir_module->functions.push_back(func);

    BytecodeBuilder builder;
    BCModule* bc_module = builder.build(ir_module);

    CHECK(bc_module->functions[0]->constants.size() == 1);
    CHECK(bc_module->functions[0]->constants[0].as_int == 1000000);

    RoxyVM vm;
    vm_init(&vm);
    vm_load_module(&vm, bc_module);

    CHECK(vm_call(&vm, "main", {}));
    Value result = vm_get_result(&vm);
    CHECK(result.as_int == 1000000);

    vm_destroy(&vm);
    delete bc_module;
}

TEST_CASE("Lower float constant") {
    BumpAllocator alloc(4096);
    TypeCache types(alloc);

    IRFunction* func = alloc.emplace<IRFunction>();
    func->name = "main";
    func->return_type = types.f64_type();

    IRBlock* entry = alloc.emplace<IRBlock>();
    entry->id = BlockId{0};

    IRInst* const_inst = alloc.emplace<IRInst>();
    const_inst->op = IROp::ConstFloat;
    const_inst->result = func->new_value();
    const_inst->type = types.f64_type();
    const_inst->const_data.float_val = 3.14159;
    entry->instructions.push_back(const_inst);

    entry->terminator.kind = TerminatorKind::Return;
    entry->terminator.return_value = const_inst->result;
    func->blocks.push_back(entry);

    IRModule* ir_module = alloc.emplace<IRModule>();
    ir_module->name = "test";
    ir_module->functions.push_back(func);

    BytecodeBuilder builder;
    BCModule* bc_module = builder.build(ir_module);

    CHECK(bc_module->functions[0]->constants.size() == 1);

    RoxyVM vm;
    vm_init(&vm);
    vm_load_module(&vm, bc_module);

    CHECK(vm_call(&vm, "main", {}));
    Value result = vm_get_result(&vm);
    // With untyped u64 registers, type info is not preserved - use float_from_u64
    Value float_result = Value::float_from_u64(result.as_u64());
    CHECK(float_result.as_float == doctest::Approx(3.14159));

    vm_destroy(&vm);
    delete bc_module;
}

TEST_CASE("Lower comparison operations") {
    BumpAllocator alloc(4096);
    TypeCache types(alloc);

    IRFunction* func = alloc.emplace<IRFunction>();
    func->name = "lt";
    func->return_type = types.bool_type();

    BlockParam param0, param1;
    param0.value = func->new_value();
    param0.type = types.i64_type();
    param1.value = func->new_value();
    param1.type = types.i64_type();
    func->params.push_back(param0);
    func->params.push_back(param1);

    IRBlock* entry = alloc.emplace<IRBlock>();
    entry->id = BlockId{0};

    IRInst* lt = alloc.emplace<IRInst>();
    lt->op = IROp::LtI;
    lt->result = func->new_value();
    lt->type = types.bool_type();
    lt->binary.left = param0.value;
    lt->binary.right = param1.value;
    entry->instructions.push_back(lt);

    entry->terminator.kind = TerminatorKind::Return;
    entry->terminator.return_value = lt->result;
    func->blocks.push_back(entry);

    IRModule* ir_module = alloc.emplace<IRModule>();
    ir_module->name = "test";
    ir_module->functions.push_back(func);

    BytecodeBuilder builder;
    BCModule* bc_module = builder.build(ir_module);

    RoxyVM vm;
    vm_init(&vm);
    vm_load_module(&vm, bc_module);

    Value args_true[2] = {Value::make_int(5), Value::make_int(10)};
    CHECK(vm_call(&vm, "lt", Span<Value>(args_true, 2)));
    CHECK(vm_get_result(&vm).as_bool == true);

    Value args_false[2] = {Value::make_int(10), Value::make_int(5)};
    CHECK(vm_call(&vm, "lt", Span<Value>(args_false, 2)));
    CHECK(vm_get_result(&vm).as_bool == false);

    vm_destroy(&vm);
    delete bc_module;
}

TEST_CASE("Disassemble lowered bytecode") {
    BumpAllocator alloc(4096);
    TypeCache types(alloc);

    IRModule* ir_module = alloc.emplace<IRModule>();
    ir_module->name = "test";
    ir_module->functions.push_back(create_add_func(alloc, "add", types));

    BytecodeBuilder builder;
    BCModule* bc_module = builder.build(ir_module);

    Vector<char> out;
    disassemble_module(bc_module, out);

    // Check that disassembly contains expected elements
    CHECK(strstr(out.data(), "test") != nullptr);
    CHECK(strstr(out.data(), "add") != nullptr);
    CHECK(strstr(out.data(), "ADD_I") != nullptr);
    CHECK(strstr(out.data(), "RET") != nullptr);

    delete bc_module;
}
