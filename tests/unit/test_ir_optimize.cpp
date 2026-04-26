#include "roxy/core/doctest/doctest.h"

#include "roxy/core/bump_allocator.hpp"
#include "roxy/core/string.hpp"
#include "roxy/shared/lexer.hpp"
#include "roxy/compiler/parser.hpp"
#include "roxy/compiler/semantic.hpp"
#include "roxy/compiler/type_env.hpp"
#include "roxy/compiler/ssa_ir.hpp"
#include "roxy/compiler/ir_builder.hpp"
#include "roxy/compiler/ir_optimize.hpp"
#include "roxy/compiler/module_registry.hpp"
#include "roxy/vm/natives.hpp"
#include "roxy/vm/binding/registry.hpp"

using namespace rx;

namespace {

// Build IR through the same pipeline test_ssa_ir.cpp uses, so optimizer
// tests see the same IR shape that IRBuilder produces in production.
IRModule* build_ir(BumpAllocator& allocator, const char* source) {
    u32 len = 0;
    while (source[len]) len++;

    TypeEnv type_env(allocator);
    NativeRegistry registry(allocator, type_env.types());
    register_builtin_natives(registry);

    Lexer lexer(source, len);
    Parser parser(lexer, allocator);
    Program* program = parser.parse();
    if (!program || parser.has_error()) return nullptr;

    ModuleRegistry modules(allocator);
    modules.register_native_module(BUILTIN_MODULE_NAME, &registry, type_env.types());

    SemanticAnalyzer analyzer(allocator, type_env, modules);
    if (!analyzer.analyze(program)) return nullptr;

    IRBuilder builder(allocator, type_env, registry, analyzer.symbols(), modules);
    return builder.build(program);
}

IRModule* build_and_optimize(BumpAllocator& allocator, const char* source) {
    IRModule* module = build_ir(allocator, source);
    if (!module) return nullptr;
    optimize_module(module, allocator);
    return module;
}

int count_op(IRFunction* func, IROp op) {
    int n = 0;
    for (IRBlock* block : func->blocks) {
        for (IRInst* inst : block->instructions) {
            if (inst->op == op) n++;
        }
    }
    return n;
}

IRFunction* find_function(IRModule* module, const char* name) {
    for (IRFunction* func : module->functions) {
        if (func->name == name) return func;
    }
    return nullptr;
}

}  // namespace

TEST_CASE("IR Optimize - has_side_effect classification") {
    // Side-effectful ops must NEVER be DCE'd.
    CHECK(has_side_effect(IROp::SetField));
    CHECK(has_side_effect(IROp::StorePtr));
    CHECK(has_side_effect(IROp::StructCopy));
    CHECK(has_side_effect(IROp::IndexSet));
    CHECK(has_side_effect(IROp::RefInc));
    CHECK(has_side_effect(IROp::RefDec));
    CHECK(has_side_effect(IROp::New));
    CHECK(has_side_effect(IROp::Delete));
    CHECK(has_side_effect(IROp::Call));
    CHECK(has_side_effect(IROp::CallNative));
    CHECK(has_side_effect(IROp::CallExternal));
    CHECK(has_side_effect(IROp::Throw));
    CHECK(has_side_effect(IROp::Yield));
    // Critical: lowering reads Nullify position to narrow cleanup scope.
    // Removing it would re-destroy moved-from owned locals.
    CHECK(has_side_effect(IROp::Nullify));

    // Pure ops — DCE may remove if unused.
    CHECK_FALSE(has_side_effect(IROp::AddI));
    CHECK_FALSE(has_side_effect(IROp::SubI));
    CHECK_FALSE(has_side_effect(IROp::MulI));
    CHECK_FALSE(has_side_effect(IROp::Copy));
    CHECK_FALSE(has_side_effect(IROp::ConstInt));
    CHECK_FALSE(has_side_effect(IROp::GetField));
    CHECK_FALSE(has_side_effect(IROp::GetFieldAddr));
    CHECK_FALSE(has_side_effect(IROp::LoadPtr));
    CHECK_FALSE(has_side_effect(IROp::Cast));
    CHECK_FALSE(has_side_effect(IROp::WeakCheck));
    CHECK_FALSE(has_side_effect(IROp::WeakCreate));
    CHECK_FALSE(has_side_effect(IROp::IndexGet));
}

TEST_CASE("IR Optimize - DCE removes dead arithmetic") {
    BumpAllocator allocator(4096);
    const char* source = R"(
        fun dead_add(a: i32, b: i32): i32 {
            var unused: i32 = a + b;
            return a;
        }
    )";
    IRModule* module = build_and_optimize(allocator, source);
    REQUIRE(module != nullptr);
    IRFunction* func = find_function(module, "dead_add");
    REQUIRE(func != nullptr);

    CHECK(count_op(func, IROp::AddI) == 0);
}

