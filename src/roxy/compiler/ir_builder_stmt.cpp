// IRBuilder — statement generation: block management, the shared branch/merge
// (phi + scope-snapshot) machinery, statement and declaration lowering, and
// assigned-variable collection. Split out of ir_builder.cpp (which keeps the
// class overview and the module/decl build phases); file-internal helpers
// shared across the ir_builder*.cpp TUs live in ir_builder_internal.hpp.

#include "roxy/compiler/ir_builder.hpp"

#include "ir_builder_internal.hpp"

namespace rx {

using namespace ir_builder_detail;

IRBlock* IRBuilder::create_block(StringView name) {
    IRBlock* block = m_allocator.emplace<IRBlock>();
    block->id = BlockId{static_cast<u32>(m_current_func->blocks.size())};
    block->name = name;
    m_current_func->blocks.push_back(block);
    return block;
}

void IRBuilder::set_current_block(IRBlock* block) {
    m_current_block = block;
}

void IRBuilder::finish_block_goto(BlockId target, Span<BlockArgPair> args) {
    if (!m_current_block) return;
    m_current_block->terminator.kind = TerminatorKind::Goto;
    m_current_block->terminator.goto_target.block = target;
    m_current_block->terminator.goto_target.args = args;
}

void IRBuilder::finish_block_branch(ValueId cond, BlockId then_block, BlockId else_block,
                                    Span<BlockArgPair> then_args, Span<BlockArgPair> else_args) {
    if (!m_current_block) return;
    m_current_block->terminator.kind = TerminatorKind::Branch;
    m_current_block->terminator.branch.condition = cond;
    m_current_block->terminator.branch.then_target.block = then_block;
    m_current_block->terminator.branch.then_target.args = then_args;
    m_current_block->terminator.branch.else_target.block = else_block;
    m_current_block->terminator.branch.else_target.args = else_args;
}

void IRBuilder::finish_block_return(ValueId value) {
    if (!m_current_block) return;

    // Release ref borrows before returning (constraint reference model)
    emit_ref_param_decrements();

    m_current_block->terminator.kind = TerminatorKind::Return;
    m_current_block->terminator.return_value = value;
}

void IRBuilder::finish_block_unreachable() {
    if (!m_current_block) return;
    m_current_block->terminator.kind = TerminatorKind::Unreachable;
    m_current_block = nullptr;  // Dead code after unreachable
}

// Instruction emission

void IRBuilder::gen_stmt(Stmt* stmt) {
    if (!stmt) return;

    // Track this statement's source line so `emit_inst` can stamp it onto
    // every IRInst produced by lowering its body. Nested blocks/expressions
    // overwrite and never need to restore — at the top of each new
    // statement we re-set, and synthesized lowering paths that don't go
    // through `gen_stmt` keep `source_line == 0`.
    if (stmt->loc.line != 0) m_current_source_line = stmt->loc.line;

    switch (stmt->kind) {
        case AstKind::StmtExpr:
            gen_expr_stmt(stmt);
            break;
        case AstKind::StmtBlock:
            gen_block_stmt(stmt);
            break;
        case AstKind::StmtIf:
            gen_if_stmt(stmt);
            break;
        case AstKind::StmtWhile:
            gen_while_stmt(stmt);
            break;
        case AstKind::StmtFor:
            gen_for_stmt(stmt);
            break;
        case AstKind::StmtReturn:
            gen_return_stmt(stmt);
            break;
        case AstKind::StmtBreak:
            gen_break_stmt(stmt);
            break;
        case AstKind::StmtContinue:
            gen_continue_stmt(stmt);
            break;
        case AstKind::StmtDelete:
            gen_delete_stmt(stmt);
            break;
        case AstKind::StmtWhen:
            gen_when_stmt(stmt);
            break;
        case AstKind::StmtThrow:
            gen_throw_stmt(stmt);
            break;
        case AstKind::StmtTry:
            gen_try_stmt(stmt);
            break;
        case AstKind::StmtYield:
            gen_yield_stmt(stmt);
            break;
        default:
            break;
    }
}

void IRBuilder::gen_expr_stmt(Stmt* stmt) {
    gen_expr(stmt->expr_stmt.expr);
}

void IRBuilder::gen_block_stmt(Stmt* stmt) {
    push_scope();

    BlockStmt& block = stmt->block;
    for (auto* decl : block.declarations) {
        gen_decl(decl);
    }

    pop_scope();
}

IRBuilder::ScopeSnapshot IRBuilder::snapshot_scopes(bool with_move_state) {
    ScopeSnapshot snapshot;
    snapshot.scopes.reserve(m_local_scopes.size());
    for (auto& scope : m_local_scopes) {
        snapshot.scopes.push_back(scope);
    }
    if (with_move_state) {
        snapshot.has_move_state = true;
        m_ownership.snapshot_move_state(snapshot.is_moved);
    }
    return snapshot;
}

void IRBuilder::restore_scopes(const ScopeSnapshot& snapshot, bool restore_move_state) {
    m_local_scopes.clear_keep_capacity();
    for (const auto& scope : snapshot.scopes) {
        m_local_scopes.push_back(scope);
    }
    if (restore_move_state && snapshot.has_move_state) {
        m_ownership.restore_move_state(snapshot.is_moved);
    }
}

void IRBuilder::restore_scopes_move(ScopeSnapshot&& snapshot) {
    m_local_scopes = std::move(snapshot.scopes);
    if (snapshot.has_move_state) {
        m_ownership.restore_move_state(snapshot.is_moved);
    }
}

Vector<IRBuilder::PhiInfo> IRBuilder::make_merge_phis(IRBlock* merge_block,
                                                      const Vector<StringView>& modified) {
    Vector<PhiInfo> phi_info;
    for (const auto& name : modified) {
        LocalVar* local_var = find_local(name);
        if (!local_var || !local_var->value.is_valid()) continue;
        bool seen = false;
        for (const auto& phi : phi_info) {
            if (phi.name == name) { seen = true; break; }
        }
        if (seen) continue;
        ValueId param = m_current_func->new_value();
        merge_block->params.push_back({param, local_var->type, name});
        phi_info.push_back({name, local_var->type, param, local_var->value});
    }
    return phi_info;
}

Span<BlockArgPair> IRBuilder::phi_original_args(const Vector<PhiInfo>& phi_info) {
    Span<BlockArgPair> args = alloc_span<BlockArgPair>(static_cast<u32>(phi_info.size()));
    for (u32 i = 0; i < phi_info.size(); i++) {
        args[i] = BlockArgPair{phi_info[i].original_value};
    }
    return args;
}

Span<BlockArgPair> IRBuilder::phi_current_args(const Vector<PhiInfo>& phi_info) {
    Span<BlockArgPair> args = alloc_span<BlockArgPair>(static_cast<u32>(phi_info.size()));
    for (u32 i = 0; i < phi_info.size(); i++) {
        args[i] = BlockArgPair{lookup_local(phi_info[i].name)};
    }
    return args;
}

void IRBuilder::bind_merge_phis(const Vector<PhiInfo>& phi_info) {
    for (const auto& phi : phi_info) {
        define_local(phi.name, phi.merge_param, phi.type);
    }
}

void IRBuilder::goto_merge_if_open(IRBlock* merge_block, const Vector<PhiInfo>& phi_info) {
    if (m_current_block && m_current_block->terminator.kind == TerminatorKind::None) {
        finish_block_goto(merge_block->id, phi_current_args(phi_info));
    }
}

void IRBuilder::gen_if_stmt(Stmt* stmt) {
    IfStmt& is = stmt->if_stmt;

    // Detect else-if chains and use flat codegen to avoid quadratic compilation
    if (is.else_branch && is.else_branch->kind == AstKind::StmtIf) {
        gen_if_else_chain(stmt);
        return;
    }

    // Evaluate condition
    ValueId cond = gen_expr(is.condition);

    // Variables assigned in either branch that already exist before the if
    // need phi nodes (block params) at the merge point.
    Vector<StringView> modified;
    collect_assigned_vars(is.then_branch, modified);
    if (is.else_branch) {
        collect_assigned_vars(is.else_branch, modified);
    }

    // Create blocks and merge-block phi params
    IRBlock* then_block = create_block("then");
    IRBlock* else_block = is.else_branch ? create_block("else") : nullptr;
    IRBlock* merge_block = create_block("endif");
    Vector<PhiInfo> phi_info = make_merge_phis(merge_block, modified);

    // Branch based on condition
    if (else_block) {
        finish_block_branch(cond, then_block->id, else_block->id);
    } else {
        // No else branch - pass original values as args to merge_block
        finish_block_branch(cond, then_block->id, merge_block->id, {},
                            phi_original_args(phi_info));
    }

    // Save pre-if local-scope and is_moved state. We must roll the IR builder's
    // bookkeeping (nullify-replace, owned_local.is_moved) back across terminating
    // branches so the merge block — reachable only via the surviving paths —
    // doesn't see consumed/nil-replaced locals from a branch that returned. This
    // mirrors the semantic-side definite-termination move-state merging.
    // (Move-restored at most once: the else path and the no-else path below are
    // mutually exclusive.)
    ScopeSnapshot pre_if = snapshot_scopes(/*with_move_state=*/true);

    // Generate then branch
    set_current_block(then_block);
    gen_stmt(is.then_branch);
    bool then_terminated = !m_current_block || m_current_block->terminator.kind != TerminatorKind::None;
    goto_merge_if_open(merge_block, phi_info);

    // Snapshot post-then state in case the else branch terminates and we need to
    // restore the then-branch's mutations for code after the merge.
    ScopeSnapshot post_then;
    if (else_block && !then_terminated) {
        post_then = snapshot_scopes(/*with_move_state=*/true);
    }

    // Generate else branch
    bool else_terminated = false;
    if (else_block) {
        // Restore pre-if state so the else branch sees original values
        restore_scopes_move(std::move(pre_if));

        set_current_block(else_block);
        gen_stmt(is.else_branch);
        else_terminated = !m_current_block || m_current_block->terminator.kind != TerminatorKind::None;
        goto_merge_if_open(merge_block, phi_info);
    }

    // Pick the IR-builder state to use at the merge block:
    //   - no else, then terminated: only the cond-false path reaches merge → pre-if state
    //   - else exists, then terminated, else not: only else path → keep current (else's post-state)
    //   - else exists, else terminated, then not: only then path → restore post-then snapshot
    //   - both terminated: merge unreachable, state doesn't matter
    //   - neither terminated: keep current (else's post-state); non-phi vars should match
    //     by construction (semantic forbids divergent moves on non-phi vars), and phi
    //     vars are rebound from merge-block params below.
    if (!else_block) {
        if (then_terminated) {
            restore_scopes_move(std::move(pre_if));
        }
    } else if (then_terminated && !else_terminated) {
        // Current state is post-else, which is correct.
    } else if (else_terminated && !then_terminated) {
        restore_scopes_move(std::move(post_then));
    }

    // Continue with merge block; bind variables to merge params (phi results)
    set_current_block(merge_block);
    bind_merge_phis(phi_info);
}

void IRBuilder::gen_if_else_chain(Stmt* stmt) {
    // Flatten the nested if-else AST into a linear list of (condition, body) pairs
    struct IfElseBranch {
        Expr* condition;
        Stmt* body;
    };
    Vector<IfElseBranch> branches;
    Stmt* default_body = nullptr;
    Stmt* current = stmt;
    while (current && current->kind == AstKind::StmtIf) {
        branches.push_back({current->if_stmt.condition, current->if_stmt.then_branch});
        if (current->if_stmt.else_branch &&
            current->if_stmt.else_branch->kind == AstKind::StmtIf) {
            current = current->if_stmt.else_branch;
        } else {
            default_body = current->if_stmt.else_branch;
            break;
        }
    }

    // 1. Collect variables assigned in any branch ONCE (replaces N separate walks)
    Vector<StringView> all_modified;
    for (auto& branch : branches) {
        collect_assigned_vars(branch.body, all_modified);
    }
    if (default_body) collect_assigned_vars(default_body, all_modified);

    // 2. Create merge block with parameters for phi vars
    IRBlock* merge_block = create_block("endif");
    Vector<PhiInfo> phi_info = make_merge_phis(merge_block, all_modified);

    // 3. Save variable + move state ONCE before any branch
    ScopeSnapshot saved = snapshot_scopes(/*with_move_state=*/true);

    // 4. Create body blocks for each branch + optional default
    Vector<IRBlock*> body_blocks;
    for (u32 i = 0; i < branches.size(); i++) {
        body_blocks.push_back(create_block("then"));
    }
    IRBlock* default_block = default_body ? create_block("else") : nullptr;

    // 5. Generate comparison chain: evaluate each condition, branch to body or next check
    for (u32 i = 0; i < branches.size(); i++) {
        ValueId cond = gen_expr(branches[i].condition);

        // Determine fallthrough target
        IRBlock* fallthrough_block = nullptr;
        if (i + 1 < branches.size()) {
            fallthrough_block = create_block("elif");
        } else if (default_block) {
            fallthrough_block = default_block;
        } else {
            fallthrough_block = merge_block;
        }

        // Branch: if condition true, go to body, else check next
        if (fallthrough_block == merge_block) {
            finish_block_branch(cond, body_blocks[i]->id, fallthrough_block->id,
                                {}, phi_original_args(phi_info));
        } else {
            finish_block_branch(cond, body_blocks[i]->id, fallthrough_block->id);
        }

        // Set next check block as current if there are more branches
        if (i + 1 < branches.size()) {
            set_current_block(fallthrough_block);
        }
    }

    // 6. Generate branch bodies (each sees the pre-chain state). When a
    // default exists, every path into the merge goes through some branch, so
    // the merge must adopt a surviving branch's post-state — track the last
    // one. (Surviving branches agree by construction: semantic forbids
    // divergent moves on non-phi vars. Restoring the PRE-chain state instead
    // would resurrect is_moved=false for a local every surviving path
    // actually moved — a scope-exit double-free when the last branch
    // terminates without moving it.)
    ScopeSnapshot post_surviving;
    bool any_surviving = false;
    auto gen_chain_body = [&](IRBlock* block, Stmt* body) {
        restore_scopes(saved);
        set_current_block(block);
        gen_stmt(body);
        bool terminated = !m_current_block
            || m_current_block->terminator.kind != TerminatorKind::None;
        goto_merge_if_open(merge_block, phi_info);
        if (!terminated && default_block) {
            post_surviving = snapshot_scopes(/*with_move_state=*/true);
            any_surviving = true;
        }
    };
    for (u32 i = 0; i < branches.size(); i++) {
        gen_chain_body(body_blocks[i], branches[i].body);
    }
    if (default_block) {
        gen_chain_body(default_block, default_body);
    }

    // 7. Pick the merge-block state (termination-aware, mirroring gen_if_stmt):
    //   - no default: the all-conditions-false fall-through edge reaches merge
    //     carrying the pre-chain state → restore pre-chain SSA bindings (phi
    //     vars are rebound below, so this only affects non-phi vars, which
    //     must agree across surviving paths by construction). is_moved is left
    //     as-is: for legal programs it already equals the pre-chain state,
    //     since the fall-through path survives and surviving paths must agree.
    //   - default present: adopt the last surviving branch's post-state
    //     (scopes AND is_moved). If every branch terminated, the merge is
    //     unreachable — restore pre-chain bindings for determinism.
    if (default_block && any_surviving) {
        restore_scopes_move(std::move(post_surviving));
    } else {
        restore_scopes(saved, /*restore_move_state=*/false);
    }

    set_current_block(merge_block);
    bind_merge_phis(phi_info);
}

void IRBuilder::gen_while_stmt(Stmt* stmt) {
    WhileStmt& ws = stmt->while_stmt;

    // 1. Collect variables assigned in the loop body
    Vector<StringView> modified_vars;
    collect_assigned_vars(ws.body, modified_vars);

    // 2. Create blocks
    IRBlock* header_block = create_block("while");
    IRBlock* body_block = create_block("body");
    IRBlock* exit_block = create_block("endwhile");

    // 3. Create block params for modified vars that exist before the loop
    Vector<LoopVarInfo> loop_vars;
    Vector<BlockArgPair> initial_args;
    for (const auto& name : modified_vars) {
        LocalVar* lv = find_local(name);
        if (lv && lv->value.is_valid()) {
            ValueId param = m_current_func->new_value();
            header_block->params.push_back({param, lv->type, name});
            loop_vars.push_back({name, lv->type, param, lv->value});
            initial_args.push_back({lv->value});
        }
    }

    // 4. Jump to header with initial values
    finish_block_goto(header_block->id, alloc_span(initial_args));

    // 5. In header, bind locals to block params
    set_current_block(header_block);
    for (const auto& lv : loop_vars) {
        define_local(lv.name, lv.header_param, lv.type);
    }

    // 6. Condition and branch
    ValueId cond = gen_expr(ws.condition);
    finish_block_branch(cond, body_block->id, exit_block->id);

    // 7. Push loop info for break/continue
    u32 while_scope_depth = static_cast<u32>(m_local_scopes.size());
    m_loop_stack.push_back({header_block, exit_block, header_block, loop_vars, while_scope_depth});

    // 8. Generate body
    set_current_block(body_block);
    gen_stmt(ws.body);

    // 9. Back edge with updated values
    if (m_current_block && m_current_block->terminator.kind == TerminatorKind::None) {
        Span<BlockArgPair> back_args = make_loop_args(m_loop_stack.back().loop_vars);
        finish_block_goto(header_block->id, back_args);
    }

    // Save the loop vars before popping (need them for exit block)
    Vector<LoopVarInfo> saved_loop_vars = m_loop_stack.back().loop_vars;  // copy, not move
    m_loop_stack.pop_back();

    // 10. Exit block - use header params as final values
    set_current_block(exit_block);
    for (const auto& slv : saved_loop_vars) {
        define_local(slv.name, slv.header_param, slv.type);
    }
}

void IRBuilder::gen_for_stmt(Stmt* stmt) {
    ForStmt& fs = stmt->for_stmt;

    push_scope();

    // 1. Initialize (creates the loop variable in scope)
    if (fs.initializer) {
        gen_decl(fs.initializer);
    }

    // 2. Collect variables assigned in the loop body AND increment
    Vector<StringView> modified_vars;
    collect_assigned_vars(fs.body, modified_vars);
    collect_assigned_vars_expr(fs.increment, modified_vars);

    // 3. Create blocks
    IRBlock* header_block = create_block("for");
    IRBlock* body_block = create_block("forbody");
    IRBlock* incr_block = create_block("forinc");
    IRBlock* exit_block = create_block("endfor");

    // 4. Create block params on header for modified vars that exist before the loop
    Vector<LoopVarInfo> loop_vars;
    Vector<BlockArgPair> initial_args;
    for (const auto& name : modified_vars) {
        LocalVar* lv = find_local(name);
        if (lv && lv->value.is_valid()) {
            ValueId param = m_current_func->new_value();
            header_block->params.push_back({param, lv->type, name});
            loop_vars.push_back({name, lv->type, param, lv->value});
            initial_args.push_back({lv->value});
        }
    }

    // 5. Jump to header with initial values
    finish_block_goto(header_block->id, alloc_span(initial_args));

    // 6. In header, bind locals to block params
    set_current_block(header_block);
    for (const auto& lv : loop_vars) {
        define_local(lv.name, lv.header_param, lv.type);
    }

    // 7. Condition and branch
    if (fs.condition) {
        ValueId cond = gen_expr(fs.condition);
        finish_block_branch(cond, body_block->id, exit_block->id);
    } else {
        // No condition = infinite loop (until break)
        finish_block_goto(body_block->id);
    }

    // 8. Push loop info for break/continue
    // continue goes to increment block, but we need to pass args to header after increment
    u32 for_scope_depth = static_cast<u32>(m_local_scopes.size());
    m_loop_stack.push_back({header_block, exit_block, incr_block, loop_vars, for_scope_depth});

    // 9. Generate body
    set_current_block(body_block);
    gen_stmt(fs.body);
    if (m_current_block && m_current_block->terminator.kind == TerminatorKind::None) {
        finish_block_goto(incr_block->id);
    }

    // 10. Increment block - generate increment, then jump back to header with args
    set_current_block(incr_block);
    if (fs.increment) {
        gen_expr(fs.increment);
    }
    Span<BlockArgPair> back_args = make_loop_args(m_loop_stack.back().loop_vars);
    finish_block_goto(header_block->id, back_args);

    // Save the loop vars before popping (need them for exit block)
    Vector<LoopVarInfo> saved_loop_vars = m_loop_stack.back().loop_vars;  // copy, not move
    m_loop_stack.pop_back();

    pop_scope();

    // 11. Exit block - use header params as final values
    set_current_block(exit_block);
    for (const auto& slv : saved_loop_vars) {
        define_local(slv.name, slv.header_param, slv.type);
    }
}

void IRBuilder::gen_return_stmt(Stmt* stmt) {
    ReturnStmt& rs = stmt->return_stmt;

    if (rs.value) {
        ValueId val = gen_expr(rs.value);

        // Consume noncopyable temporaries (ownership transfers to caller)
        if (rs.value->resolved_type && rs.value->resolved_type->noncopyable()) {
            consume_temp_noncopyable(val);
        }

        // If returning an owned identifier, mark it as moved (don't destroy what we're returning).
        // Pass null_ssa/nullify_record = false: the return value register may be the same as
        // the source's, and nulling/Nullifying would corrupt the return. The is_moved flag
        // alone prevents normal-path cleanup from freeing the returned value.
        Type* return_type = rs.value->resolved_type;
        if (rs.value->kind == AstKind::ExprIdentifier && return_type && return_type->noncopyable()) {
            mark_moved_from(rs.value->identifier.name, /*null_ssa=*/false,
                            /*nullify_record=*/false);
        } else if (return_type && return_type->kind == TypeKind::Ref) {
            // Counting convention: a ref return hands off exactly one borrow
            // count for the caller to adopt. How we produce that one count
            // depends on the returned expression:
            OwnedLocalInfo* ref_local =
                rs.value->kind == AstKind::ExprIdentifier
                    ? m_ownership.find_by_name(rs.value->identifier.name) : nullptr;
            if (ref_local && ref_local->kind == OwnedKind::RefBorrow) {
                // Ref *local*: hand off by marking it moved so emit_scope_cleanup
                // skips its normal-path RefDec — its create-inc survives as the
                // handed-off count. If the borrowed owner is a local, its RAII
                // drop below now sees the live borrow and traps (Finding 2).
                mark_moved_from(rs.value->identifier.name, /*null_ssa=*/false,
                                /*nullify_record=*/false);
            } else if (!is_ref_handoff_source(rs.value)) {
                // A ref *param* identifier, or a fresh ref (field / subscript /
                // `ref x`): these carry no net count for the caller yet, so
                // increment to produce the one handed-off count. (A ref param's
                // entry-inc is offset by its return-time RefDec, so this inc is
                // what survives.) A call result already carries one — untouched.
                // `return self` is a promotion: the inc is heap-gated.
                emit_ref_borrow_inc(val, rs.value);
            }
        }
        // String return: hand off exactly one owned count to the caller (finding
        // 9b). Adopt a fresh producer temp, or retain an existing owner — whose
        // own count is then released by emit_scope_cleanup below, leaving the one
        // handed-off count for the caller to adopt. Mirrors the ref-return handoff.
        if (return_type && return_type->kind == TypeKind::String) {
            consume_or_retain_string(val, return_type, /*adopted_by_variable=*/false);
        }

        // `return o.field`: null the moved-out field before scope cleanup destroys
        // the root (val already read its value above).
        nullify_moved_field_source(rs.value);

        // Emit cleanup for all scopes (return exits entire function)
        emit_scope_cleanup(1);

        // Check if returning a large struct
        if (m_current_func->returns_large_struct()) {
            // Large struct: copy to hidden output pointer (last parameter)
            ValueId output_ptr = m_current_func->params.back().value;
            u32 slot_count = m_current_func->return_type->struct_info.slot_count;
            emit_struct_copy(output_ptr, val, slot_count);
            finish_block_return(ValueId::invalid());  // Return void
        } else {
            finish_block_return(val);
        }
    } else {
        // Emit cleanup for all scopes before void return
        emit_scope_cleanup(1);
        finish_block_return(ValueId::invalid());
    }
}

void IRBuilder::gen_break_stmt(Stmt*) {
    if (m_loop_stack.empty()) return;  // Should be caught by semantic analysis

    LoopInfo& loop = m_loop_stack.back();

    // Emit cleanup for scopes inside the loop
    emit_scope_cleanup(loop.scope_depth + 1);

    // Exit block doesn't have parameters - it uses header params
    finish_block_goto(loop.exit_block->id);
}

void IRBuilder::gen_continue_stmt(Stmt*) {
    if (m_loop_stack.empty()) return;  // Should be caught by semantic analysis

    LoopInfo& loop = m_loop_stack.back();

    // Emit cleanup for scopes inside the loop body
    emit_scope_cleanup(loop.scope_depth + 1);

    // For while loops: continue_block == header_block, needs args
    // For for loops: continue_block == incr_block, no args needed
    if (loop.continue_block == loop.header_block) {
        // While loop - pass current values to header
        Span<BlockArgPair> args = make_loop_args(loop.loop_vars);
        finish_block_goto(loop.continue_block->id, args);
    } else {
        // For loop - just jump to increment block (no args)
        finish_block_goto(loop.continue_block->id);
    }
}

void IRBuilder::gen_delete_stmt(Stmt* stmt) {
    DeleteStmt& ds = stmt->delete_stmt;
    ValueId val = gen_expr(ds.expr);

    // Get the struct type from the expression
    Type* expr_type = ds.expr->resolved_type;
    Type* struct_type = nullptr;
    if (expr_type && expr_type->kind == TypeKind::Uniq) {
        struct_type = expr_type->ref_info.inner_type;
    }

    // Check if there's a destructor to call
    if (struct_type && struct_type->is_struct()) {
        StructTypeInfo& struct_type_info = struct_type->struct_info;

        // Look up destructor by name
        const DestructorInfo* dtor = nullptr;
        for (const auto& d : struct_type_info.destructors) {
            if (d.name == ds.destructor_name) {
                dtor = &d;
                break;
            }
        }

        if (dtor) {
            // Call the destructor: 'self' pointer + destructor arguments, with
            // the shared move-aware lowering for noncopyable args.
            StringView dtor_name = mangle_destructor(struct_type_info.name, ds.destructor_name);
            Span<ValueId> dtor_args = lower_simple_args(ds.arguments);
            emit_call(dtor_name, prepend_self(val, dtor_args), m_types.void_type());
            mark_simple_args_moved(ds.arguments);
        } else if (ds.destructor_name.empty()) {
            // Check for default destructor even if not explicitly requested
            for (const auto& d : struct_type_info.destructors) {
                if (d.name.empty()) {
                    // Found default destructor - call it
                    StringView dtor_name = mangle_destructor(struct_type_info.name);
                    Vector<ValueId> call_args;
                    call_args.push_back(val);
                    emit_call(dtor_name, alloc_span(call_args), m_types.void_type());
                    break;
                }
            }
        }
    }

    // Free the memory
    emit_delete(val, m_types.void_type());

    // Mark the variable as moved so scope cleanup doesn't double-destroy. The memory
    // was just explicitly Delete'd, so leave the SSA register alone (null_ssa=false);
    // the Nullify annotation still ends the cleanup record scope.
    if (ds.expr->kind == AstKind::ExprIdentifier) {
        StringView del_name = ds.expr->identifier.name;
        // A deleted module-level global (not shadowed by a local of the same name)
        // must have its slot nulled so __module_shutdown's null-guarded Delete
        // no-ops instead of double-freeing the already-deleted object (finding 8b).
        // Only pointer-holding globals (uniq/List/Map/Coro) carry an owning pointer
        // in the slot; the Delete above already ran (a refused free would have
        // halted the run before here).
        auto git = find_local(del_name) ? m_global_indices.end()
                                        : m_global_indices.find(del_name);
        if (git != m_global_indices.end()) {
            // `delete` only applies to `uniq` (a `uniq` global's slot holds the
            // owning pointer); a null store makes shutdown's Delete a no-op.
            const IRGlobal& g = m_module->globals[git->second];
            if (g.type && g.type->kind == TypeKind::Uniq) {
                ValueId gaddr = emit_global_addr(g.slot_offset, g.type);
                ValueId null_val = emit_const_null();
                emit_store_ptr(gaddr, null_val, g.slot_count, g.type);
            }
        } else {
            mark_moved_from(del_name, /*null_ssa=*/false);
        }
    }
}

void IRBuilder::gen_when_stmt(Stmt* stmt) {
    WhenStmt& ws = stmt->when_stmt;

    // 1. Collect variables assigned in any case or else body
    Vector<StringView> modified_in_cases;
    for (auto& wc : ws.cases) {
        for (auto* d : wc.body) {
            if (d && d->kind >= AstKind::StmtExpr && d->kind <= AstKind::StmtYield) {
                collect_assigned_vars(&d->stmt, modified_in_cases);
            }
        }
    }
    for (auto* d : ws.else_body) {
        if (d && d->kind >= AstKind::StmtExpr && d->kind <= AstKind::StmtYield) {
            collect_assigned_vars(&d->stmt, modified_in_cases);
        }
    }

    // 2. Evaluate discriminant
    ValueId discrim = gen_expr(ws.discriminant);
    Type* discrim_type = ws.discriminant->resolved_type;

    // 3. Create merge block with phi params for modified vars that exist
    // before the when
    IRBlock* merge_block = create_block("endwhen");
    Vector<PhiInfo> phi_info = make_merge_phis(merge_block, modified_in_cases);

    // 4. Save variable state before any case (so all cases see original
    // values). Scopes only: is_moved is intentionally not snapshotted here —
    // see the merge-state note below and the rationale in gen_try_stmt.
    ScopeSnapshot saved = snapshot_scopes(/*with_move_state=*/false);

    // Track case blocks and their corresponding case names for code gen
    struct CaseInfo {
        IRBlock* body_block;
        WhenCase* wc;
    };
    Vector<CaseInfo> case_infos;

    // Create body blocks for each case
    for (auto& wc : ws.cases) {
        IRBlock* body_block = create_block("when_case");
        case_infos.push_back({body_block, &wc});
    }

    // Create else block if there's an else clause. For an exhaustive no-else
    // `when` (semantic analysis proved the cases cover every variant), the
    // discriminant always matches a case, so the unmatched fall-through is
    // unreachable — synthesize a trapping "else" and route the last case's
    // no-match edge there instead of re-joining the merge with pre-when values.
    IRBlock* else_block = nullptr;
    bool else_is_trap = false;
    if (ws.else_body.size() > 0) {
        else_block = create_block("when_else");
    } else if (ws.is_exhaustive) {
        else_block = create_block("when_unreachable");
        else_is_trap = true;
    }

    // 6. Generate the comparison chain
    for (u32 i = 0; i < ws.cases.size(); i++) {
        WhenCase& wc = ws.cases[i];
        CaseInfo& ci = case_infos[i];

        // Build condition: discriminant == case_name1 || discriminant == case_name2 || ...
        ValueId case_cond = ValueId::invalid();
        for (const auto& case_name : wc.case_names) {

            // Look up the case's value in the DISCRIMINANT enum's own variant
            // table. The flat symbol namespace would resolve a same-named
            // variant of whichever enum was defined last — wrong constant.
            i64 variant_value = 0;
            if (discrim_type && discrim_type->is_enum()) {
                if (const EnumVariantInfo* variant =
                        discrim_type->enum_info.find_variant(case_name)) {
                    variant_value = variant->value;
                }
            }

            // Generate comparison: discriminant == variant_value
            ValueId variant_val = emit_const_int(variant_value, discrim_type);
            ValueId cmp = emit_binary(IROp::EqI, discrim, variant_val, m_types.bool_type());

            // OR with previous conditions
            if (!case_cond.is_valid()) {
                case_cond = cmp;
            } else {
                case_cond = emit_binary(IROp::Or, case_cond, cmp, m_types.bool_type());
            }
        }

        // Determine the fallthrough target
        IRBlock* fallthrough_block = nullptr;
        if (i + 1 < ws.cases.size()) {
            // More cases to check
            fallthrough_block = create_block("when_next");
        } else if (else_block) {
            // No more cases, go to else
            fallthrough_block = else_block;
        } else {
            // No more cases and no else, go to merge
            fallthrough_block = merge_block;
        }

        // Branch: if condition matches, go to case body, else check next
        // When falling through to merge, pass original phi values
        if (fallthrough_block == merge_block) {
            finish_block_branch(case_cond, ci.body_block->id, fallthrough_block->id,
                                {}, phi_original_args(phi_info));
        } else {
            finish_block_branch(case_cond, ci.body_block->id, fallthrough_block->id);
        }

        // Set next block as current if there are more cases
        if (i + 1 < ws.cases.size()) {
            set_current_block(fallthrough_block);
        }
    }

    // 7. Generate case bodies (each sees the pre-when scope state)
    for (u32 i = 0; i < ws.cases.size(); i++) {
        CaseInfo& ci = case_infos[i];

        restore_scopes(saved);
        set_current_block(ci.body_block);
        push_scope();
        for (auto* d : ci.wc->body) {
            gen_decl(d);
        }
        pop_scope();
        goto_merge_if_open(merge_block, phi_info);
    }

    // 8. Generate else body if present
    if (else_block) {
        restore_scopes(saved);
        set_current_block(else_block);
        if (else_is_trap) {
            // Exhaustive no-else: this edge is provably unreachable. Trap so the
            // fall-through never re-joins the merge with stale pre-when values —
            // agreeing with the semantic model, which drops the survivor path.
            finish_block_unreachable();
        } else {
            push_scope();
            for (auto* d : ws.else_body) {
                gen_decl(d);
            }
            pop_scope();
            goto_merge_if_open(merge_block, phi_info);
        }
    }

    // 9. Continue from merge block, bind phi results.
    // Restore pre-when local-scope SSA bindings: the last case body's
    // mutations (e.g. a struct-literal nullify-replacing a moved local to
    // nil) would otherwise leak into post-when code, even when that path
    // came via the unmatched-fallthrough or a different case body. Phi vars
    // are immediately rebound from merge_param below, so this only affects
    // non-phi vars — which by construction must agree across all surviving
    // paths (semantic forbids divergent moves on non-phi vars). is_moved is
    // intentionally left alone, mirroring the rationale in gen_try_stmt.
    restore_scopes(saved);
    set_current_block(merge_block);
    bind_merge_phis(phi_info);
}

void IRBuilder::gen_throw_stmt(Stmt* stmt) {
    ThrowStmt& ts = stmt->throw_stmt;

    ValueId exception_val = gen_expr(ts.expr);
    Type* expr_type = ts.expr->resolved_type;

    // If the expression is a value type (struct on stack), heap-allocate it
    // Struct literal expressions with is_heap=false are stack-allocated
    Type* base_type = expr_type->base_type();
    if (base_type->is_struct() && !expr_type->is_reference()) {
        // Wrap in heap allocation via New
        Span<ValueId> empty_args = {};
        Type* uniq_type = m_types.uniq_type(base_type);
        ValueId heap_ptr = emit_new(base_type->struct_info.name, empty_args, uniq_type);

        // Copy struct data to heap object
        u32 slot_count = base_type->struct_info.slot_count;
        emit_struct_copy(heap_ptr, exception_val, slot_count);
        exception_val = heap_ptr;
    }

    // Emit Throw instruction
    IRInst* inst = emit_inst(IROp::Throw, m_types.void_type());
    if (inst) {
        inst->unary = exception_val;
    }

    // Code after throw is unreachable
    finish_block_unreachable();
}

void IRBuilder::gen_yield_stmt(Stmt* stmt) {
    YieldStmt& ys = stmt->yield_stmt;

    // Evaluate the yield expression
    ValueId yield_val = gen_expr(ys.value);

    // Emit Yield instruction (block terminator, like Throw)
    IRInst* inst = emit_inst(IROp::Yield, m_current_func->coro_yield_type);
    if (inst) {
        inst->unary = yield_val;
    }

    // Collect all currently live local variables to pass as block arguments
    // to the resume block
    Vector<StringView> live_names;
    Vector<ValueId> live_values;
    Vector<Type*> live_types;
    for (auto& scope : m_local_scopes) {
        for (auto& [name, local] : scope) {
            live_names.push_back(name);
            live_values.push_back(local.value);
            live_types.push_back(local.type);
        }
    }

    // Create a resume block with block parameters for each live local
    IRBlock* resume_block = create_block("coro.resume");
    for (u32 i = 0; i < live_names.size(); i++) {
        BlockParam param;
        param.value = m_current_func->new_value();
        param.type = live_types[i];
        param.name = live_names[i];
        resume_block->params.push_back(param);
    }

    // Build block args to pass current values to the resume block
    Span<BlockArgPair> args = alloc_span<BlockArgPair>(static_cast<u32>(live_values.size()));
    for (u32 i = 0; i < live_values.size(); i++) {
        args[i].value = live_values[i];
    }

    // Finish current block with Goto to the resume block
    finish_block_goto(resume_block->id, args);

    // Switch to the resume block
    set_current_block(resume_block);

    // Update locals to point to the new block parameters
    for (u32 i = 0; i < live_names.size(); i++) {
        define_local(live_names[i], resume_block->params[i].value, live_types[i]);
    }
}

void IRBuilder::gen_try_stmt(Stmt* stmt) {
    TryStmt& ts = stmt->try_stmt;

    // Collect variables modified in try/catch/finally bodies for phi nodes
    Vector<StringView> modified_vars;
    collect_assigned_vars(ts.try_body, modified_vars);
    for (u32 i = 0; i < ts.catches.size(); i++) {
        collect_assigned_vars(ts.catches[i].body, modified_vars);
    }
    if (ts.finally_body) {
        collect_assigned_vars(ts.finally_body, modified_vars);
    }

    // Create after block with phi params (deduped, filtered to existing vars)
    IRBlock* after_block = create_block("try.after");
    Vector<PhiInfo> phi_info = make_merge_phis(after_block, modified_vars);

    // Snapshot pre-try local-scope SSA bindings. Each catch block must see the
    // locals as they were *before* the try body ran — otherwise the catch sees
    // rebindings (e.g. `r = throwing_call()` rebinds r to the call result's
    // SSA value, even when the call threw and never produced one), and the
    // after-block phi feeds those undefined values out.
    //
    // We deliberately do NOT snapshot the owned locals' is_moved here. That flag
    // governs IR-builder bookkeeping for runtime cleanup (whether to emit a
    // destroy at scope exit / before reassignment). Rolling it back would
    // re-enable the implicit-destroy preamble of `r = uniq T()` inside catch
    // for a uniq local already consumed in the try body, double-freeing the
    // already-dead slab slot. Use-after-move in catch is the semantic
    // analyzer's job to reject, not the IR builder's to repair.
    ScopeSnapshot pre_try = snapshot_scopes(/*with_move_state=*/false);

    // Record the first block of the try body
    IRBlock* try_entry_block = create_block("try.body");
    finish_block_goto(try_entry_block->id);
    set_current_block(try_entry_block);

    // Generate try body
    push_scope();
    u32 try_body_start_idx = static_cast<u32>(m_current_func->blocks.size()) - 1;
    gen_stmt(ts.try_body);
    pop_scope();

    // Record the last block of try body: all blocks created during try body generation
    // have IDs between try_entry and here. Catch blocks haven't been created yet,
    // so the last block in the function is the last try body block.
    // This ensures the handler covers ALL try body blocks, including resume blocks
    // created by yields inside the try body.
    BlockId try_exit_block_id = BlockId{static_cast<u32>(m_current_func->blocks.size()) - 1};

    // Capture every IR block created during the try body. Handler lookup runs
    // after RPO reorder, which can scatter these IDs (e.g. a loop body inside
    // the try ends up laid out *after* the try's fall-through), so lowering
    // needs the full set to emit the correct per-range handler table.
    Vector<BlockId> try_body_block_ids;
    try_body_block_ids.reserve(static_cast<u32>(m_current_func->blocks.size()) - try_body_start_idx);
    for (u32 b = try_body_start_idx; b < m_current_func->blocks.size(); b++) {
        try_body_block_ids.push_back(m_current_func->blocks[b]->id);
    }

    // If try body didn't terminate (no throw/return/break), jump to after block
    // (or finally block if present)
    if (m_current_block && m_current_block->terminator.kind == TerminatorKind::None) {
        if (ts.finally_body) {
            // Generate inline finally for normal exit
            push_scope();
            gen_stmt(ts.finally_body);
            pop_scope();
        }
        if (m_current_block && m_current_block->terminator.kind == TerminatorKind::None) {
            finish_block_goto(after_block->id, phi_current_args(phi_info));
        }
    }

    // Generate catch handler blocks
    for (u32 i = 0; i < ts.catches.size(); i++) {
        CatchClause& clause = ts.catches[i];

        IRBlock* catch_block = create_block("catch");

        // Add block parameter for exception pointer
        BlockParam exc_param;
        exc_param.value = m_current_func->new_value();
        if (clause.resolved_type) {
            exc_param.type = m_types.ref_type(clause.resolved_type);
        } else {
            exc_param.type = m_types.exception_ref_type();
        }
        exc_param.name = clause.var_name;
        catch_block->params.push_back(exc_param);

        set_current_block(catch_block);

        // Restore pre-try state: the catch path begins where the throw aborted,
        // so any rebindings/moves the try body did must not be visible here.
        restore_scopes(pre_try);

        // Define catch variable in scope
        push_scope();
        define_local(clause.var_name, exc_param.value, exc_param.type);

        // The caught exception object is heap-allocated (gen_throw_stmt) and, on
        // the handled path, the unwinder hands its pointer to this catch without
        // freeing it — so the catch scope owns it and must free it on every exit
        // (finding 9a). Register it as an owned local so the ordinary scope-cleanup
        // machinery frees it exactly once on normal fall-through, return, break,
        // continue, and a *new* throw unwinding out of the catch. A re-throw
        // (`throw e`, or a nested `throw` while this is in flight) is handled by the
        // in-flight guard in object_free, which refuses to free the object the
        // unwind machinery still owns — so no move-state bookkeeping is needed here.
        // A typed catch frees as `uniq E` (runs E's destructor); a catch-all has no
        // compile-time concrete type, so it frees the memory type-erased.
        Type* owned_exc_type = clause.resolved_type
            ? m_types.uniq_type(clause.resolved_type)
            : m_types.exception_ref_type();
        u32 catch_scope_depth = static_cast<u32>(m_local_scopes.size());
        BlockId catch_owned_block = m_current_block ? m_current_block->id : BlockId::invalid();
        m_ownership.track({clause.var_name, owned_exc_type, catch_scope_depth,
                           false, false, catch_owned_block, exc_param.value,
                           OwnedKind::Owned});

        gen_stmt(clause.body);
        pop_scope();

        // If catch body didn't terminate, jump to after
        if (m_current_block && m_current_block->terminator.kind == TerminatorKind::None) {
            if (ts.finally_body) {
                // Generate inline finally for catch exit
                push_scope();
                gen_stmt(ts.finally_body);
                pop_scope();
            }
            if (m_current_block && m_current_block->terminator.kind == TerminatorKind::None) {
                finish_block_goto(after_block->id, phi_current_args(phi_info));
            }
        }

        // Record exception handler
        IRExceptionHandler handler;
        handler.try_entry = try_entry_block->id;
        handler.try_exit = try_exit_block_id;
        handler.handler_block = catch_block->id;
        handler.type_id = 0;  // Will be filled by lowering
        handler.type_name = StringView(nullptr, 0);  // Catch-all by default
        if (clause.resolved_type) {
            handler.type_name = clause.resolved_type->struct_info.name;
        }
        for (BlockId bid : try_body_block_ids) handler.try_body_blocks.push_back(bid);
        m_current_func->exception_handlers.push_back(handler);
    }

    // If there's a finally block, add a catch-all handler that runs the finally
    // body and re-throws. This is registered AFTER all typed catches so they're
    // tried first. The finally handler catches everything else.
    if (ts.finally_body) {
        IRBlock* finally_catch_block = create_block("finally.catch");

        // Add block parameter for exception pointer (opaque)
        BlockParam exc_param;
        exc_param.value = m_current_func->new_value();
        exc_param.type = m_types.exception_ref_type();
        exc_param.name = "__exc"_sv;
        finally_catch_block->params.push_back(exc_param);

        set_current_block(finally_catch_block);

        // Restore pre-try state for the same reason as the typed catches.
        restore_scopes(pre_try);

        // Execute finally body
        push_scope();
        gen_stmt(ts.finally_body);
        pop_scope();

        // Re-throw the exception
        if (m_current_block && m_current_block->terminator.kind == TerminatorKind::None) {
            IRInst* inst = emit_inst(IROp::Throw, m_types.void_type());
            if (inst) {
                inst->unary = exc_param.value;
            }
            finish_block_unreachable();
        }

        // Register as catch-all handler (type_id=0, no type_name)
        IRExceptionHandler handler;
        handler.try_entry = try_entry_block->id;
        handler.try_exit = try_exit_block_id;
        handler.handler_block = finally_catch_block->id;
        handler.type_id = 0;
        handler.type_name = StringView(nullptr, 0);
        for (BlockId bid : try_body_block_ids) handler.try_body_blocks.push_back(bid);
        m_current_func->exception_handlers.push_back(handler);
    }

    // Continue after try/catch - bind phi variables to merge params
    set_current_block(after_block);
    bind_merge_phis(phi_info);
}

// Expression generation

void IRBuilder::gen_decl(Decl* decl) {
    if (!decl) return;

    // Stamp this decl's source line onto subsequent emit_inst calls.
    if (decl->loc.line != 0) m_current_source_line = decl->loc.line;

    switch (decl->kind) {
        case AstKind::DeclVar:
            gen_var_decl(decl);
            break;
        default:
            // Statement wrapped in declaration
            if (decl->kind >= AstKind::StmtExpr && decl->kind <= AstKind::StmtYield) {
                gen_stmt(&decl->stmt);
            }
            break;
    }
}

void IRBuilder::gen_var_decl(Decl* decl) {
    VarDecl& var_decl = decl->var_decl;

    // Use the resolved type from semantic analysis
    Type* type = var_decl.resolved_type;
    if (!type) {
        type = m_types.error_type();
    }

    ValueId value;

    // Check if this is a struct type - needs stack allocation
    if (type->is_struct()) {
        // If the initializer is a struct literal or a call, it already produces
        // fresh storage (the literal stack-allocs; the call either writes to a
        // hidden output slot for large structs or materializes a small-struct
        // return through the return-unpack path). No copy needed — aliasing is
        // impossible.
        bool init_produces_fresh = var_decl.initializer &&
            (var_decl.initializer->kind == AstKind::ExprStructLiteral ||
             var_decl.initializer->kind == AstKind::ExprCall);
        if (init_produces_fresh) {
            value = gen_expr(var_decl.initializer);
        } else if (var_decl.initializer) {
            ValueId src = gen_expr(var_decl.initializer);
            if (type->is_copy()) {
                // The rvalue is a pointer into storage owned by some other entity
                // (another local, a struct field, a list/map element). Binding
                // `value` directly to `src` would make this variable alias that
                // storage — mutations through the new variable would be visible
                // in the source, and the pointer would dangle if the source
                // storage is later invalidated (e.g. a list realloc). Allocate
                // fresh storage and copy for copyable structs. Noncopyable
                // types keep the direct rebind; semantic analysis has already
                // validated the move.
                u32 slot_count = type->struct_info.slot_count;
                value = emit_stack_alloc(slot_count, type);
                emit_struct_copy(value, src, slot_count);
            } else {
                value = src;
            }
        } else {
            // No initializer - allocate stack space for the struct (zero-initialized by VM)
            u32 slot_count = type->struct_info.slot_count;
            value = emit_stack_alloc(slot_count, type);
        }
    } else {
        // Non-struct types: use register storage
        if (var_decl.initializer) {
            value = gen_expr(var_decl.initializer);
            // Wrap uniq/ref → weak conversion
            value = maybe_wrap_weak(value, var_decl.initializer->resolved_type, type, var_decl.initializer);
        } else {
            // Default initialization
            value = emit_const_null();
        }
    }

    define_local(var_decl.name, value, type);

    // Track owned locals for implicit destruction (uniq refs and value structs with destructors)
    if (type && type->noncopyable()) {
        // If the initializer was a temporary, consume it (variable takes over tracking).
        // Pass adopted_by_variable=true: the variable's cleanup record handles cleanup,
        // so no Nullify annotation is needed for the temp.
        consume_temp_noncopyable(value, true);

        u32 scope_depth = static_cast<u32>(m_local_scopes.size());
        BlockId current_block_id = m_current_block ? m_current_block->id : BlockId::invalid();
        m_ownership.track({var_decl.name, type, scope_depth, false, false, current_block_id, value});

        // Mark the source variable as moved when initializing from an identifier
        if (var_decl.initializer && var_decl.initializer->kind == AstKind::ExprIdentifier) {
            mark_moved_from(var_decl.initializer->identifier.name);
        }
        // `var x = o.field`: null the moved-out field in the root.
        nullify_moved_field_source(var_decl.initializer);
    } else if (type && type->kind == TypeKind::Ref) {
        // Ref local: a counted borrow (constraint-reference model), tracked as a
        // RefBorrow so it is decremented on every exit path (scope exit, return,
        // break, continue, exception unwind) via the cleanup machinery.
        //
        // Counting convention: every `ref`-returning call hands off exactly one
        // borrow count to its caller (see gen_return_stmt), so binding a call
        // result *adopts* that count rather than incrementing again. Binding any
        // other source (a uniq / ref identifier, a borrowed subscript, `ref x`)
        // is a fresh borrow alongside the still-live source, so it increments.
        if (!is_ref_handoff_source(var_decl.initializer)) {
            // `var r: ref T = self` is a promotion: the inc is heap-gated.
            emit_ref_borrow_inc(value, var_decl.initializer);
        }
        u32 scope_depth = static_cast<u32>(m_local_scopes.size());
        BlockId current_block_id = m_current_block ? m_current_block->id : BlockId::invalid();
        m_ownership.track({var_decl.name, type, scope_depth, false, false,
                           current_block_id, value, OwnedKind::RefBorrow});
    } else if (type && type->kind == TypeKind::String) {
        // String local: a reference-counted owned value (finding 9b). Adopt a
        // fresh producer temp (count transfers) or retain an existing owner, then
        // track as a StrOwn local so it's released on every exit path.
        consume_or_retain_string(value, type, /*adopted_by_variable=*/true);
        u32 scope_depth = static_cast<u32>(m_local_scopes.size());
        BlockId current_block_id = m_current_block ? m_current_block->id : BlockId::invalid();
        m_ownership.track({var_decl.name, type, scope_depth, false, false,
                           current_block_id, value, OwnedKind::StrOwn});
    }
}

// Variable management

// Public entry points: seed the membership set from `out`'s current contents
// (call sites accumulate across several collect calls — if/else branches, for
// body + increment — and loop-header param creation relies on `out` being
// duplicate-free), then delegate to the _impl recursion, which dedupes through
// the set instead of rescanning `out` per assignment (formerly O(n²)).
void IRBuilder::collect_assigned_vars(Stmt* stmt, Vector<StringView>& out) {
    tsl::robin_map<StringView, bool> seen;
    for (const auto& existing : out) seen[existing] = true;
    collect_assigned_vars_impl(stmt, out, seen);
}

void IRBuilder::collect_assigned_vars_expr(Expr* expr, Vector<StringView>& out) {
    tsl::robin_map<StringView, bool> seen;
    for (const auto& existing : out) seen[existing] = true;
    collect_assigned_vars_expr_impl(expr, out, seen);
}

void IRBuilder::collect_assigned_vars_impl(Stmt* stmt, Vector<StringView>& out,
                                           tsl::robin_map<StringView, bool>& seen) {
    if (!stmt) return;

    switch (stmt->kind) {
        case AstKind::StmtExpr:
            collect_assigned_vars_expr_impl(stmt->expr_stmt.expr, out, seen);
            break;
        case AstKind::StmtBlock: {
            BlockStmt& block = stmt->block;
            for (auto* d : block.declarations) {
                if (!d) continue;
                // Recurse into statements (not var decls - those are new vars)
                if (d->kind >= AstKind::StmtExpr && d->kind <= AstKind::StmtYield) {
                    collect_assigned_vars_impl(&d->stmt, out, seen);
                }
            }
            break;
        }
        case AstKind::StmtIf:
            collect_assigned_vars_impl(stmt->if_stmt.then_branch, out, seen);
            collect_assigned_vars_impl(stmt->if_stmt.else_branch, out, seen);
            break;
        case AstKind::StmtWhile:
            collect_assigned_vars_impl(stmt->while_stmt.body, out, seen);
            break;
        case AstKind::StmtFor:
            collect_assigned_vars_impl(stmt->for_stmt.body, out, seen);
            collect_assigned_vars_expr_impl(stmt->for_stmt.increment, out, seen);
            break;
        case AstKind::StmtWhen: {
            WhenStmt& ws = stmt->when_stmt;
            for (auto& when_case : ws.cases) {
                for (auto* d : when_case.body) {
                    if (d && d->kind >= AstKind::StmtExpr && d->kind <= AstKind::StmtYield) {
                        collect_assigned_vars_impl(&d->stmt, out, seen);
                    }
                }
            }
            for (auto* d : ws.else_body) {
                if (d && d->kind >= AstKind::StmtExpr && d->kind <= AstKind::StmtYield) {
                    collect_assigned_vars_impl(&d->stmt, out, seen);
                }
            }
            break;
        }
        case AstKind::StmtThrow:
            collect_assigned_vars_expr_impl(stmt->throw_stmt.expr, out, seen);
            break;
        case AstKind::StmtYield:
            collect_assigned_vars_expr_impl(stmt->yield_stmt.value, out, seen);
            break;
        case AstKind::StmtTry: {
            TryStmt& ts = stmt->try_stmt;
            collect_assigned_vars_impl(ts.try_body, out, seen);
            for (u32 i = 0; i < ts.catches.size(); i++) {
                collect_assigned_vars_impl(ts.catches[i].body, out, seen);
            }
            if (ts.finally_body) {
                collect_assigned_vars_impl(ts.finally_body, out, seen);
            }
            break;
        }
        default:
            break;
    }
}

void IRBuilder::collect_assigned_vars_expr_impl(Expr* expr, Vector<StringView>& out,
                                                tsl::robin_map<StringView, bool>& seen) {
    if (!expr) return;

    // Record an assigned/written identifier once (first-occurrence order).
    auto add_once = [&](StringView name) {
        auto it = seen.find(name);
        if (it != seen.end()) return;
        seen[name] = true;
        out.push_back(name);
    };

    switch (expr->kind) {
        case AstKind::ExprAssign: {
            // Check if the target is an identifier
            if (expr->assign.target->kind == AstKind::ExprIdentifier) {
                add_once(expr->assign.target->identifier.name);
            }
            // Recurse into value expression (it might have nested assignments)
            collect_assigned_vars_expr_impl(expr->assign.value, out, seen);
            break;
        }
        case AstKind::ExprBinary:
            collect_assigned_vars_expr_impl(expr->binary.left, out, seen);
            collect_assigned_vars_expr_impl(expr->binary.right, out, seen);
            break;
        case AstKind::ExprUnary:
            collect_assigned_vars_expr_impl(expr->unary.operand, out, seen);
            break;
        case AstKind::ExprTernary:
            collect_assigned_vars_expr_impl(expr->ternary.condition, out, seen);
            collect_assigned_vars_expr_impl(expr->ternary.then_expr, out, seen);
            collect_assigned_vars_expr_impl(expr->ternary.else_expr, out, seen);
            break;
        case AstKind::ExprCall:
            collect_assigned_vars_expr_impl(expr->call.callee, out, seen);
            for (auto& arg : expr->call.arguments) {
                collect_assigned_vars_expr_impl(arg.expr, out, seen);
                // An `inout`/`out` argument stores through the caller's slot
                // via the post-call reload (see gen_call_expr's inout_args).
                // Treat it as a write to the identifier so loop variable
                // collection phis the local at the loop header — otherwise
                // the reload redefines `xs` inside the body but the SSA value
                // never makes it back to the header, and post-loop uses read
                // a stale register / trip register allocation.
                if ((arg.modifier == ParamModifier::Inout || arg.modifier == ParamModifier::Out)
                    && arg.expr && arg.expr->kind == AstKind::ExprIdentifier) {
                    add_once(arg.expr->identifier.name);
                }
            }
            break;
        case AstKind::ExprIndex:
            collect_assigned_vars_expr_impl(expr->index.object, out, seen);
            collect_assigned_vars_expr_impl(expr->index.index, out, seen);
            break;
        case AstKind::ExprGet:
            collect_assigned_vars_expr_impl(expr->get.object, out, seen);
            break;
        case AstKind::ExprGrouping:
            collect_assigned_vars_expr_impl(expr->grouping.expr, out, seen);
            break;
        case AstKind::ExprStringInterp:
            for (auto* sub_expr : expr->string_interp.expressions) {
                collect_assigned_vars_expr_impl(sub_expr, out, seen);
            }
            break;
        default:
            break;
    }
}

Span<BlockArgPair> IRBuilder::make_loop_args(const Vector<LoopVarInfo>& loop_vars) {
    if (loop_vars.empty()) return {};

    Vector<BlockArgPair> args;
    for (const auto& lv : loop_vars) {
        // Look up the current value of this variable
        ValueId current_val = lookup_local(lv.name);
        args.push_back({current_val});
    }
    return alloc_span(args);
}

// Opcode selection

}
