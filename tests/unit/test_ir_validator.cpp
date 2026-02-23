#include "roxy/core/doctest/doctest.h"

#include "roxy/compiler/ir_validator.hpp"
#include "roxy/compiler/ssa_ir.hpp"
#include "roxy/compiler/types.hpp"
#include "roxy/core/bump_allocator.hpp"

using namespace rx;

// Helper: create a minimal valid IR module with a single function that returns a constant
static IRModule* create_valid_module(BumpAllocator& alloc, TypeCache& types) {
    IRFunction* func = alloc.emplace<IRFunction>();
    func->name = "main";
    func->return_type = types.i64_type();

    IRBlock* entry = alloc.emplace<IRBlock>();
    entry->id = BlockId{0};
    entry->name = "entry";

    IRInst* const_inst = alloc.emplace<IRInst>();
    const_inst->op = IROp::ConstInt;
    const_inst->result = func->new_value();
    const_inst->type = types.i64_type();
    const_inst->const_data.int_val = 42;
    entry->instructions.push_back(const_inst);

    entry->terminator.kind = TerminatorKind::Return;
    entry->terminator.return_value = const_inst->result;

    func->blocks.push_back(entry);

    IRModule* module = alloc.emplace<IRModule>();
    module->name = "test";
    module->functions.push_back(func);
    return module;
}

TEST_CASE("IR Validator - valid IR passes") {
    BumpAllocator alloc(4096);
    TypeCache types(alloc);

    IRModule* module = create_valid_module(alloc, types);

    IRValidator validator;
    CHECK(validator.validate(module));
    CHECK(!validator.has_error());
}

TEST_CASE("IR Validator - empty function (no blocks)") {
    BumpAllocator alloc(4096);
    TypeCache types(alloc);

    IRFunction* func = alloc.emplace<IRFunction>();
    func->name = "empty";
    func->return_type = types.void_type();

    IRModule* module = alloc.emplace<IRModule>();
    module->name = "test";
    module->functions.push_back(func);

    IRValidator validator;
    CHECK(!validator.validate(module));
    CHECK(validator.has_error());
    CHECK(strstr(validator.error(), "no blocks") != nullptr);
}

TEST_CASE("IR Validator - missing terminator") {
    BumpAllocator alloc(4096);
    TypeCache types(alloc);

    IRFunction* func = alloc.emplace<IRFunction>();
    func->name = "no_term";
    func->return_type = types.void_type();

    IRBlock* entry = alloc.emplace<IRBlock>();
    entry->id = BlockId{0};
    // terminator.kind defaults to None

    func->blocks.push_back(entry);

    IRModule* module = alloc.emplace<IRModule>();
    module->name = "test";
    module->functions.push_back(func);

    IRValidator validator;
    CHECK(!validator.validate(module));
    CHECK(strstr(validator.error(), "missing terminator") != nullptr);
}

TEST_CASE("IR Validator - invalid ValueId in binary operand") {
    BumpAllocator alloc(4096);
    TypeCache types(alloc);

    IRFunction* func = alloc.emplace<IRFunction>();
    func->name = "bad_binary";
    func->return_type = types.i64_type();

    IRBlock* entry = alloc.emplace<IRBlock>();
    entry->id = BlockId{0};

    IRInst* add_inst = alloc.emplace<IRInst>();
    add_inst->op = IROp::AddI;
    add_inst->result = func->new_value();
    add_inst->type = types.i64_type();
    add_inst->binary.left = ValueId::invalid();  // invalid!
    add_inst->binary.right = ValueId{0};
    entry->instructions.push_back(add_inst);

    entry->terminator.kind = TerminatorKind::Return;
    entry->terminator.return_value = add_inst->result;

    func->blocks.push_back(entry);

    IRModule* module = alloc.emplace<IRModule>();
    module->name = "test";
    module->functions.push_back(func);

    IRValidator validator;
    CHECK(!validator.validate(module));
    CHECK(strstr(validator.error(), "binary op left operand") != nullptr);
}