TEST_CASE("IR Optimize - DCE removes transitive dead chain") {
    BumpAllocator allocator(4096);
    const char* source = R"(
        fun chain(a: i32): i32 {
            var x: i32 = a + 1;
            var y: i32 = x + 1;
            var z: i32 = y + 1;
            return a;
        }
    )";
    IRModule* module = build_and_optimize(allocator, source);
    REQUIRE(module != nullptr);
    IRFunction* func = find_function(module, "chain");
    REQUIRE(func != nullptr);

    // All three AddI instructions chain through dead values; DCE worklist
    // should kill them transitively.
    CHECK(count_op(func, IROp::AddI) == 0);
}

TEST_CASE("IR Optimize - DCE preserves CallNative side effects") {
    BumpAllocator allocator(4096);
    const char* source = R"(
        fun calls() {
            print("hi");
        }
    )";
    IRModule* module = build_and_optimize(allocator, source);
    REQUIRE(module != nullptr);
    IRFunction* func = find_function(module, "calls");
    REQUIRE(func != nullptr);

    // print() result is unused, but the call must remain — printing is
    // observable.
    CHECK(count_op(func, IROp::CallNative) == 1);
}

TEST_CASE("IR Optimize - DCE preserves SetField") {
    BumpAllocator allocator(4096);
    const char* source = R"(
        struct Vec {
            x: f32 = 0.0f;
            y: f32 = 0.0f;
        }
        fun store(p: ref Vec) {
            p.x = 1.0f;
        }
    )";
    IRModule* module = build_and_optimize(allocator, source);
    REQUIRE(module != nullptr);
    IRFunction* func = find_function(module, "store");
    REQUIRE(func != nullptr);

    // SetField is a memory write — must survive even with no further uses.
    CHECK(count_op(func, IROp::SetField) == 1);
}

TEST_CASE("IR Optimize - DCE removes Phase 1 leftover constants") {
    BumpAllocator allocator(4096);
    const char* source = R"(
        fun fold_const(): i64 {
            return 2l + 3l;
        }
    )";
    IRModule* module = build_and_optimize(allocator, source);
    REQUIRE(module != nullptr);
    IRFunction* func = find_function(module, "fold_const");
    REQUIRE(func != nullptr);

    // Phase 1 fold leaves ConstInt 2 and ConstInt 3 behind after replacing
    // their AddI with ConstInt 5. After Phase 2 DCE, only the folded
    // result remains.
    CHECK(count_op(func, IROp::AddI) == 0);
    CHECK(count_op(func, IROp::ConstInt) == 1);

    // Verify the surviving constant is the folded value 5.
    bool found_five = false;
    for (IRBlock* block : func->blocks) {
        for (IRInst* inst : block->instructions) {
            if (inst->op == IROp::ConstInt && inst->const_data.int_val == 5) {
                found_five = true;
            }
        }
    }
    CHECK(found_five);
}

TEST_CASE("IR Optimize - DCE preserves Nullify on uniq move") {
    BumpAllocator allocator(4096);
    const char* source = R"(
        struct Box {
            x: i32 = 0;
        }
        fun take(b: uniq Box) {}
        fun mover() {
            var b: uniq Box = uniq Box {};
            take(b);
        }
    )";
    IRModule* module = build_and_optimize(allocator, source);
    REQUIRE(module != nullptr);
    IRFunction* func = find_function(module, "mover");
    REQUIRE(func != nullptr);

    // Passing a uniq local to a uniq parameter emits Nullify so exception
    // cleanup skips the moved-from slot. Nullify has no useful result and
    // zero uses; without the side-effect guard, DCE would remove it and
    // exception cleanup would re-destroy the moved object. The has_side_effect
    // guard is what keeps this test green.
    CHECK(count_op(func, IROp::Nullify) >= 1);
}

TEST_CASE("IR Optimize - compute_use_counts includes block-arg operands") {
    BumpAllocator allocator(4096);
    // A function whose if/else branch passes the same value as a block
    // argument. The block-arg's source value should have its use count
    // bumped for each branch arm — proving the terminator-operand walk
    // correctly visits Branch::then_target.args and else_target.args.
    const char* source = R"(
        fun branchy(c: bool, a: i32): i32 {
            var x: i32 = a;
            if (c) {
                x = a;
            } else {
                x = a;
            }
            return x;
        }
    )";
    IRModule* module = build_ir(allocator, source);  // do NOT optimize — we test the analysis primitive
    REQUIRE(module != nullptr);
    IRFunction* func = find_function(module, "branchy");
    REQUIRE(func != nullptr);

    Vector<u32> counts = compute_use_counts(func);

    // Param `a` is ValueId{1} (after `c` at id 0). It's used in three
    // contexts: var x = a (Copy or direct), then x = a in each branch arm
    // which becomes a block-arg passing `a`. We don't pin the exact count
    // (IR shape can shift), but it must be at least 2 — proving block-arg
    // operands inside terminators were enumerated.
    REQUIRE(counts.size() >= 2);
    CHECK(counts[1] >= 2);
}

