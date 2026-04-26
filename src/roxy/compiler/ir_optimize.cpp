#include "roxy/compiler/ir_optimize.hpp"

namespace rx {

bool has_side_effect(IROp op) {
    switch (op) {
        // Memory writes
        case IROp::SetField:
        case IROp::StorePtr:
        case IROp::StructCopy:
        case IROp::IndexSet:
        // Reference counting
        case IROp::RefInc:
        case IROp::RefDec:
        // Object lifecycle (constructors / destructors may print, throw,
        // mutate out params)
        case IROp::New:
        case IROp::Delete:
        // Calls — opaque, may do anything
        case IROp::Call:
        case IROp::CallNative:
        case IROp::CallExternal:
        // Control-flow / exceptional / coroutine
        case IROp::Throw:
        case IROp::Yield:
        // Cleanup-record metadata: lowering reads the position of Nullify
        // (m_nullify_pcs) to narrow cleanup scope after a uniq move.
        // Removing it would re-destroy moved-from owned locals.
        case IROp::Nullify:
            return true;
        default:
            return false;
    }
}

Vector<u32> compute_use_counts(IRFunction* func) {
    Vector<u32> counts(func->next_value_id, 0u);
    auto bump = [&](ValueId v) {
        if (v.is_valid() && v.id < counts.size()) counts[v.id]++;
    };
    for (IRBlock* block : func->blocks) {
        for (IRInst* inst : block->instructions) {
            for_each_operand(inst, [&](ValueId& v) { bump(v); });
        }
        for_each_terminator_operand(block->terminator, [&](ValueId& v) { bump(v); });
    }
    return counts;
}

bool run_dce(IRFunction* func) {
    const u32 N = func->next_value_id;
    Vector<u32> use_counts = compute_use_counts(func);
    Vector<bool> is_dead(N, false);

    // Worklist of value IDs whose defining instruction is currently
    // dead-pending (zero uses, no side effect, not a BlockArg).
    Vector<u32> worklist;
    worklist.reserve(N);

    auto consider = [&](IRInst* inst) {
        if (!inst) return;                          // function/block param
        if (!inst->result.is_valid()) return;
        u32 id = inst->result.id;
        if (id >= N) return;
        if (is_dead[id]) return;
        if (use_counts[id] != 0) return;
        if (has_side_effect(inst->op)) return;
        // Phase 2 leaves block parameters alone; trimming dead block-args
        // is a Phase 3 pass.
        if (inst->op == IROp::BlockArg) return;
        is_dead[id] = true;
        worklist.push_back(id);
    };

    // Seed.
    for (IRBlock* block : func->blocks) {
        for (IRInst* inst : block->instructions) {
            consider(inst);
        }
    }

    // Iterate to fixed point.
    while (!worklist.empty()) {
        u32 id = worklist.pop_back();
        IRInst* inst = func->values_by_id[id];
        // Decrement operand use counts; if any drops to zero, re-consider.
        for_each_operand(inst, [&](ValueId& v) {
            if (!v.is_valid() || v.id >= N) return;
            if (use_counts[v.id] > 0) {
                use_counts[v.id]--;
                if (use_counts[v.id] == 0) {
                    consider(func->values_by_id[v.id]);
                }
            }
        });
    }

    // Compact each block's instruction vector and poison values_by_id slots
    // for removed values (helps later passes catch stale lookups).
    bool changed = false;
    for (IRBlock* block : func->blocks) {
        u32 w = 0;
        for (u32 r = 0; r < block->instructions.size(); r++) {
            IRInst* inst = block->instructions[r];
            if (inst->result.is_valid() && is_dead[inst->result.id]) {
                func->values_by_id[inst->result.id] = nullptr;
                changed = true;
            } else {
                block->instructions[w++] = inst;
            }
        }
        // Vector::resize() reallocates and may copy out-of-range slots when
        // shrinking, so use pop_back() to truncate in place.
        while (block->instructions.size() > w) block->instructions.pop_back();
    }
    return changed;
}

bool run_copy_propagation(IRFunction* func) {
    const u32 N = func->next_value_id;
    if (N == 0) return false;

    // subst[id] = id        means "no substitution" (root)
    // subst[id] = other.id  means "use other instead of this"
    Vector<u32> subst(N, 0u);
    for (u32 i = 0; i < N; i++) subst[i] = i;

    // Pass 1: every Copy is a substitution v_result -> v_source.
    for (u32 i = 0; i < N; i++) {
        IRInst* inst = func->values_by_id[i];
        if (!inst) continue;
        if (inst->op != IROp::Copy) continue;
        if (!inst->unary.is_valid()) continue;
        u32 src = inst->unary.id;
        if (src != i && src < N) subst[i] = src;
    }

    // Pass 2: path compression with halving.
    auto find = [&](u32 id) -> u32 {
        while (subst[id] != id) {
            subst[id] = subst[subst[id]];
            id = subst[id];
        }
        return id;
    };
    for (u32 i = 0; i < N; i++) (void)find(i);

    // Pass 3: rewrite all operands and terminator operands.
    bool changed = false;
    auto rewrite = [&](ValueId& v) {
        if (!v.is_valid() || v.id >= N) return;
        u32 root = subst[v.id];
        if (root != v.id) {
            v = ValueId{root};
            changed = true;
        }
    };
    for (IRBlock* block : func->blocks) {
        for (IRInst* inst : block->instructions) {
            // Skip rewriting Copy's own operand. After rewrite, the Copy is
            // useless (its uses point past it) and DCE removes it; keeping
            // its `unary` pointing at the original source means DCE's
            // operand decrement debits the correct value.
            if (inst->op == IROp::Copy) continue;
            for_each_operand(inst, [&](ValueId& v) { rewrite(v); });
        }
        for_each_terminator_operand(block->terminator, [&](ValueId& v) { rewrite(v); });
    }
    return changed;
}

// =====================================================================
// Phase 3: control-flow optimizations.
// =====================================================================

Vector<Vector<BlockId>> compute_predecessors(IRFunction* func) {
    Vector<Vector<BlockId>> preds(static_cast<u32>(func->blocks.size()));
    auto add_edge = [&](BlockId from, BlockId to) {
        if (!to.is_valid() || to.id >= preds.size()) return;
        preds[to.id].push_back(from);
    };
    for (IRBlock* block : func->blocks) {
        const Terminator& t = block->terminator;
        switch (t.kind) {
            case TerminatorKind::Goto:
                add_edge(block->id, t.goto_target.block);
                break;
            case TerminatorKind::Branch:
                add_edge(block->id, t.branch.then_target.block);
                add_edge(block->id, t.branch.else_target.block);
                break;
            default: break;
        }
    }
    return preds;
}

bool run_branch_folding(IRFunction* func) {
    bool changed = false;
    for (IRBlock* block : func->blocks) {
        Terminator& t = block->terminator;
        if (t.kind != TerminatorKind::Branch) continue;
        IRInst* cond_def = func->inst_for(t.branch.condition);
        if (!cond_def || cond_def->op != IROp::ConstBool) continue;
        bool taken = cond_def->const_data.bool_val;
        // Snapshot the chosen target before mutating the union.
        JumpTarget chosen = taken ? t.branch.then_target : t.branch.else_target;
        t.kind = TerminatorKind::Goto;
        t.goto_target = chosen;
        changed = true;
    }
    return changed;
}

// Returns true if the block is referenced by any exception/finally/cleanup
// metadata in a position where merging it would invalidate that metadata.
// This is conservative — we forbid merging away blocks that participate in
// any handler region or cleanup live-range. Rewriting metadata to remap the
// removed block to its merged-into target is delicate (try_body_blocks is
// order-sensitive, try_exit is the inclusive end of a contiguous bytecode
// range), so v1 errs on the side of safety.
static bool block_in_metadata(IRFunction* func, BlockId b) {
    for (const IRExceptionHandler& h : func->exception_handlers) {
        if (h.try_entry == b) return true;
        if (h.try_exit == b) return true;
        if (h.handler_block == b) return true;
        for (BlockId tb : h.try_body_blocks) if (tb == b) return true;
    }
    for (const IRFinallyInfo& f : func->finally_handlers) {
        if (f.try_entry == b) return true;
        if (f.try_exit == b) return true;
        if (f.finally_block == b) return true;
        if (f.finally_end_block == b) return true;
    }
    for (const IRCleanupInfo& ci : func->cleanup_info) {
        if (ci.start_block == b) return true;
        if (ci.end_block == b) return true;
    }
    return false;
}

bool run_block_merging(IRFunction* func) {
    bool any_changed = false;
    bool inner_changed = true;
    while (inner_changed) {
        inner_changed = false;
        Vector<Vector<BlockId>> preds = compute_predecessors(func);
        for (u32 b_idx = 0; b_idx < func->blocks.size(); b_idx++) {
            IRBlock* B = func->blocks[b_idx];
            if (B->terminator.kind == TerminatorKind::None) continue;  // already-emptied
            if (preds[B->id.id].size() != 1) continue;
            BlockId a_id = preds[B->id.id][0];
            if (a_id == B->id) continue;                                // self-loop
            if (a_id.id >= func->blocks.size()) continue;
            IRBlock* A = func->blocks[a_id.id];
            // A's terminator must be an unconditional Goto to B (no other
            // successors).
            if (A->terminator.kind != TerminatorKind::Goto) continue;
            if (A->terminator.goto_target.block != B->id) continue;
            // Skip if B is referenced by any exception/finally/cleanup
            // metadata. (We don't rewrite metadata across a merge in v1.)
            if (block_in_metadata(func, B->id)) continue;

            // Build substitution map: B's params -> A's goto args.
            Span<BlockArgPair> a_args = A->terminator.goto_target.args;
            const u32 N = func->next_value_id;
            Vector<u32> subst(N);
            for (u32 i = 0; i < N; i++) subst[i] = i;
            // Block-arg count mismatches would indicate malformed IR; assert
            // and skip rather than crash on out-of-range indexing.
            if (a_args.size() != B->params.size()) continue;
            for (u32 i = 0; i < B->params.size(); i++) {
                ValueId param = B->params[i].value;
                ValueId arg = a_args[i].value;
                if (param.is_valid() && param.id < N) {
                    subst[param.id] = arg.id;
                }
            }
            auto find = [&](u32 id) -> u32 {
                while (subst[id] != id) { subst[id] = subst[subst[id]]; id = subst[id]; }
                return id;
            };
            for (u32 i = 0; i < N; i++) (void)find(i);
            auto rewrite = [&](ValueId& v) {
                if (!v.is_valid() || v.id >= N) return;
                if (subst[v.id] != v.id) v = ValueId{subst[v.id]};
            };
            // Rewrite B's instructions and terminator (B-local refs to its
            // params now resolve to A's args).
            for (IRInst* inst : B->instructions) {
                for_each_operand(inst, [&](ValueId& v) { rewrite(v); });
            }
            for_each_terminator_operand(B->terminator, [&](ValueId& v) { rewrite(v); });

            // Move B's instructions into A, replace A's terminator.
            for (IRInst* inst : B->instructions) {
                A->instructions.push_back(inst);
            }
            A->terminator = B->terminator;

            // Empty B in place; reorder_blocks_rpo() will drop it later.
            while (B->params.size() > 0) B->params.pop_back();
            while (B->instructions.size() > 0) B->instructions.pop_back();
            B->terminator = Terminator{};

            inner_changed = true;
            any_changed = true;
            break;  // restart scan (preds vector is now stale)
        }
    }
    return any_changed;
}

// Return the value passed by predecessor P for the param_idx-th parameter
// of `target`. Sets `ok=false` if the predecessor doesn't actually target
// `target` or arg counts don't match. For a Branch with both arms targeting
// the same block, both arms must agree on the value (else ok=false).
static ValueId arg_for_target(IRBlock* P, BlockId target, u32 param_idx, bool& ok) {
    ok = true;
    const Terminator& t = P->terminator;
    auto get = [&](const JumpTarget& jt) -> ValueId {
        if (jt.args.size() <= param_idx) { ok = false; return ValueId::invalid(); }
        return jt.args[param_idx].value;
    };
    switch (t.kind) {
        case TerminatorKind::Goto:
            if (t.goto_target.block != target) { ok = false; return ValueId::invalid(); }
            return get(t.goto_target);
        case TerminatorKind::Branch: {
            bool then_match = (t.branch.then_target.block == target);
            bool else_match = (t.branch.else_target.block == target);
            if (!then_match && !else_match) { ok = false; return ValueId::invalid(); }
            if (then_match && else_match) {
                ValueId tv = get(t.branch.then_target);
                if (!ok) return ValueId::invalid();
                ValueId ev = get(t.branch.else_target);
                if (!ok) return ValueId::invalid();
                if (tv.id != ev.id) { ok = false; return ValueId::invalid(); }
                return tv;
            }
            return then_match ? get(t.branch.then_target) : get(t.branch.else_target);
        }
        default:
            ok = false;
            return ValueId::invalid();
    }
}

// Compact a JumpTarget's args span by dropping indices marked false in
// `keep`. The bump allocator never frees, so we just reduce the visible
// size — leftover slots leak but compile lifetime is bounded.
static void compact_jump_target(JumpTarget& jt, const Vector<bool>& keep) {
    if (jt.args.size() == 0) return;
    u32 w = 0;
    for (u32 r = 0; r < jt.args.size(); r++) {
        if (r >= keep.size() || keep[r]) {
            if (w != r) jt.args[w] = jt.args[r];
            w++;
        }
    }
    jt.args = Span<BlockArgPair>(jt.args.data(), w);
}

bool run_trivial_block_arg_elim(IRFunction* func) {
    Vector<Vector<BlockId>> preds = compute_predecessors(func);
    const u32 N = func->next_value_id;
    Vector<u32> subst(N);
    for (u32 i = 0; i < N; i++) subst[i] = i;
    bool any = false;

    // For each block, indices of params to drop.
    Vector<Vector<bool>> drop_keep(static_cast<u32>(func->blocks.size()));
    for (u32 b = 0; b < func->blocks.size(); b++) {
        IRBlock* B = func->blocks[b];
        drop_keep[b] = Vector<bool>(static_cast<u32>(B->params.size()), true);
    }

    for (u32 b = 0; b < func->blocks.size(); b++) {
        IRBlock* B = func->blocks[b];
        // Skip the entry block — its "params" are function parameters, not
        // block arguments fed by predecessors. (Entry has no predecessors.)
        if (b == 0) continue;
        for (u32 pi = 0; pi < B->params.size(); pi++) {
            ValueId param_val = B->params[pi].value;
            if (!param_val.is_valid()) continue;
            ValueId common = ValueId::invalid();
            bool unanimous = true;
            for (BlockId pred_id : preds[B->id.id]) {
                if (pred_id.id >= func->blocks.size()) { unanimous = false; break; }
                IRBlock* P = func->blocks[pred_id.id];
                bool ok = true;
                ValueId arg = arg_for_target(P, B->id, pi, ok);
                if (!ok) { unanimous = false; break; }
                if (!arg.is_valid()) { unanimous = false; break; }
                // Self-references (loop back-edge feeding the param itself)
                // don't constrain the unanimity check — strip them.
                if (arg.id == param_val.id) continue;
                if (!common.is_valid()) common = arg;
                else if (common.id != arg.id) { unanimous = false; break; }
            }
            if (!unanimous) continue;
            if (!common.is_valid()) continue;  // only self-refs, no real value
            subst[param_val.id] = common.id;
            drop_keep[b][pi] = false;
            any = true;
        }
    }
    if (!any) return false;

    // Path-compress so chains (param -> param -> value) collapse.
    auto find = [&](u32 id) -> u32 {
        while (subst[id] != id) { subst[id] = subst[subst[id]]; id = subst[id]; }
        return id;
    };
    for (u32 i = 0; i < N; i++) (void)find(i);

    // Rewrite operands across the function.
    auto rewrite = [&](ValueId& v) {
        if (!v.is_valid() || v.id >= N) return;
        if (subst[v.id] != v.id) v = ValueId{subst[v.id]};
    };
    for (IRBlock* block : func->blocks) {
        for (IRInst* inst : block->instructions) {
            for_each_operand(inst, [&](ValueId& v) { rewrite(v); });
        }
        for_each_terminator_operand(block->terminator, [&](ValueId& v) { rewrite(v); });
    }

    // Drop trivial params from each block + matching args from each pred's
    // jump target.
    for (u32 b = 0; b < func->blocks.size(); b++) {
        const Vector<bool>& keep = drop_keep[b];
        bool any_dropped = false;
        for (u32 i = 0; i < keep.size(); i++) if (!keep[i]) { any_dropped = true; break; }
        if (!any_dropped) continue;
        IRBlock* B = func->blocks[b];
        // Compact B->params.
        u32 w = 0;
        for (u32 r = 0; r < B->params.size(); r++) {
            if (r >= keep.size() || keep[r]) {
                if (w != r) B->params[w] = B->params[r];
                w++;
            }
        }
        while (B->params.size() > w) B->params.pop_back();
        // Compact every predecessor's jump-target args.
        for (BlockId pred_id : preds[b]) {
            if (pred_id.id >= func->blocks.size()) continue;
            IRBlock* P = func->blocks[pred_id.id];
            Terminator& t = P->terminator;
            switch (t.kind) {
                case TerminatorKind::Goto:
                    if (t.goto_target.block.id == b) compact_jump_target(t.goto_target, keep);
                    break;
                case TerminatorKind::Branch:
                    if (t.branch.then_target.block.id == b) compact_jump_target(t.branch.then_target, keep);
                    if (t.branch.else_target.block.id == b) compact_jump_target(t.branch.else_target, keep);
                    break;
                default: break;
            }
        }
    }
    return true;
}

void optimize_function(IRFunction* func, BumpAllocator& /*allocator*/) {
    // Phase 2 first to clean up Copy chains (so branch conditions resolve
    // to their underlying ConstBool, not a Copy of one) and dead values.
    run_copy_propagation(func);
    run_dce(func);

    // Phase 3 to fixed point. Each pass can expose opportunities for the
    // others (folded branch -> unreachable block, merged block -> exposed
    // dead values, eliminated trivial param -> simplified operands).
    bool changed = true;
    while (changed) {
        changed = false;
        if (run_branch_folding(func)) changed = true;
        if (run_block_merging(func)) changed = true;
        if (run_trivial_block_arg_elim(func)) changed = true;
        if (changed) {
            // Re-run Phase 2 to clean up dead values exposed by the CFG
            // mutations (ConstBool conditions, eliminated params, etc.).
            run_copy_propagation(func);
            run_dce(func);
        }
    }

    // Drop now-unreachable blocks (from branch folding) and emptied blocks
    // (from block merging) and re-establish RPO. This also remaps every
    // BlockId reference in terminators and exception/finally/cleanup
    // metadata, so the optimized IR validates and lowers correctly.
    func->reorder_blocks_rpo();
}

void optimize_module(IRModule* module, BumpAllocator& allocator) {
    for (IRFunction* func : module->functions) {
        optimize_function(func, allocator);
    }
}

}  // namespace rx