TEST_CASE("IR Validator - ValueId out of range") {
    BumpAllocator alloc(4096);
    TypeCache types(alloc);

    IRFunction* func = alloc.emplace<IRFunction>();
    func->name = "out_of_range";
    func->return_type = types.i64_type();

    IRBlock* entry = alloc.emplace<IRBlock>();
    entry->id = BlockId{0};

    // Create const with v0
    IRInst* const_inst = alloc.emplace<IRInst>();
    const_inst->op = IROp::ConstInt;
    const_inst->result = func->new_value();  // v0
    const_inst->type = types.i64_type();
    const_inst->const_data.int_val = 1;
    entry->instructions.push_back(const_inst);

    // Create add referencing v999 which is out of range
    IRInst* add_inst = alloc.emplace<IRInst>();
    add_inst->op = IROp::AddI;
    add_inst->result = func->new_value();  // v1
    add_inst->type = types.i64_type();
    add_inst->binary.left = const_inst->result;
    add_inst->binary.right = ValueId{999};  // out of range
    entry->instructions.push_back(add_inst);

    entry->terminator.kind = TerminatorKind::Return;
    entry->terminator.return_value = add_inst->result;

    func->blocks.push_back(entry);

    IRModule* module = alloc.emplace<IRModule>();
    module->name = "test";
    module->functions.push_back(func);

    IRValidator validator;
    CHECK(!validator.validate(module));
    CHECK(strstr(validator.error(), "binary op right operand") != nullptr);
}

TEST_CASE("IR Validator - invalid BlockId in goto target") {
    BumpAllocator alloc(4096);
    TypeCache types(alloc);

    IRFunction* func = alloc.emplace<IRFunction>();
    func->name = "bad_goto";
    func->return_type = types.void_type();

    IRBlock* entry = alloc.emplace<IRBlock>();
    entry->id = BlockId{0};

    // Goto targets block 99 which doesn't exist
    entry->terminator.kind = TerminatorKind::Goto;
    entry->terminator.goto_target.block = BlockId{99};
    entry->terminator.goto_target.args = {};

    func->blocks.push_back(entry);

    IRModule* module = alloc.emplace<IRModule>();
    module->name = "test";
    module->functions.push_back(func);

    IRValidator validator;
    CHECK(!validator.validate(module));
    CHECK(strstr(validator.error(), "target block 99 out of range") != nullptr);
}

TEST_CASE("IR Validator - block arg count mismatch") {
    BumpAllocator alloc(4096);
    TypeCache types(alloc);

    IRFunction* func = alloc.emplace<IRFunction>();
    func->name = "arg_mismatch";
    func->return_type = types.void_type();

    // Block 0: entry, goto block 1 with no args
    IRBlock* entry = alloc.emplace<IRBlock>();
    entry->id = BlockId{0};
    entry->terminator.kind = TerminatorKind::Goto;
    entry->terminator.goto_target.block = BlockId{1};
    entry->terminator.goto_target.args = {};  // 0 args

    // Block 1: expects 1 parameter
    IRBlock* target = alloc.emplace<IRBlock>();
    target->id = BlockId{1};

    BlockParam block_param;
    block_param.value = func->new_value();
    block_param.type = types.i64_type();
    target->params.push_back(block_param);

    target->terminator.kind = TerminatorKind::Return;
    target->terminator.return_value = ValueId::invalid();

    func->blocks.push_back(entry);
    func->blocks.push_back(target);

    IRModule* module = alloc.emplace<IRModule>();
    module->name = "test";
    module->functions.push_back(func);

    IRValidator validator;
    CHECK(!validator.validate(module));
    CHECK(strstr(validator.error(), "arg count") != nullptr);
}

TEST_CASE("IR Validator - null type on instruction with valid result") {
    BumpAllocator alloc(4096);
    TypeCache types(alloc);

    IRFunction* func = alloc.emplace<IRFunction>();
    func->name = "null_type";
    func->return_type = types.i64_type();

    IRBlock* entry = alloc.emplace<IRBlock>();
    entry->id = BlockId{0};

    IRInst* const_inst = alloc.emplace<IRInst>();
    const_inst->op = IROp::ConstInt;
    const_inst->result = func->new_value();
    const_inst->type = nullptr;  // null type!
    const_inst->const_data.int_val = 42;
    entry->instructions.push_back(const_inst);

    entry->terminator.kind = TerminatorKind::Return;
    entry->terminator.return_value = const_inst->result;

    func->blocks.push_back(entry);

    IRModule* module = alloc.emplace<IRModule>();
    module->name = "test";
    module->functions.push_back(func);

    IRValidator validator;
    CHECK(!validator.validate(module));
    CHECK(strstr(validator.error(), "null type") != nullptr);
}

TEST_CASE("IR Validator - block ID != array index") {
    BumpAllocator alloc(4096);
    TypeCache types(alloc);

    IRFunction* func = alloc.emplace<IRFunction>();
    func->name = "bad_block_id";
    func->return_type = types.void_type();

    IRBlock* entry = alloc.emplace<IRBlock>();
    entry->id = BlockId{5};  // Should be 0!

    entry->terminator.kind = TerminatorKind::Return;
    entry->terminator.return_value = ValueId::invalid();

    func->blocks.push_back(entry);

    IRModule* module = alloc.emplace<IRModule>();
    module->name = "test";
    module->functions.push_back(func);

    IRValidator validator;
    CHECK(!validator.validate(module));
    CHECK(strstr(validator.error(), "blocks[0]->id.id == 5") != nullptr);
}