TEST_CASE("IR Optimize - optimize_function is idempotent") {
    BumpAllocator allocator(4096);
    const char* source = R"(
        fun dead_add(a: i32, b: i32): i32 {
            var unused: i32 = a + b;
            return a;
        }
    )";
    IRModule* module = build_ir(allocator, source);
    REQUIRE(module != nullptr);
    IRFunction* func = find_function(module, "dead_add");
    REQUIRE(func != nullptr);

    // First run: must remove something.
    bool first_copy = run_copy_propagation(func);
    bool first_dce = run_dce(func);
    (void)first_copy;
    CHECK(first_dce);  // dead AddI was removed

    // Second run: must be a no-op (fixed point).
    CHECK_FALSE(run_copy_propagation(func));
    CHECK_FALSE(run_dce(func));
}

// ===========================================================================
// Phase 3 tests — control-flow optimizations.
// ===========================================================================

namespace {

int count_terminator(IRFunction* func, TerminatorKind kind) {
    int n = 0;
    for (IRBlock* block : func->blocks) {
        if (block->terminator.kind == kind) n++;
    }
    return n;
}

}  // namespace

TEST_CASE("IR Optimize - branch folding: if(true) keeps then arm") {
    BumpAllocator allocator(4096);
    const char* source = R"(
        fun true_branch(): i32 {
            if (true) {
                return 1;
            } else {
                return 2;
            }
        }
    )";
    IRModule* module = build_and_optimize(allocator, source);
    REQUIRE(module != nullptr);
    IRFunction* func = find_function(module, "true_branch");
    REQUIRE(func != nullptr);

    // Branch terminator should be folded away.
    CHECK(count_terminator(func, TerminatorKind::Branch) == 0);
    // The constant 2 should be DCE'd along with the unreachable else arm.
    bool found_one = false, found_two = false;
    for (IRBlock* block : func->blocks) {
        for (IRInst* inst : block->instructions) {
            if (inst->op == IROp::ConstInt) {
                if (inst->const_data.int_val == 1) found_one = true;
                if (inst->const_data.int_val == 2) found_two = true;
            }
        }
    }
    CHECK(found_one);
    CHECK_FALSE(found_two);
}

TEST_CASE("IR Optimize - branch folding: if(false) keeps else arm") {
    BumpAllocator allocator(4096);
    const char* source = R"(
        fun false_branch(): i32 {
            if (false) {
                return 1;
            } else {
                return 2;
            }
        }
    )";
    IRModule* module = build_and_optimize(allocator, source);
    REQUIRE(module != nullptr);
    IRFunction* func = find_function(module, "false_branch");
    REQUIRE(func != nullptr);

    CHECK(count_terminator(func, TerminatorKind::Branch) == 0);
    bool found_one = false, found_two = false;
    for (IRBlock* block : func->blocks) {
        for (IRInst* inst : block->instructions) {
            if (inst->op == IROp::ConstInt) {
                if (inst->const_data.int_val == 1) found_one = true;
                if (inst->const_data.int_val == 2) found_two = true;
            }
        }
    }
    CHECK_FALSE(found_one);
    CHECK(found_two);
}

TEST_CASE("IR Optimize - branch folding feeds DCE on dead arm computation") {
    BumpAllocator allocator(4096);
    const char* source = R"(
        fun dead_arm_calc(a: i32, b: i32): i32 {
            if (true) {
                return a;
            } else {
                var dead: i32 = a + b;
                return dead;
            }
        }
    )";
    IRModule* module = build_and_optimize(allocator, source);
    REQUIRE(module != nullptr);
    IRFunction* func = find_function(module, "dead_arm_calc");
    REQUIRE(func != nullptr);

    // The AddI in the dead arm should be gone after fold + RPO + DCE.
    CHECK(count_op(func, IROp::AddI) == 0);
}

TEST_CASE("IR Optimize - block merging collapses straight-line successor") {
    BumpAllocator allocator(4096);
    // After branch folding, the surviving arm becomes the unique successor
    // of entry; block merging should collapse the resulting Goto chain.
    const char* source = R"(
        fun straight(): i32 {
            if (true) {
                return 42;
            } else {
                return 0;
            }
        }
    )";
    IRModule* module = build_and_optimize(allocator, source);
    REQUIRE(module != nullptr);
    IRFunction* func = find_function(module, "straight");
    REQUIRE(func != nullptr);

    // Maximum collapse: one block with a return.
    CHECK(func->blocks.size() == 1);
    CHECK(func->blocks[0]->terminator.kind == TerminatorKind::Return);
}

