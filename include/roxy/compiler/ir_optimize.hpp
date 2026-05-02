#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/vector.hpp"
#include "roxy/core/bump_allocator.hpp"
#include "roxy/compiler/ssa_ir.hpp"

namespace rx {

// SSA IR optimization passes.
//
// Phase 1 (constant folding, algebraic simplifications, cast folding) is
// implemented eagerly inside IRBuilder::emit_binary / emit_unary /
// gen_primitive_cast. Phase 1 leaves dead originals behind (e.g. after
// folding `2 + 3` to `ConstInt 5`, the `ConstInt 2` and `ConstInt 3` still
// sit in the block); Phase 2 DCE cleans them up.
//
// Phase 2 (this file): use-count computation, Dead Code Elimination,
// Copy Propagation. Per-instruction passes — no CFG mutation.
//
// Phase 3 (this file): control-flow optimizations.
//   - Branch folding: a Branch with a ConstBool condition becomes a Goto.
//   - Block merging: B with sole pred A whose terminator is Goto-to-B is
//     merged into A.
//   - Trivial block-argument elimination: when every predecessor passes
//     the same value for a block parameter, the parameter is replaced
//     with that value and dropped from each pred.
// Phase 3 mutates the CFG in place and relies on
// IRFunction::reorder_blocks_rpo() (called once at the end of
// optimize_function) to drop newly-unreachable blocks and remap BlockId
// references in exception/finally/cleanup metadata.
//
// Phase 4 (this file): block-local Common Subexpression Elimination.
// Within each block, identical pure expressions are deduplicated and
// redirected to the first occurrence. The next DCE round drops the
// orphaned duplicates.

// Run all currently-implemented optimization passes on every function in
// `module`. Idempotent; safe to re-run.
void optimize_module(IRModule* module, BumpAllocator& allocator);

// Per-function driver, exposed for unit tests.
void optimize_function(IRFunction* func, BumpAllocator& allocator);

// Phase 2 building blocks (exposed for unit tests).

// Returns a vector indexed by ValueId.id giving the number of times each
// SSA value is used as an operand (in instructions or terminators).
Vector<u32> compute_use_counts(IRFunction* func);

// Replace uses of every IROp::Copy result with the copy's source, transitively.
// Returns true if any operand was rewritten.
bool run_copy_propagation(IRFunction* func);

// Worklist-based DCE: remove pure instructions whose results are unused.
// Side-effectful instructions (writes, calls, throw/yield, ref counting,
// New/Delete, Nullify) are preserved. BlockArg instructions are also
// preserved; trimming dead block parameters is a Phase 3 pass.
// Returns true if any instruction was removed.
bool run_dce(IRFunction* func);

// Side-effect classification. Used by DCE; will also be used by Phase 4
// local CSE to skip impure ops.
bool has_side_effect(IROp op);

// Phase 3: branch folding. Replaces every Branch terminator whose condition
// is a ConstBool with a Goto to the taken target. Returns true if changed.
bool run_branch_folding(IRFunction* func);

// Phase 3: block merging. Merges block B into its sole predecessor A when
// A's terminator is an unconditional Goto to B. Skips merges that would
// invalidate exception/finally/cleanup metadata. Merged-away blocks are
// emptied in place; reorder_blocks_rpo() will drop them. Returns true if
// any merge occurred.
bool run_block_merging(IRFunction* func);

// Phase 3: trivial block-argument elimination. When all predecessors of a
// block pass the same value for a particular block parameter, replaces the
// parameter with that value (function-wide substitution) and removes the
// argument from every predecessor's jump target. Returns true if changed.
bool run_trivial_block_arg_elim(IRFunction* func);

// Compute predecessors for every block. preds[b.id] lists every block
// whose terminator targets b (with duplicates if a Branch's both arms
// target the same block — those count as two predecessor edges, which
// matters for the trivial-block-arg-elim unanimity check).
//
// We do NOT populate IRBlock::predecessors because that field is currently
// unused outside reorder_blocks_rpo() and could go stale across passes.
Vector<Vector<BlockId>> compute_predecessors(IRFunction* func);

// Phase 4: block-local Common Subexpression Elimination. Within each
// block, builds a hash table keyed on (op, result_type, operands, const
// payload) and redirects later occurrences of an equivalent expression
// to the earlier value. Only applies to pure ops (no memory loads, no
// calls, no side effects). Returns true if any redirection happened.
// The resulting dead duplicates are cleaned up by the next DCE run.
bool run_local_cse(IRFunction* func);

// CSE eligibility classifier. Pure ops where (op, operands, const
// payload) uniquely determines the result with no aliasing or
// side-effect interactions. Excludes memory loads (GetField, LoadPtr,
// IndexGet) and weak-ref operations.
bool is_cse_eligible(IROp op);

// Operand-enumeration helper. Visits each ValueId operand of `inst` (NOT
// including the result, NOT including operands inside a terminator). The
// callback receives a mutable reference so the same helper serves both
// reading (use-count, DCE) and rewriting (copy propagation).
//
// IMPORTANT: This switch covers every IROp. New ops must be added here AND
// to the corresponding switch in lowering.cpp's compute_liveness().
template <typename Fn>
inline void for_each_operand(IRInst* inst, Fn&& fn) {
    switch (inst->op) {
        // ── Binary ops ──
        case IROp::AddI: case IROp::SubI: case IROp::MulI:
        case IROp::DivI: case IROp::ModI:
        case IROp::AddF: case IROp::SubF: case IROp::MulF: case IROp::DivF:
        case IROp::AddD: case IROp::SubD: case IROp::MulD: case IROp::DivD:
        case IROp::EqI: case IROp::NeI: case IROp::LtI:
        case IROp::LeI: case IROp::GtI: case IROp::GeI:
        case IROp::EqF: case IROp::NeF: case IROp::LtF:
        case IROp::LeF: case IROp::GtF: case IROp::GeF:
        case IROp::EqD: case IROp::NeD: case IROp::LtD:
        case IROp::LeD: case IROp::GtD: case IROp::GeD:
        case IROp::And: case IROp::Or:
        case IROp::BitAnd: case IROp::BitOr: case IROp::BitXor:
        case IROp::Shl: case IROp::Shr:
            fn(inst->binary.left);
            fn(inst->binary.right);
            break;

        // ── Unary ops ──
        case IROp::NegI: case IROp::NegF: case IROp::NegD:
        case IROp::BitNot: case IROp::Not:
        case IROp::I_TO_F64: case IROp::F64_TO_I:
        case IROp::I_TO_B: case IROp::B_TO_I:
        case IROp::Copy:
        case IROp::RefInc: case IROp::RefDec:
        case IROp::WeakCheck: case IROp::WeakCreate:
        case IROp::Delete:
        case IROp::Throw: case IROp::Yield:
        case IROp::Nullify:
            fn(inst->unary);
            break;

        // ── Calls (variable-length arg span) ──
        case IROp::Call:
        case IROp::CallNative:
            for (u32 i = 0; i < inst->call.args.size(); i++) {
                fn(inst->call.args[i]);
            }
            break;
        case IROp::CallExternal:
            for (u32 i = 0; i < inst->call_external.args.size(); i++) {
                fn(inst->call_external.args[i]);
            }
            break;
        case IROp::CallIndirect:
            fn(inst->call_indirect.callee);
            for (u32 i = 0; i < inst->call_indirect.args.size(); i++) {
                fn(inst->call_indirect.args[i]);
            }
            break;

        // ── Object construction args ──
        case IROp::New:
            for (u32 i = 0; i < inst->new_data.args.size(); i++) {
                fn(inst->new_data.args[i]);
            }
            break;
        case IROp::Closure:
            for (u32 i = 0; i < inst->closure.captures.size(); i++) {
                fn(inst->closure.captures[i]);
            }
            break;

        // ── Field access ──
        case IROp::GetField:
        case IROp::GetFieldAddr:
            fn(inst->field.object);
            break;
        case IROp::SetField:
            // store_value is a top-level ValueId on IRInst (NOT inside the
            // field union — easy to miss).
            fn(inst->field.object);
            fn(inst->store_value);
            break;

        // ── Container indexing ──
        case IROp::IndexGet:
            fn(inst->index_data.container);
            fn(inst->index_data.index);
            break;
        case IROp::IndexSet:
            fn(inst->index_data.container);
            fn(inst->index_data.index);
            fn(inst->index_data.value);
            break;

        // ── Memory / pointer ops ──
        case IROp::StructCopy:
            fn(inst->struct_copy.dest_ptr);
            fn(inst->struct_copy.source_ptr);
            break;
        case IROp::LoadPtr:
            fn(inst->load_ptr.ptr);
            break;
        case IROp::StorePtr:
            fn(inst->store_ptr.ptr);
            fn(inst->store_ptr.value);
            break;

        // ── Cast ──
        case IROp::Cast:
            fn(inst->cast.source);
            break;

        // ── No-operand ops ──
        case IROp::ConstNull: case IROp::ConstBool: case IROp::ConstInt:
        case IROp::ConstF: case IROp::ConstD: case IROp::ConstString:
        case IROp::StackAlloc:
        case IROp::BlockArg:
        case IROp::VarAddr:
            break;
    }
}

// Visit every ValueId operand inside a block's Terminator: Goto args,
// Branch condition + then/else args, Return value. Mutable references.
template <typename Fn>
inline void for_each_terminator_operand(Terminator& term, Fn&& fn) {
    switch (term.kind) {
        case TerminatorKind::Goto:
            for (u32 i = 0; i < term.goto_target.args.size(); i++) {
                fn(term.goto_target.args[i].value);
            }
            break;
        case TerminatorKind::Branch:
            fn(term.branch.condition);
            for (u32 i = 0; i < term.branch.then_target.args.size(); i++) {
                fn(term.branch.then_target.args[i].value);
            }
            for (u32 i = 0; i < term.branch.else_target.args.size(); i++) {
                fn(term.branch.else_target.args[i].value);
            }
            break;
        case TerminatorKind::Return:
            if (term.return_value.is_valid()) fn(term.return_value);
            break;
        case TerminatorKind::None:
        case TerminatorKind::Unreachable:
            break;
    }
}

}  // namespace rx
