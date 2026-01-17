#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "roxy/core/doctest/doctest.h"

#include "roxy/core/bump_allocator.hpp"
#include "roxy/shared/lexer.hpp"
#include "roxy/compiler/parser.hpp"
#include "roxy/compiler/semantic.hpp"
#include "roxy/compiler/ssa_ir.hpp"
#include "roxy/compiler/ir_builder.hpp"

using namespace rx;

// Helper to parse, analyze, and build IR
static IRModule* build_ir(BumpAllocator& allocator, const char* source) {
    u32 len = 0;
    while (source[len]) len++;

    Lexer lexer(source, len);
    Parser parser(lexer, allocator);
    Program* program = parser.parse();

    if (!program || parser.has_error()) {
        return nullptr;
    }

    SemanticAnalyzer analyzer(allocator);
    if (!analyzer.analyze(program)) {
        return nullptr;
    }

    IRBuilder builder(allocator, analyzer.types());
    return builder.build(program);
}

// Helper to get IR as string
static std::string ir_to_string(const IRModule* module) {
    Vector<char> out;
    ir_module_to_string(module, out);
    return std::string(out.data(), out.size());
}

TEST_CASE("SSA IR - Empty function") {
    BumpAllocator allocator(4096);

    const char* source = R"(
        fun empty() {}
    )";

    IRModule* module = build_ir(allocator, source);
    REQUIRE(module != nullptr);
    REQUIRE(module->functions.size() == 1);

    IRFunction* func = module->functions[0];
    CHECK(func->name == StringView("empty", 5));
    CHECK(func->params.empty());
    CHECK(func->blocks.size() >= 1);

    // Entry block should have a return terminator
    IRBlock* entry = func->blocks[0];
    CHECK(entry->terminator.kind == TerminatorKind::Return);
}

TEST_CASE("SSA IR - Return literal") {
    BumpAllocator allocator(4096);

    const char* source = R"(
        fun answer(): i32 {
            return 42;
        }
    )";

    IRModule* module = build_ir(allocator, source);
    REQUIRE(module != nullptr);
    REQUIRE(module->functions.size() == 1);

    IRFunction* func = module->functions[0];
    CHECK(func->name == StringView("answer", 6));

    // Should have entry block with const and return
    IRBlock* entry = func->blocks[0];
    CHECK(entry->instructions.size() >= 1);
    CHECK(entry->instructions[0]->op == IROp::ConstInt);
    CHECK(entry->instructions[0]->const_data.int_val == 42);
    CHECK(entry->terminator.kind == TerminatorKind::Return);
}

TEST_CASE("SSA IR - Binary expression") {
    BumpAllocator allocator(4096);

    const char* source = R"(
        fun add(a: i32, b: i32): i32 {
            return a + b;
        }
    )";

    IRModule* module = build_ir(allocator, source);
    REQUIRE(module != nullptr);
    REQUIRE(module->functions.size() == 1);

    IRFunction* func = module->functions[0];
    CHECK(func->params.size() == 2);

    // Should have add instruction
    IRBlock* entry = func->blocks[0];
    bool found_add = false;
    for (IRInst* inst : entry->instructions) {
        if (inst->op == IROp::AddI) {
            found_add = true;
            break;
        }
    }
    CHECK(found_add);
}

TEST_CASE("SSA IR - Local variable") {
    BumpAllocator allocator(4096);

    const char* source = R"(
        fun local_var(): i32 {
            var x: i32 = 10;
            var y: i32 = 20;
            return x + y;
        }
    )";

    IRModule* module = build_ir(allocator, source);
    REQUIRE(module != nullptr);
    REQUIRE(module->functions.size() == 1);

    IRFunction* func = module->functions[0];
    IRBlock* entry = func->blocks[0];

    // Should have two const instructions and an add
    int const_count = 0;
    bool found_add = false;
    for (IRInst* inst : entry->instructions) {
        if (inst->op == IROp::ConstInt) const_count++;
        if (inst->op == IROp::AddI) found_add = true;
    }
    CHECK(const_count >= 2);
    CHECK(found_add);
}

TEST_CASE("SSA IR - If statement") {
    BumpAllocator allocator(4096);

    const char* source = R"(
        fun abs(x: i32): i32 {
            if (x < 0) {
                return -x;
            }
            return x;
        }
    )";

    IRModule* module = build_ir(allocator, source);
    REQUIRE(module != nullptr);
    REQUIRE(module->functions.size() == 1);

    IRFunction* func = module->functions[0];

    // Should have multiple blocks: entry, then, endif (at least)
    CHECK(func->blocks.size() >= 3);

    // Entry block should have a branch terminator
    IRBlock* entry = func->blocks[0];
    CHECK(entry->terminator.kind == TerminatorKind::Branch);
}

