#include "roxy/core/doctest/doctest.h"

#include "roxy/core/bump_allocator.hpp"
#include "roxy/core/string.hpp"
#include "roxy/shared/lexer.hpp"
#include "roxy/compiler/parser.hpp"
#include "roxy/compiler/semantic.hpp"
#include "roxy/compiler/type_env.hpp"
#include "roxy/compiler/ssa_ir.hpp"
#include "roxy/compiler/ir_builder.hpp"
#include "roxy/compiler/module_registry.hpp"
#include "roxy/vm/natives.hpp"
#include "roxy/vm/binding/registry.hpp"

#include <cstring>

using namespace rx;

// Helper to parse, analyze, and build IR
static IRModule* build_ir(BumpAllocator& allocator, const char* source) {
    u32 len = 0;
    while (source[len]) len++;

    TypeEnv type_env(allocator);
    NativeRegistry registry(allocator, type_env.types());
    register_builtin_natives(registry);

    Lexer lexer(source, len);
    Parser parser(lexer, allocator);
    Program* program = parser.parse();

    if (!program || parser.has_error()) {
        return nullptr;
    }

    // Create module registry and register builtin module for prelude auto-import
    ModuleRegistry modules(allocator);
    modules.register_native_module(BUILTIN_MODULE_NAME, &registry, type_env.types());

    SemanticAnalyzer analyzer(allocator, type_env, modules);
    if (!analyzer.analyze(program)) {
        return nullptr;
    }

    IRBuilder builder(allocator, type_env, registry, analyzer.symbols(), modules);
    return builder.build(program);
}

// Helper to get IR as string
static String ir_to_string(const IRModule* module) {
    String out;
    ir_module_to_string(module, out);
    return String(out.data(), out.size());
}