TEST_CASE("IR Validator - invalid branch condition") {
    BumpAllocator alloc(4096);
    TypeCache types(alloc);

    IRFunction* func = alloc.emplace<IRFunction>();
    func->name = "bad_branch";
    func->return_type = types.void_type();

    // Block 0: branch on invalid condition
    IRBlock* entry = alloc.emplace<IRBlock>();
    entry->id = BlockId{0};
    entry->terminator.kind = TerminatorKind::Branch;
    entry->terminator.branch.condition = ValueId::invalid();  // invalid!
    entry->terminator.branch.then_target.block = BlockId{1};
    entry->terminator.branch.then_target.args = {};
    entry->terminator.branch.else_target.block = BlockId{2};
    entry->terminator.branch.else_target.args = {};

    // Block 1 and 2: return
    IRBlock* then_block = alloc.emplace<IRBlock>();
    then_block->id = BlockId{1};
    then_block->terminator.kind = TerminatorKind::Return;
    then_block->terminator.return_value = ValueId::invalid();

    IRBlock* else_block = alloc.emplace<IRBlock>();
    else_block->id = BlockId{2};
    else_block->terminator.kind = TerminatorKind::Return;
    else_block->terminator.return_value = ValueId::invalid();

    func->blocks.push_back(entry);
    func->blocks.push_back(then_block);
    func->blocks.push_back(else_block);

    IRModule* module = alloc.emplace<IRModule>();
    module->name = "test";
    module->functions.push_back(func);

    IRValidator validator;
    CHECK(!validator.validate(module));
    CHECK(strstr(validator.error(), "branch condition") != nullptr);
}

TEST_CASE("IR Validator - BlockArg index out of range") {
    BumpAllocator alloc(4096);
    TypeCache types(alloc);

    IRFunction* func = alloc.emplace<IRFunction>();
    func->name = "bad_block_arg";
    func->return_type = types.i64_type();

    IRBlock* entry = alloc.emplace<IRBlock>();
    entry->id = BlockId{0};
    // Block has no params, but instruction references block_arg_index 0

    IRInst* block_arg_inst = alloc.emplace<IRInst>();
    block_arg_inst->op = IROp::BlockArg;
    block_arg_inst->result = func->new_value();
    block_arg_inst->type = types.i64_type();
    block_arg_inst->block_arg_index = 0;  // out of range (no params)
    entry->instructions.push_back(block_arg_inst);

    entry->terminator.kind = TerminatorKind::Return;
    entry->terminator.return_value = block_arg_inst->result;

    func->blocks.push_back(entry);

    IRModule* module = alloc.emplace<IRModule>();
    module->name = "test";
    module->functions.push_back(func);

    IRValidator validator;
    CHECK(!validator.validate(module));
    CHECK(strstr(validator.error(), "block_arg_index") != nullptr);
}

TEST_CASE("IR Validator - cast with null source_type") {
    BumpAllocator alloc(4096);
    TypeCache types(alloc);

    IRFunction* func = alloc.emplace<IRFunction>();
    func->name = "bad_cast";
    func->return_type = types.f64_type();

    IRBlock* entry = alloc.emplace<IRBlock>();
    entry->id = BlockId{0};

    // Create a source value
    IRInst* const_inst = alloc.emplace<IRInst>();
    const_inst->op = IROp::ConstInt;
    const_inst->result = func->new_value();  // v0
    const_inst->type = types.i64_type();
    const_inst->const_data.int_val = 42;
    entry->instructions.push_back(const_inst);

    // Cast with null source_type
    IRInst* cast_inst = alloc.emplace<IRInst>();
    cast_inst->op = IROp::Cast;
    cast_inst->result = func->new_value();  // v1
    cast_inst->type = types.f64_type();
    cast_inst->cast.source = const_inst->result;
    cast_inst->cast.source_type = nullptr;  // null!
    entry->instructions.push_back(cast_inst);

    entry->terminator.kind = TerminatorKind::Return;
    entry->terminator.return_value = cast_inst->result;

    func->blocks.push_back(entry);

    IRModule* module = alloc.emplace<IRModule>();
    module->name = "test";
    module->functions.push_back(func);

    IRValidator validator;
    CHECK(!validator.validate(module));
    CHECK(strstr(validator.error(), "null source_type") != nullptr);
}