TEST_CASE("SSA IR - If-else statement") {
    BumpAllocator allocator(4096);

    const char* source = R"(
        fun max(a: i32, b: i32): i32 {
            if (a > b) {
                return a;
            } else {
                return b;
            }
        }
    )";

    IRModule* module = build_ir(allocator, source);
    REQUIRE(module != nullptr);
    REQUIRE(module->functions.size() == 1);

    IRFunction* func = module->functions[0];

    // Should have: entry, then, else, endif blocks
    CHECK(func->blocks.size() >= 4);

    // Both then and else blocks should have return terminators
    bool found_then_return = false;
    bool found_else_return = false;
    for (u32 i = 1; i < func->blocks.size(); i++) {
        IRBlock* block = func->blocks[i];
        if (block->terminator.kind == TerminatorKind::Return) {
            if (!found_then_return) found_then_return = true;
            else found_else_return = true;
        }
    }
    CHECK(found_then_return);
    CHECK(found_else_return);
}

TEST_CASE("SSA IR - While loop") {
    BumpAllocator allocator(4096);

    const char* source = R"(
        fun count(n: i32): i32 {
            var i: i32 = 0;
            while (i < n) {
                i = i + 1;
            }
            return i;
        }
    )";

    IRModule* module = build_ir(allocator, source);
    REQUIRE(module != nullptr);
    REQUIRE(module->functions.size() == 1);

    IRFunction* func = module->functions[0];

    // Should have: entry, while (header), body, endwhile blocks
    CHECK(func->blocks.size() >= 4);

    // Check for loop structure
    bool found_branch = false;
    bool found_back_edge = false;
    for (IRBlock* block : func->blocks) {
        if (block->terminator.kind == TerminatorKind::Branch) {
            found_branch = true;
        }
        // Back edge: goto to a block that comes before
        if (block->terminator.kind == TerminatorKind::Goto) {
            if (block->terminator.goto_target.block.id < block->id.id) {
                found_back_edge = true;
            }
        }
    }
    CHECK(found_branch);
    CHECK(found_back_edge);
}

TEST_CASE("SSA IR - For loop") {
    BumpAllocator allocator(4096);

    const char* source = R"(
        fun sum(n: i32): i32 {
            var total: i32 = 0;
            for (var i: i32 = 0; i < n; i = i + 1) {
                total = total + i;
            }
            return total;
        }
    )";

    IRModule* module = build_ir(allocator, source);
    REQUIRE(module != nullptr);
    REQUIRE(module->functions.size() == 1);

    IRFunction* func = module->functions[0];

    // For loop creates: entry, for (header), forbody, forinc, endfor blocks
    CHECK(func->blocks.size() >= 5);
}

TEST_CASE("SSA IR - Break statement") {
    BumpAllocator allocator(4096);

    const char* source = R"(
        fun find_first(n: i32): i32 {
            var i: i32 = 0;
            while (true) {
                if (i == n) {
                    break;
                }
                i = i + 1;
            }
            return i;
        }
    )";

    IRModule* module = build_ir(allocator, source);
    REQUIRE(module != nullptr);
    REQUIRE(module->functions.size() == 1);

    IRFunction* func = module->functions[0];

    // Should have a goto to the exit block (break)
    bool found_break_goto = false;
    for (IRBlock* block : func->blocks) {
        if (block->terminator.kind == TerminatorKind::Goto) {
            // Check if it's going to a block after the loop header
            // This is a simplified check
            found_break_goto = true;
        }
    }
    CHECK(found_break_goto);
}

TEST_CASE("SSA IR - Unary expression") {
    BumpAllocator allocator(4096);

    const char* source = R"(
        fun negate(x: i32): i32 {
            return -x;
        }
    )";

    IRModule* module = build_ir(allocator, source);
    REQUIRE(module != nullptr);
    REQUIRE(module->functions.size() == 1);

    IRFunction* func = module->functions[0];
    IRBlock* entry = func->blocks[0];

    bool found_neg = false;
    for (IRInst* inst : entry->instructions) {
        if (inst->op == IROp::NegI) {
            found_neg = true;
            break;
        }
    }
    CHECK(found_neg);
}

TEST_CASE("SSA IR - Boolean operations") {
    BumpAllocator allocator(4096);

    const char* source = R"(
        fun not_op(x: bool): bool {
            return !x;
        }
    )";

    IRModule* module = build_ir(allocator, source);
    REQUIRE(module != nullptr);
    REQUIRE(module->functions.size() == 1);

    IRFunction* func = module->functions[0];
    IRBlock* entry = func->blocks[0];

    bool found_not = false;
    for (IRInst* inst : entry->instructions) {
        if (inst->op == IROp::Not) {
            found_not = true;
            break;
        }
    }
    CHECK(found_not);
}

TEST_CASE("SSA IR - Comparison operations") {
    BumpAllocator allocator(4096);

    const char* source = R"(
        fun less(a: i32, b: i32): bool {
            return a < b;
        }
    )";

    IRModule* module = build_ir(allocator, source);
    REQUIRE(module != nullptr);
    REQUIRE(module->functions.size() == 1);

    IRFunction* func = module->functions[0];
    IRBlock* entry = func->blocks[0];

    bool found_lt = false;
    for (IRInst* inst : entry->instructions) {
        if (inst->op == IROp::LtI) {
            found_lt = true;
            break;
        }
    }
    CHECK(found_lt);
}

