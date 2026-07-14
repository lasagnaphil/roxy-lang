#include "roxy/compiler/lifetime_checker.hpp"

namespace rx {

// ===== Move-State Tracking for Uniq Variables =====

void LifetimeChecker::merge_move_states(const MoveStateSnapshot& then_states,
                                          const MoveStateSnapshot& else_states) {
    // For each tracked variable, merge states from both branches
    for (auto it = m_move_states.begin(); it != m_move_states.end(); ++it) {
        Symbol* sym = it->first;
        auto then_it = then_states.find(sym);
        auto else_it = else_states.find(sym);

        MoveState then_state = (then_it != then_states.end()) ? then_it->second : it->second;
        MoveState else_state = (else_it != else_states.end()) ? else_it->second : it->second;

        if (then_state == else_state) {
            it.value() = then_state;
        } else {
            // Branches disagree — variable may or may not be valid
            it.value() = MoveState::MaybeValid;
        }
    }
}

void LifetimeChecker::merge_two_branches(const MoveStateSnapshot& pre_branch,
                                          const MoveStateSnapshot& then_states,
                                          const MoveStateSnapshot& else_states,
                                          bool then_terminates, bool else_terminates) {
    // Terminating branches contribute no state to the post-merge point.
    // Pick the surviving branch's snapshot; if both terminate, the code
    // after is unreachable but we pick then_states arbitrarily.
    if (then_terminates && !else_terminates) {
        restore_move_states(else_states);
    } else if (!then_terminates && else_terminates) {
        restore_move_states(then_states);
    } else {
        restore_move_states(pre_branch);
        merge_move_states(then_states, else_states);
    }
    m_branch_terminates = then_terminates && else_terminates;
}

bool LifetimeChecker::merge_branch_snapshots(const Vector<MoveStateSnapshot>& snapshots,
                                              const Vector<bool>& terminates) {
    // Pairwise-merge only the surviving (non-terminating) snapshots.
    bool have_survivor = false;
    for (u32 i = 0; i < snapshots.size(); i++) {
        if (terminates[i]) continue;
        if (!have_survivor) {
            restore_move_states(snapshots[i]);
            have_survivor = true;
        } else {
            MoveStateSnapshot current = save_move_states();
            merge_move_states(current, snapshots[i]);
        }
    }

    // No survivor means every branch terminates: the join point is
    // unreachable. Restore an arbitrary snapshot and report it upward.
    if (!have_survivor && !snapshots.empty()) {
        restore_move_states(snapshots[0]);
        return true;
    }
    return false;
}

bool LifetimeChecker::expr_references_name(Expr* expr, StringView name) const {
    if (!expr) return false;
    switch (expr->kind) {
        case AstKind::ExprIdentifier:
            return expr->identifier.name == name;
        case AstKind::ExprLiteral:
        case AstKind::ExprThis:
        case AstKind::ExprSuper:
        case AstKind::ExprStaticGet:
            return false;
        case AstKind::ExprUnary:
            return expr_references_name(expr->unary.operand, name);
        case AstKind::ExprBinary:
            return expr_references_name(expr->binary.left, name) ||
                   expr_references_name(expr->binary.right, name);
        case AstKind::ExprTernary:
            return expr_references_name(expr->ternary.condition, name) ||
                   expr_references_name(expr->ternary.then_expr, name) ||
                   expr_references_name(expr->ternary.else_expr, name);
        case AstKind::ExprCall: {
            if (expr_references_name(expr->call.callee, name)) return true;
            for (const auto& arg : expr->call.arguments) {
                if (expr_references_name(arg.expr, name)) return true;
            }
            return false;
        }
        case AstKind::ExprIndex:
            return expr_references_name(expr->index.object, name) ||
                   expr_references_name(expr->index.index, name);
        case AstKind::ExprGet:
            return expr_references_name(expr->get.object, name);
        case AstKind::ExprAssign:
            return expr_references_name(expr->assign.target, name) ||
                   expr_references_name(expr->assign.value, name);
        case AstKind::ExprGrouping:
            return expr_references_name(expr->grouping.expr, name);
        case AstKind::ExprStructLiteral: {
            for (const auto& field : expr->struct_literal.fields) {
                if (expr_references_name(field.value, name)) return true;
            }
            return false;
        }
        case AstKind::ExprStringInterp: {
            for (Expr* sub : expr->string_interp.expressions) {
                if (expr_references_name(sub, name)) return true;
            }
            return false;
        }
        default:
            // Lambdas (which may capture the variable) and any future expression
            // kind: assume a reference so we never wrongly exempt a real hazard.
            return true;
    }
}

bool LifetimeChecker::loop_reassigns_var_first(Stmt* body, StringView var_name) const {
    if (!body) return false;

    // Find the leading statement. For a braced block, that is its first
    // declaration — which must be a plain expression statement (a leading var
    // decl introduces a new name, never a reassignment of the outer variable).
    // `Decl::kind` holds the statement kind directly for statement-wrapped decls.
    Stmt* first = body;
    if (body->kind == AstKind::StmtBlock) {
        if (body->block.declarations.size() == 0) return false;
        Decl* decl = body->block.declarations[0];
        if (!decl || decl->kind != AstKind::StmtExpr) return false;
        first = &decl->stmt;
    }

    // The leading statement must be a plain `var_name = rhs` assignment.
    if (first->kind != AstKind::StmtExpr) return false;
    Expr* e = first->expr_stmt.expr;
    if (!e || e->kind != AstKind::ExprAssign) return false;
    const AssignExpr& assign = e->assign;
    if (assign.op != AssignOp::Assign) return false;  // compound ops read the target first
    if (!assign.target || assign.target->kind != AstKind::ExprIdentifier) return false;
    if (assign.target->identifier.name != var_name) return false;

    // The RHS must not read the variable (else it observes a possibly-moved value).
    return !expr_references_name(assign.value, var_name);
}

void LifetimeChecker::check_loop_cross_iteration_moves(
        Stmt* body,
        const MoveStateSnapshot& pre_loop_states,
        const MoveStateSnapshot& post_body_states,
        SourceLocation loc) {
    for (auto& [sym, pre_state] : pre_loop_states) {
        if (pre_state != MoveState::Live) continue;
        auto post_it = post_body_states.find(sym);
        if (post_it == post_body_states.end()) continue;
        MoveState post = post_it->second;
        if (post != MoveState::Moved && post != MoveState::MaybeValid) continue;

        // A variable refreshed by the body's leading statement is Live again
        // before any use on every iteration, so the back-edge state is harmless.
        if (sym && loop_reassigns_var_first(body, sym->name)) continue;

        if (post == MoveState::Moved) {
            m_reporter.error_fmt(loc,
                "variable '{}' is moved in the loop body and never reassigned; "
                "it would be used after move on the next iteration",
                sym ? sym->name : StringView());
        } else {
            m_reporter.error_fmt(loc,
                "variable '{}' may be moved in the loop body without being "
                "reassigned; it could be used after move on the next iteration",
                sym ? sym->name : StringView());
        }
    }
}

bool LifetimeChecker::check_not_moved(StringView name, SourceLocation loc) {
    Symbol* sym = m_symbols.lookup(name);
    if (!sym) return true;
    auto it = m_move_states.find(sym);
    if (it == m_move_states.end()) return true;  // Not tracked (not noncopyable)

    if (it->second == MoveState::Moved) {
        m_reporter.error_fmt(loc, "use of moved value '{}'", name);
        return false;
    }
    if (it->second == MoveState::MaybeValid) {
        m_reporter.error_fmt(loc, "use of possibly moved value '{}'", name);
        return false;
    }
    return true;
}

bool LifetimeChecker::check_not_field_move(Expr* expr, SourceLocation loc) {
    if (expr->kind != AstKind::ExprGet) return true;

    Type* field_type = expr->resolved_type;
    if (!field_type || field_type->is_copy()) return true;

    // A noncopyable *value-struct* field can't be moved out. The containing
    // struct is destroyed by a type-level descriptor that walks every field, so
    // it would re-run the moved field's destructor (double-free); and unlike a
    // pointer field it can't be nulled in place (the move-target aliases the same
    // inline storage). Pointer-valued noncopyable fields (uniq/List/Map/Coro/fun)
    // are fine — they are nulled in the root at the move (see the IR builder).
    if (field_type->is_struct()) {
        m_reporter.error(loc, "cannot move a value-type struct field out of a struct: the "
                   "container can't track a partial move. Move the whole struct, "
                   "borrow the field with 'ref', or make the field 'uniq'");
        return false;
    }

    // Allow moving a noncopyable *pointer* field (uniq/List/Map/...) out of a
    // local value-struct variable, including nested chains like `obj.inner.field`
    // provided every link in the chain is a value struct (no references). A
    // reference type anywhere along the chain breaks the rule: we can read through
    // `uniq`/`ref`/`weak` but can't take ownership of storage we don't own.
    //
    // The root is marked moved here for *use-checking* — conservatively the whole
    // variable becomes unusable, even though its sibling fields are still valid.
    // But its scope-exit cleanup still runs: the IR builder nulls the moved-out
    // field in the root (nullify_moved_field_source) so the root's destructor
    // no-ops that field and frees the surviving siblings, instead of re-freeing
    // the value the move transferred out.
    Expr* current = expr->get.object;
    while (current->kind == AstKind::ExprGet) {
        Type* parent_type = current->resolved_type;
        if (!parent_type || parent_type->is_reference() || !parent_type->is_struct()) {
            m_reporter.error(loc, "cannot move out of a struct field; consider borrowing with 'ref' instead");
            return false;
        }
        current = current->get.object;
    }

    if (current->kind == AstKind::ExprIdentifier) {
        StringView root_name = current->identifier.name;
        Type* root_type = current->resolved_type;

        if (root_type && !root_type->is_reference() && root_type->is_struct()) {
            Symbol* root_sym = m_symbols.lookup(root_name);
            auto it = root_sym ? m_move_states.find(root_sym) : m_move_states.end();
            if (it != m_move_states.end() && it->second == MoveState::Live) {
                mark_moved(root_name);
                return true;
            }
        }
    }

    m_reporter.error(loc, "cannot move out of a struct field; consider borrowing with 'ref' instead");
    return false;
}

bool LifetimeChecker::is_out_inout_param(Expr* expr) {
    if (!expr || expr->kind != AstKind::ExprIdentifier) return false;
    Symbol* sym = m_symbols.lookup(expr->identifier.name);
    return sym && sym->kind == SymbolKind::Parameter && sym->is_out_inout;
}

void LifetimeChecker::consume_noncopyable(Expr* expr, SourceLocation loc) {
    if (!expr) return;
    Type* type = expr->resolved_type;
    if (!type || type->is_copy()) return;

    // Look through parenthesization: `(p)` denotes the same storage as `p`
    // (is_lvalue() recurses through grouping the same way). A move whose source
    // is wrapped in a grouping must still mark the underlying variable moved;
    // otherwise `consume((p))` launders the move past the identifier check below
    // and leaves `p` Live — a use-after-move false negative.
    while (expr->kind == AstKind::ExprGrouping) {
        expr = expr->grouping.expr;
        if (!expr) return;
    }

    // Second-class family (lifetimes.md "The second-class family"): an `out`/`inout` parameter borrows
    // the caller's value and the caller retains ownership, so a noncopyable
    // out/inout cannot be moved out of this frame (binding it, returning it,
    // passing it by value, storing it, capturing it by move) — that would
    // transfer and then free the caller's value, leaving it dangling. It may
    // still be used in place or passed onward as another out/inout argument
    // (the downward path, which does not consume). Copyable out/inout aren't
    // affected: a copy escapes nothing, and the type system already blocks
    // converting a value to `ref`/`weak`.
    if (is_out_inout_param(expr)) {
        m_reporter.error(loc, "cannot move an 'out'/'inout' parameter out of its frame; it "
                   "borrows the caller's value (use it in place, or pass it "
                   "onward as an 'out'/'inout' argument)");
        return;
    }

    // Moving a noncopyable value out of an index expression can be unsound. `[]`
    // dispatches to the `index` method, and the distinguishing signal is whether
    // that method is native: a user-defined `index` (the Index trait) has a
    // move-checked body, so its noncopyable return is a genuine ownership
    // transfer and is sound to consume. A *native* `index` (built-in List/Map) is
    // outside the move checker and returns the element by alias without nullifying
    // the slot, so the new owner and the container's own scope-exit cleanup both
    // free it (double-free). Reject the move only for a native index method.
    // (This is the interim rule until `borrowed`-typed returns make the result a
    // `ref`, at which point the move-out becomes a plain ref→uniq type error.)
    if (expr->kind == AstKind::ExprIndex && expr->index.object) {
        Type* obj_type = expr->index.object->resolved_type;
        if (obj_type) {
            const MethodInfo* index_method =
                m_types.lookup_method(obj_type->base_type(), "index"_sv);
            if (index_method && !index_method->native_name.empty()) {
                m_reporter.error(loc, "cannot move a noncopyable value out of a container element; "
                           "borrow it with 'ref' or remove it from the container instead");
                return;
            }
        }
    }

    if (!check_not_field_move(expr, loc)) return;

    if (expr->kind == AstKind::ExprIdentifier) {
        mark_moved(expr->identifier.name);
    }
}

void LifetimeChecker::mark_moved(StringView name) {
    Symbol* sym = m_symbols.lookup(name);
    if (!sym) return;
    auto it = m_move_states.find(sym);
    if (it != m_move_states.end()) {
        it.value() = MoveState::Moved;
    }
}

void LifetimeChecker::mark_live(StringView name) {
    Symbol* sym = m_symbols.lookup(name);
    if (!sym) return;
    auto it = m_move_states.find(sym);
    if (it != m_move_states.end()) {
        it.value() = MoveState::Live;
    }
}

void LifetimeChecker::check_scope_exit_uniq_destructors(const Scope* scope, SourceLocation loc) {
    for (Symbol* sym : scope->symbols) {
        if (sym->kind != SymbolKind::Variable && sym->kind != SymbolKind::Parameter) continue;

        Type* type = sym->type;
        if (!type || type->kind != TypeKind::Uniq) continue;

        // Check if the variable is still live (not moved/deleted)
        auto it = m_move_states.find(sym);
        if (it == m_move_states.end() || it->second != MoveState::Live) continue;

        // Get the inner struct type
        Type* inner = type->inner_type();
        if (!inner || inner->kind != TypeKind::Struct) continue;

        // Check if struct has destructors but no default (unnamed) destructor
        const StructTypeInfo& struct_info = inner->struct_info;
        if (struct_info.destructors.size() == 0) continue;

        bool has_default = false;
        for (const DestructorInfo& dtor : struct_info.destructors) {
            if (dtor.name.empty()) {
                has_default = true;
                break;
            }
        }

        if (!has_default) {
            m_reporter.error_fmt(loc, "variable '{}' of type 'uniq {}' has only named destructors; "
                          "must be explicitly deleted with 'delete {}.name(args)' before scope exit",
                      sym->name, struct_info.name, sym->name);
        }
    }
}

void LifetimeChecker::check_all_scopes_uniq_destructors(SourceLocation loc, ScopeKind stop_kind) {
    Scope* scope = m_symbols.current_scope();
    while (scope) {
        check_scope_exit_uniq_destructors(scope, loc);
        if (scope->kind == stop_kind) break;
        scope = scope->parent;
    }
}

}