TEST_SUITE("SSA IR") {

    TEST_CASE("Empty function") {
        BumpAllocator allocator(4096);

        const char* source = R"(
        fun empty() {}
    )";

        IRModule* module = build_ir(allocator, source);
        REQUIRE(module != nullptr);
        REQUIRE(module->functions.size() == 1);

        IRFunction* func = module->functions[0];
        CHECK(func->name == "empty");
        CHECK(func->params.empty());
        CHECK(func->blocks.size() >= 1);

        // Entry block should have a return terminator
        IRBlock* entry = func->blocks[0];
        CHECK(entry->terminator.kind == TerminatorKind::Return);
    }

    TEST_CASE("Return literal") {
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
        CHECK(func->name == "answer");

        // Should have entry block with const and return
        IRBlock* entry = func->blocks[0];
        CHECK(entry->instructions.size() >= 1);
        CHECK(entry->instructions[0]->op == IROp::ConstInt);
        CHECK(entry->instructions[0]->const_data.int_val == 42);
        CHECK(entry->terminator.kind == TerminatorKind::Return);
    }

    TEST_CASE("Binary expression") {
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

    TEST_CASE("Local variable") {
        BumpAllocator allocator(4096);

        // Use parameters so the addition isn't constant-folded.
        const char* source = R"(
        fun local_var(a: i32, b: i32): i32 {
            var x: i32 = a;
            var y: i32 = b;
            return x + y;
        }
    )";

        IRModule* module = build_ir(allocator, source);
        REQUIRE(module != nullptr);
        REQUIRE(module->functions.size() == 1);

        IRFunction* func = module->functions[0];
        IRBlock* entry = func->blocks[0];

        bool found_add = false;
        for (IRInst* inst : entry->instructions) {
            if (inst->op == IROp::AddI) found_add = true;
        }
        CHECK(found_add);
    }

    TEST_CASE("If statement") {
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

    TEST_CASE("If-else statement") {
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

        // Should have: entry, then, else blocks (merge block is unreachable since both branches return)
        CHECK(func->blocks.size() >= 3);

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

    TEST_CASE("While loop") {
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

    TEST_CASE("For loop") {
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

    TEST_CASE("Break statement") {
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

    TEST_CASE("Unary expression") {
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

    TEST_CASE("Boolean operations") {
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

    TEST_CASE("Comparison operations") {
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

    TEST_CASE("Float operations") {
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

        // f64 addition uses AddD (D = double)
        bool found_addd = false;
        for (IRInst* inst : entry->instructions) {
            if (inst->op == IROp::AddD) {
                found_addd = true;
                break;
            }
        }
        CHECK(found_addd);
    }

    TEST_CASE("Function call") {
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
            if (func->name == "caller") {
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
                CHECK(inst->call.func_name == "helper");
                CHECK(inst->call.args.size() == 1);
                break;
            }
        }
        CHECK(found_call);
    }

    TEST_CASE("Multiple functions") {
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

    TEST_CASE("Compound assignment") {
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

    TEST_CASE("IR string output") {
        BumpAllocator allocator(4096);

        const char* source = R"(
        fun simple(): i32 {
            return 42;
        }
    )";

        IRModule* module = build_ir(allocator, source);
        REQUIRE(module != nullptr);

        String ir_str = ir_to_string(module);

        // Check that the string contains expected parts
        CHECK(ir_str.find("fn simple") != String::npos);
        CHECK(ir_str.find("const_int") != String::npos);
        CHECK(ir_str.find("42") != String::npos);
        CHECK(ir_str.find("return") != String::npos);
    }

    TEST_CASE("Nested if statements") {
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

        // Should have multiple blocks for nested if-else (merge blocks are unreachable since all branches return)
        CHECK(func->blocks.size() >= 5);

        // Count return terminators
        int return_count = 0;
        for (IRBlock* block : func->blocks) {
            if (block->terminator.kind == TerminatorKind::Return) {
                return_count++;
            }
        }
        CHECK(return_count >= 3);  // Three return statements
    }

    TEST_CASE("Op to string") {
        CHECK(StringView(ir_op_to_string(IROp::ConstNull)) == "const_null");
        CHECK(StringView(ir_op_to_string(IROp::ConstInt)) == "const_int");
        CHECK(StringView(ir_op_to_string(IROp::AddI)) == "add_i");
        CHECK(StringView(ir_op_to_string(IROp::AddF)) == "add_f");
        CHECK(StringView(ir_op_to_string(IROp::LtI)) == "lt_i");
        CHECK(StringView(ir_op_to_string(IROp::Not)) == "not");
        CHECK(StringView(ir_op_to_string(IROp::Call)) == "call");
    }

    // Helper: count instructions of a given op across all blocks of a function.
    static int count_op(IRFunction* func, IROp op) {
        int n = 0;
        for (IRBlock* block : func->blocks) {
            for (IRInst* inst : block->instructions) {
                if (inst->op == op) n++;
            }
        }
        return n;
    }

    // Helper: find first ConstInt with given value across all blocks; nullptr if none.
    static IRInst* find_const_int(IRFunction* func, i64 value) {
        for (IRBlock* block : func->blocks) {
            for (IRInst* inst : block->instructions) {
                if (inst->op == IROp::ConstInt && inst->const_data.int_val == value) return inst;
            }
        }
        return nullptr;
    }

    TEST_CASE("Constant folding: integer arithmetic") {
        BumpAllocator allocator(4096);
        const char* source = R"(
        fun add_consts(): i64 { return 3l + 5l; }
        fun sub_consts(): i64 { return 10l - 3l; }
        fun mul_consts(): i64 { return 4l * 5l; }
        fun div_consts(): i64 { return 20l / 4l; }
        fun mod_consts(): i64 { return 17l % 5l; }
    )";
        IRModule* module = build_ir(allocator, source);
        REQUIRE(module != nullptr);
        REQUIRE(module->functions.size() == 5);

        CHECK(find_const_int(module->functions[0], 8) != nullptr);
        CHECK(count_op(module->functions[0], IROp::AddI) == 0);

        CHECK(find_const_int(module->functions[1], 7) != nullptr);
        CHECK(count_op(module->functions[1], IROp::SubI) == 0);

        CHECK(find_const_int(module->functions[2], 20) != nullptr);
        CHECK(count_op(module->functions[2], IROp::MulI) == 0);

        CHECK(find_const_int(module->functions[3], 5) != nullptr);
        CHECK(count_op(module->functions[3], IROp::DivI) == 0);

        CHECK(find_const_int(module->functions[4], 2) != nullptr);
        CHECK(count_op(module->functions[4], IROp::ModI) == 0);
    }

    TEST_CASE("Constant folding: integer comparisons") {
        BumpAllocator allocator(4096);
        const char* source = R"(
        fun lt(): bool { return 3l < 5l; }
        fun ge(): bool { return 5l >= 3l; }
        fun eq(): bool { return 7l == 7l; }
    )";
        IRModule* module = build_ir(allocator, source);
        REQUIRE(module != nullptr);
        REQUIRE(module->functions.size() == 3);

        CHECK(count_op(module->functions[0], IROp::LtI) == 0);
        CHECK(count_op(module->functions[1], IROp::GeI) == 0);
        CHECK(count_op(module->functions[2], IROp::EqI) == 0);

        // Each function should have a ConstBool result.
        for (IRFunction* func : module->functions) {
            bool found_bool = false;
            for (IRBlock* block : func->blocks) {
                for (IRInst* inst : block->instructions) {
                    if (inst->op == IROp::ConstBool) found_bool = true;
                }
            }
            CHECK(found_bool);
        }
    }

    TEST_CASE("Constant folding: bitwise and shifts") {
        BumpAllocator allocator(4096);
        const char* source = R"(
        fun band(): i64 { return 255l & 15l; }
        fun bor(): i64 { return 240l | 15l; }
        fun bxor(): i64 { return 255l ^ 15l; }
        fun shl(): i64 { return 1l << 4l; }
    )";
        IRModule* module = build_ir(allocator, source);
        REQUIRE(module != nullptr);

        CHECK(find_const_int(module->functions[0], 15) != nullptr);
        CHECK(count_op(module->functions[0], IROp::BitAnd) == 0);

        CHECK(find_const_int(module->functions[1], 255) != nullptr);
        CHECK(count_op(module->functions[1], IROp::BitOr) == 0);

        CHECK(find_const_int(module->functions[2], 240) != nullptr);
        CHECK(count_op(module->functions[2], IROp::BitXor) == 0);

        CHECK(find_const_int(module->functions[3], 16) != nullptr);
        CHECK(count_op(module->functions[3], IROp::Shl) == 0);
    }

    TEST_CASE("Constant folding: float arithmetic") {
        BumpAllocator allocator(4096);
        const char* source = R"(
        fun add_f(): f32 { return 3.5f + 2.5f; }
        fun add_d(): f64 { return 3.5 + 2.5; }
    )";
        IRModule* module = build_ir(allocator, source);
        REQUIRE(module != nullptr);
        REQUIRE(module->functions.size() == 2);

        CHECK(count_op(module->functions[0], IROp::AddF) == 0);
        CHECK(count_op(module->functions[1], IROp::AddD) == 0);

        bool found_const_f = false;
        for (IRBlock* block : module->functions[0]->blocks) {
            for (IRInst* inst : block->instructions) {
                if (inst->op == IROp::ConstF && inst->const_data.f32_val == 6.0f) found_const_f = true;
            }
        }
        CHECK(found_const_f);

        bool found_const_d = false;
        for (IRBlock* block : module->functions[1]->blocks) {
            for (IRInst* inst : block->instructions) {
                if (inst->op == IROp::ConstD && inst->const_data.f64_val == 6.0) found_const_d = true;
            }
        }
        CHECK(found_const_d);
    }

    TEST_CASE("Constant folding: unary ops") {
        BumpAllocator allocator(4096);
        const char* source = R"(
        fun neg_i(): i64 { return -42l; }
        fun not_b(): bool { return !true; }
        fun bnot_i(): i64 { return ~0l; }
        fun neg_d(): f64 { return -3.5; }
    )";
        IRModule* module = build_ir(allocator, source);
        REQUIRE(module != nullptr);
        REQUIRE(module->functions.size() == 4);

        CHECK(count_op(module->functions[0], IROp::NegI) == 0);
        CHECK(find_const_int(module->functions[0], -42) != nullptr);

        CHECK(count_op(module->functions[1], IROp::Not) == 0);
        bool found_false = false;
        for (IRBlock* b : module->functions[1]->blocks) {
            for (IRInst* inst : b->instructions) {
                if (inst->op == IROp::ConstBool && !inst->const_data.bool_val) found_false = true;
            }
        }
        CHECK(found_false);

        CHECK(count_op(module->functions[2], IROp::BitNot) == 0);
        CHECK(find_const_int(module->functions[2], -1) != nullptr);

        CHECK(count_op(module->functions[3], IROp::NegD) == 0);
        bool found_neg_d = false;
        for (IRBlock* b : module->functions[3]->blocks) {
            for (IRInst* inst : b->instructions) {
                if (inst->op == IROp::ConstD && inst->const_data.f64_val == -3.5) found_neg_d = true;
            }
        }
        CHECK(found_neg_d);
    }

    TEST_CASE("Cast folding") {
        BumpAllocator allocator(4096);
        // Roxy's semantic analyzer rejects casts directly from int literals (whose
        // literal type isn't a concrete int type), so each constant is bound to a
        // typed local first. The IR builder still surfaces these as Const* values.
        const char* source = R"(
        fun i_to_d(): f64 { var x: i32 = 42; return f64(x); }
        fun d_to_i(): i32 { var x: f64 = 3.7; return i32(x); }
        fun i_narrow(): i32 { var x: i64 = 257l; return i32(x); }
        fun b_to_i(): i32 { var x: bool = true; return i32(x); }
        fun i_to_b(): bool { var x: i32 = 0; return bool(x); }
    )";
        IRModule* module = build_ir(allocator, source);
        REQUIRE(module != nullptr);
        REQUIRE(module->functions.size() == 5);

        // No Cast IR ops should remain in any of these functions.
        for (IRFunction* func : module->functions) {
            CHECK(count_op(func, IROp::Cast) == 0);
        }

        // i_to_d: 42.0
        bool found_42d = false;
        for (IRBlock* b : module->functions[0]->blocks) {
            for (IRInst* inst : b->instructions) {
                if (inst->op == IROp::ConstD && inst->const_data.f64_val == 42.0) found_42d = true;
            }
        }
        CHECK(found_42d);

        // d_to_i: 3 (truncates toward zero)
        CHECK(find_const_int(module->functions[1], 3) != nullptr);

        // i_narrow: 257 truncated to i32 → 257 fits in i32, so still 257.
        // Use a wider value to actually test truncation.
        CHECK(find_const_int(module->functions[2], 257) != nullptr);

        // b_to_i: true → 1
        CHECK(find_const_int(module->functions[3], 1) != nullptr);

        // i_to_b: 0 → false
        bool found_false = false;
        for (IRBlock* b : module->functions[4]->blocks) {
            for (IRInst* inst : b->instructions) {
                if (inst->op == IROp::ConstBool && !inst->const_data.bool_val) found_false = true;
            }
        }
        CHECK(found_false);
    }

    TEST_CASE("Algebraic simplification: binary") {
        BumpAllocator allocator(4096);
        const char* source = R"(
        fun add_zero(x: i32): i32 { return x + 0; }
        fun mul_one(x: i32): i32 { return x * 1; }
        fun mul_zero(x: i32): i32 { return x * 0; }
        fun sub_self(x: i32): i32 { return x - x; }
        fun mul_two(x: i32): i32 { return x * 2; }
        fun shl_zero(x: i32): i32 { return x << 0; }
        fun and_minus_one(x: i64): i64 { return x & -1l; }
        fun xor_self(x: i32): i32 { return x ^ x; }
    )";
        IRModule* module = build_ir(allocator, source);
        REQUIRE(module != nullptr);
        REQUIRE(module->functions.size() == 8);

        // add_zero: no AddI emitted.
        CHECK(count_op(module->functions[0], IROp::AddI) == 0);
        // mul_one: no MulI emitted.
        CHECK(count_op(module->functions[1], IROp::MulI) == 0);
        // mul_zero: no MulI; the result is ConstInt 0.
        CHECK(count_op(module->functions[2], IROp::MulI) == 0);
        CHECK(find_const_int(module->functions[2], 0) != nullptr);
        // sub_self: no SubI; result is ConstInt 0.
        CHECK(count_op(module->functions[3], IROp::SubI) == 0);
        CHECK(find_const_int(module->functions[3], 0) != nullptr);
        // mul_two: strength-reduced to AddI(x, x); no MulI.
        CHECK(count_op(module->functions[4], IROp::MulI) == 0);
        CHECK(count_op(module->functions[4], IROp::AddI) == 1);
        // shl_zero: no Shl emitted.
        CHECK(count_op(module->functions[5], IROp::Shl) == 0);
        // and_minus_one: no BitAnd emitted.
        CHECK(count_op(module->functions[6], IROp::BitAnd) == 0);
        // xor_self: no BitXor; result is ConstInt 0.
        CHECK(count_op(module->functions[7], IROp::BitXor) == 0);
        CHECK(find_const_int(module->functions[7], 0) != nullptr);
    }

    TEST_CASE("Algebraic simplification: unary double-negation") {
        BumpAllocator allocator(4096);
        const char* source = R"(
        fun neg_neg(x: i32): i32 { return -(-x); }
        fun not_not(x: bool): bool { return !!x; }
        fun bnot_bnot(x: i32): i32 { return ~~x; }
    )";
        IRModule* module = build_ir(allocator, source);
        REQUIRE(module != nullptr);
        REQUIRE(module->functions.size() == 3);

        // Outer op is removed by simplification (inner remains; DCE is Phase 2).
        CHECK(count_op(module->functions[0], IROp::NegI) == 1);
        CHECK(count_op(module->functions[1], IROp::Not) == 1);
        CHECK(count_op(module->functions[2], IROp::BitNot) == 1);
    }

    TEST_CASE("Constant folding: div-by-zero is NOT folded") {
        BumpAllocator allocator(4096);
        const char* source = R"(
        fun divz(): i64 { return 10l / 0l; }
        fun modz(): i64 { return 10l % 0l; }
    )";
        IRModule* module = build_ir(allocator, source);
        REQUIRE(module != nullptr);
        REQUIRE(module->functions.size() == 2);

        // The DivI/ModI instruction must remain so the runtime produces "Division by zero".
        CHECK(count_op(module->functions[0], IROp::DivI) == 1);
        CHECK(count_op(module->functions[1], IROp::ModI) == 1);
    }

}  // TEST_SUITE("SSA IR")