TEST_CASE("SSA IR - Float operations") {
    BumpAllocator allocator(4096);

    const char* source = R"(
        fun add_floats(a: f64, b: f64): f64 {
            return a + b;
        }
    )";

    IRModule* module = build_ir(allocator, source);
    REQUIRE(module != nullptr);
    REQUIRE(module->functions.size() == 1);

    IRFunction* func = module->functions[0];
    IRBlock* entry = func->blocks[0];

    bool found_addf = false;
    for (IRInst* inst : entry->instructions) {
        if (inst->op == IROp::AddF) {
            found_addf = true;
            break;
        }
    }
    CHECK(found_addf);
}

TEST_CASE("SSA IR - Function call") {
    BumpAllocator allocator(4096);

    const char* source = R"(
        fun helper(x: i32): i32 {
            return x + 1;
        }

        fun caller(): i32 {
            return helper(5);
        }
    )";

    IRModule* module = build_ir(allocator, source);
    REQUIRE(module != nullptr);
    REQUIRE(module->functions.size() == 2);

    // Find the caller function
    IRFunction* caller = nullptr;
    for (IRFunction* func : module->functions) {
        if (func->name == StringView("caller", 6)) {
            caller = func;
            break;
        }
    }
    REQUIRE(caller != nullptr);

    IRBlock* entry = caller->blocks[0];

    bool found_call = false;
    for (IRInst* inst : entry->instructions) {
        if (inst->op == IROp::Call) {
            found_call = true;
            CHECK(inst->call.func_name == StringView("helper", 6));
            CHECK(inst->call.args.size() == 1);
            break;
        }
    }
    CHECK(found_call);
}

TEST_CASE("SSA IR - Multiple functions") {
    BumpAllocator allocator(4096);

    const char* source = R"(
        fun foo(): i32 { return 1; }
        fun bar(): i32 { return 2; }
        fun baz(): i32 { return 3; }
    )";

    IRModule* module = build_ir(allocator, source);
    REQUIRE(module != nullptr);
    CHECK(module->functions.size() == 3);
}

TEST_CASE("SSA IR - Compound assignment") {
    BumpAllocator allocator(4096);

    const char* source = R"(
        fun increment(x: i32): i32 {
            var y: i32 = x;
            y += 1;
            return y;
        }
    )";

    IRModule* module = build_ir(allocator, source);
    REQUIRE(module != nullptr);
    REQUIRE(module->functions.size() == 1);

    IRFunction* func = module->functions[0];
    IRBlock* entry = func->blocks[0];

    // Should have an AddI instruction for the compound assignment
    bool found_add = false;
    for (IRInst* inst : entry->instructions) {
        if (inst->op == IROp::AddI) {
            found_add = true;
            break;
        }
    }
    CHECK(found_add);
}

TEST_CASE("SSA IR - IR string output") {
    BumpAllocator allocator(4096);

    const char* source = R"(
        fun simple(): i32 {
            return 42;
        }
    )";

    IRModule* module = build_ir(allocator, source);
    REQUIRE(module != nullptr);

    std::string ir_str = ir_to_string(module);

    // Check that the string contains expected parts
    CHECK(ir_str.find("fn simple") != std::string::npos);
    CHECK(ir_str.find("const_int") != std::string::npos);
    CHECK(ir_str.find("42") != std::string::npos);
    CHECK(ir_str.find("return") != std::string::npos);
}

TEST_CASE("SSA IR - Nested if statements") {
    BumpAllocator allocator(4096);

    const char* source = R"(
        fun classify(x: i32): i32 {
            if (x < 0) {
                return -1;
            } else {
                if (x > 0) {
                    return 1;
                } else {
                    return 0;
                }
            }
        }
    )";

    IRModule* module = build_ir(allocator, source);
    REQUIRE(module != nullptr);
    REQUIRE(module->functions.size() == 1);

    IRFunction* func = module->functions[0];

    // Should have multiple blocks for nested if-else
    CHECK(func->blocks.size() >= 6);

    // Count return terminators
    int return_count = 0;
    for (IRBlock* block : func->blocks) {
        if (block->terminator.kind == TerminatorKind::Return) {
            return_count++;
        }
    }
    CHECK(return_count >= 3);  // Three return statements
}

TEST_CASE("SSA IR - Op to string") {
    CHECK(std::string(ir_op_to_string(IROp::ConstNull)) == "const_null");
    CHECK(std::string(ir_op_to_string(IROp::ConstInt)) == "const_int");
    CHECK(std::string(ir_op_to_string(IROp::AddI)) == "add_i");
    CHECK(std::string(ir_op_to_string(IROp::AddF)) == "add_f");
    CHECK(std::string(ir_op_to_string(IROp::LtI)) == "lt_i");
    CHECK(std::string(ir_op_to_string(IROp::Not)) == "not");
    CHECK(std::string(ir_op_to_string(IROp::Call)) == "call");
}