TEST_CASE("IR Optimize - trivial block-arg elimination on if/else with same value") {
    BumpAllocator allocator(4096);
    const char* source = R"(
        fun same_val(c: bool, a: i32): i32 {
            var x: i32 = 0;
            if (c) {
                x = a;
            } else {
                x = a;
            }
            return x;
        }
    )";
    IRModule* module = build_and_optimize(allocator, source);
    REQUIRE(module != nullptr);
    IRFunction* func = find_function(module, "same_val");
    REQUIRE(func != nullptr);

    // After trivial block-arg elim, no block in this function should have
    // any params (the join's `x_param` was the only one and it got
    // collapsed because both arms passed the same value `a`). Function-
    // entry block params are exempt because they ARE the function params,
    // but the IR builder emits those into entry differently — entry's
    // `params` typically has function args. The join block, originally
    // with one param, should now have zero.
    bool any_join_param = false;
    for (u32 i = 1; i < func->blocks.size(); i++) {  // skip entry
        if (func->blocks[i]->params.size() > 0) { any_join_param = true; break; }
    }
    CHECK_FALSE(any_join_param);
}

TEST_CASE("IR Optimize - trivial block-arg elimination preserves loop params") {
    BumpAllocator allocator(4096);
    const char* source = R"(
        fun loop_sum(n: i32): i32 {
            var i: i32 = 0;
            var s: i32 = 0;
            while (i < n) {
                s = s + i;
                i = i + 1;
            }
            return s;
        }
    )";
    IRModule* module = build_and_optimize(allocator, source);
    REQUIRE(module != nullptr);
    IRFunction* func = find_function(module, "loop_sum");
    REQUIRE(func != nullptr);

    // The loop header has params `i` and `s`. Predecessors disagree —
    // the entry edge passes 0, the back-edge passes the updated value —
    // so trivial-arg-elim must NOT collapse them.
    u32 total_non_entry_params = 0;
    for (u32 i = 1; i < func->blocks.size(); i++) {
        total_non_entry_params += static_cast<u32>(func->blocks[i]->params.size());
    }
    CHECK(total_non_entry_params >= 2);  // at least i and s survive
}

TEST_CASE("IR Optimize - Phase 3 driver is idempotent") {
    BumpAllocator allocator(4096);
    const char* source = R"(
        fun mixed(c: bool, a: i32): i32 {
            if (true) {
                if (c) { return a; } else { return a; }
            } else {
                return 0;
            }
        }
    )";
    IRModule* module = build_and_optimize(allocator, source);
    REQUIRE(module != nullptr);
    IRFunction* func = find_function(module, "mixed");
    REQUIRE(func != nullptr);

    // Re-running each Phase 3 pass must report no change.
    CHECK_FALSE(run_branch_folding(func));
    CHECK_FALSE(run_block_merging(func));
    CHECK_FALSE(run_trivial_block_arg_elim(func));
}

TEST_CASE("IR Optimize - exception handler metadata survives optimization") {
    BumpAllocator allocator(8192);
    const char* source = R"(
        struct MyErr {
            x: i32 = 0;
        }
        fun MyErr.message(): string for Exception {
            return "err";
        }
        fun maybe_throw(c: bool) {
            if (c) {
                throw MyErr {};
            }
        }
        fun guarded(c: bool): i32 {
            try {
                maybe_throw(c);
                return 1;
            } catch (e: MyErr) {
                return 2;
            }
        }
    )";
    IRModule* module = build_and_optimize(allocator, source);
    REQUIRE(module != nullptr);
    IRFunction* func = find_function(module, "guarded");
    REQUIRE(func != nullptr);

    // Exception handler metadata must reference valid blocks within
    // func->blocks. This guards against block merging across an
    // exception-handler boundary or RPO remapping going wrong.
    REQUIRE(func->exception_handlers.size() >= 1);
    for (const auto& h : func->exception_handlers) {
        CHECK(h.try_entry.is_valid());
        CHECK(h.try_entry.id < func->blocks.size());
        CHECK(h.try_exit.is_valid());
        CHECK(h.try_exit.id < func->blocks.size());
        CHECK(h.handler_block.is_valid());
        CHECK(h.handler_block.id < func->blocks.size());
        for (BlockId tb : h.try_body_blocks) {
            CHECK(tb.is_valid());
            CHECK(tb.id < func->blocks.size());
        }
    }
}
