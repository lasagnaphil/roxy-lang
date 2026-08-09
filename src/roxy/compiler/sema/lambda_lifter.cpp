#include "roxy/compiler/sema/lambda_lifter.hpp"

#include <cstdio>
#include <cstring>

namespace rx {

Vector<u32> LambdaLifter::collect_crossed_lambda_contexts(const Scope* stop_scope) {
    Vector<u32> crossed_ctx_indices;
    for (Scope* sc = m_symbols.current_scope(); sc; sc = sc->parent) {
        if (sc == stop_scope) break;
        if (sc->kind == ScopeKind::Lambda) {
            for (u32 i = 0; i < m_lambda_contexts.size(); i++) {
                if (m_lambda_contexts[i]->boundary_scope == sc) {
                    crossed_ctx_indices.push_back(i);
                    break;
                }
            }
        }
    }
    return crossed_ctx_indices;
}

bool LambdaLifter::try_capture_identifier(Expr* expr, Symbol* sym, Type** out) {
    IdentifierExpr& id = expr->identifier;

    // Capture-boundary check: if we're inside a lambda body and this identifier
    // resolves to a symbol defined past a `ScopeKind::Lambda` boundary, treat it
    // as a closure capture. Function / struct / enum / trait / module / imported
    // symbols are never captured — they're effectively top-level.
    bool is_capturable_kind =
        sym->kind != SymbolKind::Function &&
        sym->kind != SymbolKind::Struct &&
        sym->kind != SymbolKind::Enum &&
        sym->kind != SymbolKind::Trait &&
        sym->kind != SymbolKind::Module &&
        sym->kind != SymbolKind::ImportedFunction;

    if (!is_capturable_kind || !sym->defining_scope || m_lambda_contexts.empty()) {
        return false;
    }

    // Collect every lambda context whose boundary sits between us and the
    // symbol's defining scope (innermost first). For nested closures a lookup
    // can cross multiple boundaries; each enclosing lambda must also capture
    // the symbol so it can be passed inward through env-fields.
    Vector<u32> crossed_ctx_indices = collect_crossed_lambda_contexts(sym->defining_scope);

    if (crossed_ctx_indices.empty()) return false;

    StringView captured_name = id.name;
    SourceLocation captured_loc = expr->loc;
    Type* captured_type = sym->type;

    // Mode rules (copy-only for transitive captures — [move] is
    // pre-validated to forbid transit). Implicit capture of a
    // noncopyable across any boundary is a clear error.
    if (captured_type && captured_type->noncopyable()) {
        // The variable is in scope but noncopyable; only [move] makes
        // it valid, and that path pre-populates the context (so
        // by_symbol would already contain it). If we're here on first
        // reference, the user forgot [move].
        LambdaCaptureContext& innermost = *m_lambda_contexts[crossed_ctx_indices[0]];
        if (innermost.by_symbol.find(sym) == innermost.by_symbol.end()) {
            m_reporter.error_fmt(captured_loc,
                "cannot implicitly capture '{}' of noncopyable type; "
                "use 'fun[move {}](...)' to move it into the closure",
                captured_name, captured_name);
            *out = m_types.error_type();
            return true;
        }
    } else {
        // Use-before-move check on the outer symbol still applies for
        // copyable captures (e.g., capturing a moved-from i32 isn't
        // possible since i32 isn't tracked, but for `ref` types the
        // underlying owner could be).
        if (!m_lifetimes.check_not_moved(sym, captured_name, captured_loc)) {
            *out = m_types.error_type();
            return true;
        }

        // Walk crossed contexts from outermost to innermost. The
        // outermost reads the capture directly from its enclosing
        // scope (where the symbol is a normal local). Inner contexts
        // read from the next-outer lambda's __env.<name>, since at
        // their construction site the variable is no longer in scope
        // — only the enclosing env is.
        for (i32 i = static_cast<i32>(crossed_ctx_indices.size()) - 1; i >= 0; i--) {
            u32 ctx_idx = crossed_ctx_indices[i];
            LambdaCaptureContext& ctx = *m_lambda_contexts[ctx_idx];
            if (ctx.by_symbol.find(sym) != ctx.by_symbol.end()) continue;

            bool is_outermost_crossed =
                (i == static_cast<i32>(crossed_ctx_indices.size()) - 1);

            Expr* src;
            if (is_outermost_crossed) {
                // Direct identifier in the enclosing scope.
                src = make_identifier_expr(captured_name, captured_type, captured_loc);
            } else {
                // Read from the immediately-enclosing context's env.
                // crossed_ctx_indices is innermost-first, so the
                // *enclosing* of ctx is at crossed_ctx_indices[i+1].
                u32 enclosing_ctx_idx = crossed_ctx_indices[i + 1];
                Type* enclosing_env_type =
                    m_lambda_contexts[enclosing_ctx_idx]->env_struct_type;
                Type* enclosing_env_ref = enclosing_env_type
                    ? m_types.ref_type(enclosing_env_type)
                    : nullptr;

                Expr* env_id = make_identifier_expr("__env"_sv,
                                                    enclosing_env_ref, captured_loc);
                src = make_get_expr(env_id, captured_name, captured_type, captured_loc);
            }

            u32 index = static_cast<u32>(ctx.captures.size());
            CaptureInfo info{captured_name, captured_type, CaptureMode::Copy,
                             sym, captured_loc, src};
            ctx.captures.push_back(info);
            ctx.by_symbol[sym] = index;
        }
    }

    // Rewrite the IdentifierExpr in-place to `__env.<name>` referring to
    // the *innermost* lambda's env (since that's the scope we're
    // currently analyzing the body of).
    LambdaCaptureContext& innermost = *m_lambda_contexts[crossed_ctx_indices[0]];
    Type* innermost_env_type = innermost.env_struct_type;
    Type* innermost_env_ref = innermost_env_type
        ? m_types.ref_type(innermost_env_type)
        : nullptr;

    Expr* env_id = make_identifier_expr("__env"_sv,
                                        innermost_env_ref, captured_loc);

    expr->kind = AstKind::ExprGet;
    expr->get.object = env_id;
    expr->get.name = captured_name;
    *out = captured_type;
    return true;
}

// Helper: allocate a NUL-free StringView in the bump allocator.
static StringView alloc_view(BumpAllocator& alloc, const char* str) {
    u32 len = static_cast<u32>(strlen(str));
    char* buf = reinterpret_cast<char*>(alloc.alloc_bytes(len, 1));
    memcpy(buf, str, len);
    return StringView(buf, len);
}

// Helper: format a name like "__lambda_42_env" via snprintf into the bump allocator.
static StringView alloc_view_fmt(BumpAllocator& alloc, const char* fmt, u32 id) {
    char tmp[64];
    int n = snprintf(tmp, sizeof(tmp), fmt, id);
    if (n < 0) n = 0;
    if (n > (int)sizeof(tmp) - 1) n = (int)sizeof(tmp) - 1;
    char* buf = reinterpret_cast<char*>(alloc.alloc_bytes(static_cast<u32>(n), 1));
    memcpy(buf, tmp, static_cast<u32>(n));
    return StringView(buf, static_cast<u32>(n));
}

Expr* LambdaLifter::make_identifier_expr(StringView name, Type* type, SourceLocation loc) {
    Expr* e = m_allocator.emplace<Expr>();
    e->kind = AstKind::ExprIdentifier;
    e->loc = loc;
    e->identifier.name = name;
    e->resolved_type = type;
    return e;
}

Expr* LambdaLifter::make_get_expr(Expr* object, StringView name, Type* type, SourceLocation loc) {
    Expr* e = m_allocator.emplace<Expr>();
    e->kind = AstKind::ExprGet;
    e->loc = loc;
    e->get.object = object;
    e->get.name = name;
    e->resolved_type = type;
    return e;
}

Expr* LambdaLifter::make_this_expr(Type* type, SourceLocation loc) {
    Expr* e = m_allocator.emplace<Expr>();
    e->kind = AstKind::ExprThis;
    e->loc = loc;
    e->resolved_type = type;
    return e;
}

void LambdaLifter::ensure_self_captured_through(u32 target_idx, Type* struct_type,
                                                    SourceLocation loc) {
    if (target_idx >= m_lambda_contexts.size()) return;
    LambdaCaptureContext& ctx = *m_lambda_contexts[target_idx];
    if (ctx.has_self_capture) return;

    // Recurse outward first: the source for level N depends on level N-1
    // having `__self` available in its env.
    if (target_idx > 0) {
        ensure_self_captured_through(target_idx - 1, struct_type, loc);
    }

    Type* ref_self = m_types.ref_type(struct_type);

    Expr* src;
    if (target_idx == 0) {
        // Outermost lambda: source is ExprThis (resolves to the method's
        // `self` parameter at IR-build time, where the IR is being emitted in
        // the enclosing method's context).
        src = make_this_expr(ref_self, loc);
    } else {
        // Inner lambda: read from the immediately-enclosing lambda's env.
        // That outer's `__self` field was just populated by the recursive call
        // above (or was already there from a previous capture).
        LambdaCaptureContext& outer = *m_lambda_contexts[target_idx - 1];
        Type* outer_env_ref = outer.env_struct_type
            ? m_types.ref_type(outer.env_struct_type)
            : nullptr;

        Expr* env_id = make_identifier_expr("__env"_sv, outer_env_ref, loc);
        src = make_get_expr(env_id, "__self"_sv, ref_self, loc);
    }

    CaptureInfo info{};
    info.name = "__self"_sv;
    info.type = ref_self;
    info.mode = CaptureMode::Copy;       // ref pointer copied
    info.source_symbol = nullptr;
    info.loc = loc;
    info.source_expr = src;
    // The outermost capture (target_idx == 0) sources a bare `self` (ExprThis)
    // whose receiver may be stack-allocated, so it needs the runtime heap gate
    // before the borrow inc. Inner captures source through the enclosing lambda's
    // env (`__env.__self`), a known-heap pointer, so they never need it. This
    // holds regardless of copyability: a *noncopyable* value-struct (one with a
    // destructor) is still stack-capable, so it must be gated too — the old
    // "noncopyable ⇒ heap" assumption was wrong (lifetimes.md "Promotion").
    info.needs_heap_check = (target_idx == 0);

    ctx.self_capture_index = static_cast<u32>(ctx.captures.size());
    ctx.captures.push_back(info);
    ctx.has_self_capture = true;
}

Type* LambdaLifter::analyze_lambda_expr(Expr* expr) {
    LambdaExpr& le = expr->lambda;

    // Resolve user-facing param and return types (the `Function<sig>` type).
    Vector<Type*> sig_param_types;
    for (auto& p : le.params) {
        Type* pt = m_context.resolve_type_expr(p.type);
        if (pt->is_error()) return m_types.error_type();
        sig_param_types.push_back(pt);
    }
    Type* ret_type = le.return_type ? m_context.resolve_type_expr(le.return_type) : m_types.void_type();
    if (ret_type->is_error()) return m_types.error_type();

    u32 lambda_id = m_lambda_id_counter++;
    StringView env_name = alloc_view_fmt(m_allocator, "__lambda_%u_env", lambda_id);
    StringView fun_name = alloc_view_fmt(m_allocator, "__lambda_%u_call", lambda_id);

    // Register the env struct type early (with no fields yet) so that __env's
    // type annotation `ref __lambda_<id>_env` resolves during param setup. We
    // backfill the fields after body analysis once captures are known.
    Type* env_type = m_types.struct_type(env_name, nullptr);
    env_type->struct_info.fields = Span<FieldInfo>();
    env_type->struct_info.slot_count = 0;
    env_type->struct_info.constructors = Span<ConstructorInfo>();
    env_type->struct_info.destructors = Span<DestructorInfo>();
    env_type->struct_info.methods = Span<MethodInfo>();
    env_type->struct_info.when_clauses = Span<WhenClauseInfo>();
    env_type->struct_info.implemented_traits = Span<TraitImplRecord>();
    env_type->struct_info.parent = nullptr;
    env_type->struct_info.module_name = StringView(nullptr, 0);
    // Fieldless for now; backfill_lambda_env re-derives once the captures are in.
    derive_struct_move_only(env_type->struct_info);
    m_type_env.register_named_type(env_name, env_type);

    LambdaCaptureContext context;
    context.boundary_scope = nullptr;       // set after pushing the Lambda scope
    context.env_struct_type = env_type;

    // Phase 1: pre-validate and collect the capture list into `context`.
    if (!validate_lambda_captures(le, context)) return m_types.error_type();

    // Phase 2: synthesize the lifted call function and analyze its body.
    Decl* synth_decl = synthesize_lambda_call_fn(expr, le, fun_name, env_name, ret_type, context);

    // Phase 3: backfill the env struct fields from the resolved captures.
    backfill_lambda_env(env_type, context);

    // ===== Mark outer-scope move captures as consumed =====
    // For each Move-mode capture, mark the OUTER symbol moved so subsequent
    // references in the surrounding scope correctly fail with use-after-move.
    for (const CaptureInfo& cap : context.captures) {
        if (cap.mode == CaptureMode::Move) {
            // Moving an out/inout parameter into a closure env transfers the
            // caller's value to the env (which frees it on drop) — a second-class
            // escape (lifetimes.md "The second-class family"), even when the closure itself does not
            // escape. Reject it (this move site bypasses consume_noncopyable).
            Symbol* cap_sym = m_symbols.lookup(cap.name);
            if (cap_sym && cap_sym->kind == SymbolKind::Parameter && cap_sym->is_out_inout) {
                m_reporter.error_fmt(expr->loc,
                          "cannot move an 'out'/'inout' parameter ('{}') into a "
                          "closure; it borrows the caller's value",
                          cap.name);
                continue;
            }
            m_lifetimes.mark_moved(cap_sym);
        }
    }

    // Stash the synthesized decl so the IR builder picks it up.
    m_synthetic_decls.push_back(synth_decl);

    // Annotate the LambdaExpr for downstream consumption (IR builder).
    le.env_struct_name = env_name;
    le.call_function_name = fun_name;
    le.env_struct_type = env_type;
    le.resolved_captures = m_allocator.alloc_span(context.captures);

    // The lambda expression's resolved type is the user-facing `Function<sig>`.
    Span<Type*> sig_span = m_allocator.alloc_span(sig_param_types);
    return m_types.function_type(sig_span, ret_type);
}

bool LambdaLifter::validate_lambda_captures(LambdaExpr& le, LambdaCaptureContext& context) {
    // Pre-validate explicit capture entries:
    // [move <name>]: noncopyable, no transitive moves, sets up a Move source.
    // [copy self]:   copyable struct, no when-clauses; synthesizes a struct
    //                literal source so the env field holds a value snapshot.
    // [weak self]:   any struct kind; copyable structs get a runtime heap check
    //                at construction time (receiver might be stack-allocated).
    //
    // [copy self] / [weak self] are valid as long as we're inside a struct
    // method. Nested closures are supported: the outer chain gets implicit
    // ref-self captures so the inner lambda can read self via the enclosing
    // env's `__self` field at construction time.
    auto self_lambda_method_struct = [this]() -> Type* {
        if (!m_symbols.is_in_struct()) return nullptr;
        return m_symbols.current_struct_type();
    };

    for (auto& entry : le.captures) {
        if (entry.mode == CaptureMode::Move) {
            Symbol* outer_sym = m_symbols.lookup(entry.name);
            if (!outer_sym) {
                m_reporter.error_fmt(entry.loc, "capture list references unknown variable '{}'", entry.name);
                return false;
            }
            if (!outer_sym->type || outer_sym->type->is_copy()) {
                m_reporter.error_fmt(entry.loc,
                    "move captures only apply to noncopyable types; '{}' is copyable, capture it implicitly",
                    entry.name);
                return false;
            }
            if (context.by_symbol.find(outer_sym) != context.by_symbol.end()) {
                m_reporter.error_fmt(entry.loc, "duplicate capture entry for '{}'", entry.name);
                return false;
            }
            if (!m_lifetimes.check_not_moved(outer_sym, entry.name, entry.loc)) return false;

            // Walk crossed Lambda boundaries between this lambda and the
            // symbol's defining scope. For each one, propagate a Move-mode
            // capture so ownership flows down the chain: outermost lambda
            // moves x from the function scope; each intermediate moves x
            // out of its enclosing env's field; the innermost (this) lambda
            // does the same. Mirrors the implicit-copy logic in
            // analyze_identifier_expr but with Move mode + per-level
            // ownership transfer.
            Vector<u32> crossed_ctx_indices;
            if (outer_sym->defining_scope) {
                crossed_ctx_indices = collect_crossed_lambda_contexts(outer_sym->defining_scope);
            }

            // Helper to build a source expression that reads the variable
            // either directly from the enclosing scope (outermost) or from
            // the next-outer lambda's env field (intermediate / innermost).
            auto build_src_for_level = [&](i32 enclosing_ctx_idx) -> Expr* {
                if (enclosing_ctx_idx < 0) {
                    return make_identifier_expr(entry.name, outer_sym->type, entry.loc);
                }
                LambdaCaptureContext& enclosing_ctx =
                    *m_lambda_contexts[enclosing_ctx_idx];
                Type* enclosing_env_ref = enclosing_ctx.env_struct_type
                    ? m_types.ref_type(enclosing_ctx.env_struct_type)
                    : nullptr;
                Expr* env_id = make_identifier_expr("__env"_sv,
                                                    enclosing_env_ref, entry.loc);
                return make_get_expr(env_id, entry.name, outer_sym->type, entry.loc);
            };

            // Add Move captures to crossed enclosing contexts (outermost
            // first). Each one's source reads from the next outer level.
            for (i32 i = static_cast<i32>(crossed_ctx_indices.size()) - 1; i >= 0; i--) {
                u32 ctx_idx = crossed_ctx_indices[i];
                LambdaCaptureContext& ctx = *m_lambda_contexts[ctx_idx];
                if (ctx.by_symbol.find(outer_sym) != ctx.by_symbol.end()) {
                    m_reporter.error_fmt(entry.loc,
                        "'{}' is already captured implicitly by an enclosing "
                        "lambda; declare '[move {}]' on it (or refactor) so the "
                        "ownership chain is consistent", entry.name, entry.name);
                    return false;
                }
                bool is_outermost = (i == static_cast<i32>(crossed_ctx_indices.size()) - 1);
                i32 enclosing_idx = is_outermost
                    ? -1
                    : static_cast<i32>(crossed_ctx_indices[i + 1]);
                Expr* src = build_src_for_level(enclosing_idx);

                u32 index = static_cast<u32>(ctx.captures.size());
                CaptureInfo info{};
                info.name = entry.name;
                info.type = outer_sym->type;
                info.mode = CaptureMode::Move;
                info.source_symbol = outer_sym;
                info.loc = entry.loc;
                info.source_expr = src;
                ctx.captures.push_back(info);
                ctx.by_symbol[outer_sym] = index;
            }

            // This (innermost) lambda's own capture entry. Source reads from
            // the immediate enclosing lambda's env (if any), else from the
            // function scope directly.
            i32 imm_enclosing_idx = crossed_ctx_indices.empty()
                ? -1
                : static_cast<i32>(crossed_ctx_indices[0]);
            Expr* src = build_src_for_level(imm_enclosing_idx);

            u32 index = static_cast<u32>(context.captures.size());
            CaptureInfo info{};
            info.name = entry.name;
            info.type = outer_sym->type;
            info.mode = CaptureMode::Move;
            info.source_symbol = outer_sym;
            info.loc = entry.loc;
            info.source_expr = src;
            context.captures.push_back(info);
            context.by_symbol[outer_sym] = index;
            continue;
        }

        // [copy self] and [weak self] — both are self-only in this commit.
        if (entry.name != "self"_sv) {
            m_reporter.error_fmt(entry.loc,
                "[copy ...] / [weak ...] captures are currently restricted to 'self'");
            return false;
        }
        if (context.has_self_capture) {
            m_reporter.error(entry.loc, "duplicate self capture in capture list");
            return false;
        }

        Type* struct_type = self_lambda_method_struct();
        if (!struct_type) {
            m_reporter.error_fmt(entry.loc,
                "[{} self] is only valid inside a struct method",
                entry.mode == CaptureMode::Copy ? "copy" : "weak");
            return false;
        }

        // For nested lambdas, ensure each enclosing context has self captured
        // (implicit ref-self) so the chain works. After this, the immediately-
        // enclosing context's `__env.__self` field is available at the inner's
        // construction site.
        if (m_lambda_contexts.size() > 0) {
            ensure_self_captured_through(static_cast<u32>(m_lambda_contexts.size()) - 1,
                                         struct_type, entry.loc);
        }

        // Helper: build an Expr* that, when gen_expr'd in the *enclosing* IR
        // scope (i.e. at the lambda's construction site), produces a ref Self.
        // Directly inside a method → ExprThis; nested → ExprGet on the
        // enclosing env's __self field.
        auto build_outer_self_ref_source = [&](SourceLocation loc) -> Expr* {
            Type* ref_self = m_types.ref_type(struct_type);
            if (m_lambda_contexts.empty()) {
                return make_this_expr(ref_self, loc);
            }
            LambdaCaptureContext& outer = *m_lambda_contexts.back();
            Type* outer_env_ref = outer.env_struct_type
                ? m_types.ref_type(outer.env_struct_type)
                : nullptr;

            Expr* env_id = make_identifier_expr("__env"_sv, outer_env_ref, loc);
            return make_get_expr(env_id, "__self"_sv, ref_self, loc);
        };

        if (entry.mode == CaptureMode::Copy) {
            if (struct_type->noncopyable()) {
                m_reporter.error_fmt(entry.loc,
                    "cannot [copy self] of noncopyable struct '{}'; use [weak self] instead",
                    struct_type->struct_info.name);
                return false;
            }
            if (struct_type->struct_info.when_clauses.size() > 0) {
                m_reporter.error_fmt(entry.loc,
                    "[copy self] on tagged-union struct '{}' is not yet supported",
                    struct_type->struct_info.name);
                return false;
            }

            // Synthesize the struct literal:
            //   Self { f0 = <self_ref>.f0, f1 = <self_ref>.f1, ... }
            // where <self_ref> is ExprThis (direct method) or ExprGet(__env, __self)
            // (nested). Each field initializer needs its own clone of the
            // self-ref source so the AST nodes aren't shared.
            const auto& fields = struct_type->struct_info.fields;
            FieldInit* inits = reinterpret_cast<FieldInit*>(
                m_allocator.alloc_bytes(sizeof(FieldInit) * fields.size(), alignof(FieldInit)));
            for (u32 i = 0; i < fields.size(); i++) {
                Expr* self_ref = build_outer_self_ref_source(entry.loc);

                Expr* field_get = make_get_expr(self_ref, fields[i].name,
                                                fields[i].type, entry.loc);

                inits[i].name = fields[i].name;
                inits[i].value = field_get;
                inits[i].loc = entry.loc;
            }
            Expr* src = m_allocator.emplace<Expr>();
            src->kind = AstKind::ExprStructLiteral;
            src->loc = entry.loc;
            src->struct_literal.type_name = struct_type->struct_info.name;
            src->struct_literal.fields = Span<FieldInit>(inits, fields.size());
            src->struct_literal.type_args = Span<TypeExpr*>();
            src->struct_literal.mangled_name = StringView();
            src->struct_literal.is_heap = false;
            src->resolved_type = struct_type;

            CaptureInfo info{};
            info.name = "__self"_sv;
            info.type = struct_type;     // value-Self in env
            info.mode = CaptureMode::Copy;
            info.source_symbol = nullptr;
            info.loc = entry.loc;
            info.source_expr = src;
            info.needs_heap_check = false;  // dereferences happen via known-heap outer env

            context.self_capture_index = static_cast<u32>(context.captures.size());
            context.captures.push_back(info);
            context.has_self_capture = true;
        } else {  // CaptureMode::Weak
            Type* weak_self = m_types.weak_type(struct_type);
            // For nested cases the source comes through outer's __env (a heap ref
            // already), so the receiver-on-heap requirement is satisfied
            // transitively. For a direct method the source is the bare ExprThis,
            // whose receiver may be stack-allocated, so it needs the runtime heap
            // check before WeakCreate snapshots the generation — regardless of
            // copyability, since a noncopyable value-struct (with a destructor) is
            // still stack-capable and would otherwise snapshot a bogus generation
            // from stack bytes (lifetimes.md "Promotion").
            bool nested = m_lambda_contexts.size() > 0;

            Expr* src = build_outer_self_ref_source(entry.loc);

            CaptureInfo info{};
            info.name = "__self"_sv;
            info.type = weak_self;
            info.mode = CaptureMode::Weak;
            info.source_symbol = nullptr;
            info.loc = entry.loc;
            info.source_expr = src;
            info.needs_heap_check = !nested;

            context.self_capture_index = static_cast<u32>(context.captures.size());
            context.captures.push_back(info);
            context.has_self_capture = true;
        }
    }
    return true;
}

Decl* LambdaLifter::synthesize_lambda_call_fn(Expr* expr, LambdaExpr& le,
                                                  StringView fun_name, StringView env_name,
                                                  Type* ret_type, LambdaCaptureContext& context) {
    // Signature: fun __lambda_<id>_call(__env: ref __lambda_<id>_env, params...): R
    Decl* synth_decl = m_allocator.emplace<Decl>();
    synth_decl->kind = AstKind::DeclFun;
    synth_decl->loc = expr->loc;
    FunDecl& fd = synth_decl->fun_decl;
    fd.name = fun_name;
    fd.type_params = Span<TypeParam>();
    fd.return_type = le.return_type;
    fd.body = le.body;
    fd.is_pub = false;
    fd.is_native = false;

    // Build the lifted parameter list: __env first, then the lambda's params verbatim.
    {
        u32 num_params = 1 + static_cast<u32>(le.params.size());
        Param* params = reinterpret_cast<Param*>(
            m_allocator.alloc_bytes(sizeof(Param) * num_params, alignof(Param)));

        TypeExpr* env_te = m_allocator.emplace<TypeExpr>();
        env_te->kind = TypeExprKind::Named;
        env_te->name = env_name;
        env_te->loc = expr->loc;
        env_te->ref_kind = RefKind::Ref;
        env_te->type_args = Span<TypeExpr*>();
        env_te->return_type = nullptr;

        params[0].name = alloc_view(m_allocator, "__env");
        params[0].type = env_te;
        params[0].modifier = ParamModifier::None;
        params[0].loc = expr->loc;
        params[0].resolved_type = nullptr;

        for (u32 i = 0; i < le.params.size(); i++) {
            params[1 + i] = le.params[i];
        }
        fd.params = Span<Param>(params, num_params);
    }

    // Push the lambda boundary scope so analyze_identifier_expr can detect captures,
    // then push the function scope and define params. Capture detection records
    // captures into `context` and rewrites the captured IdentifierExpr in-place.
    m_symbols.push_scope(ScopeKind::Lambda);
    context.boundary_scope = m_symbols.current_scope();
    m_lambda_contexts.push_back(&context);
    {
        // Analyze the lambda body under a fresh per-function context: a
        // lambda body is its own function, so it is not a coroutine, not
        // inside the enclosing delete destructor or finally blocks (its
        // statements run when the closure is CALLED, not here), and its
        // lifetime state (move states + termination flag — a `return` in the
        // lambda body must not read as "the enclosing branch terminates") is
        // its own. The guard restores everything as one unit at block end.
        FunctionContextScope context_scope(m_function_context, m_lifetimes);

        m_symbols.push_function_scope(ret_type);

        for (u32 i = 0; i < fd.params.size(); i++) {
            Param& p = fd.params[i];
            Type* ptype = m_context.resolve_type_expr(p.type);
            p.resolved_type = ptype;
            // A USER lambda parameter shadowing an enclosing-function local is
            // banned (the walk crosses the Lambda boundary), matching the
            // var-decl and catch-variable sites. The synthesized `__env`
            // parameter (param 0 of every lifted lambda) is exempt: nested
            // lambdas each carry their own `__env`, which is sound — every
            // lambda body becomes its own IR function — and not something the
            // user wrote.
            bool is_synthesized_env = p.name == "__env"_sv;
            if (m_symbols.lookup_local(p.name)) {
                m_reporter.error_fmt(p.loc, "duplicate parameter name '{}'", p.name);
            } else if (is_synthesized_env || m_context.check_no_local_shadowing(p.name, p.loc)) {
                m_symbols.define_parameter(p.name, ptype, p.loc, i,
                                       p.modifier != ParamModifier::None);
            }
            if (ptype && ptype->noncopyable()) {
                m_lifetimes.track_live(m_symbols.lookup(p.name));
            }
        }

        m_context.analyze_stmt(fd.body);

        // All-paths-return: a non-void lambda body must terminate (return/throw
        // or an unreachable fall-through) on every path, exactly like a free
        // function (analyze_fun_body). Lambdas are never coroutines, so there is
        // no yield exemption. The `=> expr` short body desugars to a `return`, so
        // only block-bodied lambdas that fall off the end are flagged here.
        if (ret_type && !ret_type->is_void() && !m_lifetimes.branch_terminates()) {
            m_reporter.error_fmt(expr->loc, "not all code paths return a value in lambda");
        }

        m_lifetimes.check_scope_exit_uniq_destructors(m_symbols.current_scope(), expr->loc);
        m_symbols.pop_scope();  // function scope
        // context_scope restores the outer per-function context at block end.
    }
    m_lambda_contexts.pop_back();
    m_symbols.pop_scope();  // lambda boundary scope
    return synth_decl;
}

void LambdaLifter::backfill_lambda_env(Type* env_type, const LambdaCaptureContext& context) {
    // Backfill the env struct fields with [__call_idx, captures...]. Field 0 is
    // __call_idx (u32, slot 0); capture fields follow at increasing slot
    // offsets. If any capture is noncopyable, attach a synthetic default
    // destructor — Type::noncopyable() will then return true and the IR builder
    // auto-emits the destructor body (see ir_builder.cpp:260-286).
    u32 num_fields = 1 + static_cast<u32>(context.captures.size());
    FieldInfo* fields = reinterpret_cast<FieldInfo*>(
        m_allocator.alloc_bytes(sizeof(FieldInfo) * num_fields, alignof(FieldInfo)));
    fields[0].name = alloc_view(m_allocator, "__call_idx");
    fields[0].type = m_types.u32_type();
    fields[0].is_pub = false;
    fields[0].index = 0;
    fields[0].slot_offset = 0;
    fields[0].slot_count = 1;

    u32 current_slot = 1;
    bool any_noncopyable = false;
    for (u32 i = 0; i < context.captures.size(); i++) {
        const CaptureInfo& cap = context.captures[i];
        FieldInfo& f = fields[1 + i];
        f.name = cap.name;
        f.type = cap.type;
        f.is_pub = false;
        f.index = 1 + i;
        f.slot_offset = current_slot;
        f.slot_count = get_type_slot_count(cap.type);
        current_slot += f.slot_count;
        // A noncopyable capture needs delete-on-drop; a `ref` capture (a counted
        // borrow — [ref self] or a captured ref local) needs RefDec-on-drop.
        // Both require the env to carry a destructor.
        if (cap.type && (cap.type->noncopyable() || cap.type->kind == TypeKind::Ref)) {
            any_noncopyable = true;
        }
    }

    env_type->struct_info.fields = Span<FieldInfo>(fields, num_fields);
    env_type->struct_info.slot_count = current_slot;

    if (any_noncopyable) {
        DestructorInfo* dtor = reinterpret_cast<DestructorInfo*>(
            m_allocator.alloc_bytes(sizeof(DestructorInfo), alignof(DestructorInfo)));
        dtor->name = StringView();      // empty = default destructor
        dtor->param_types = Span<Type*>();
        dtor->decl = nullptr;
        env_type->struct_info.destructors = Span<DestructorInfo>(dtor, 1);
    }

    // The env's fields are final here, so derive its lifecycle. It comes out
    // move-only exactly when it captures something owned — which is also when
    // the destructor above is attached, since both read the same property of the
    // captures. (Inert in practice: an env is only ever reached through a
    // `Function` value, which is move-only on its own account. Deriving it anyway
    // keeps "every struct's flag is derived" a rule with no exceptions.)
    derive_struct_move_only(env_type->struct_info);
}

Type* LambdaLifter::try_rewrite_self_capture(Expr* expr, Type* struct_type) {
    // No active lambda context, or the struct scope is reached without
    // crossing a Lambda boundary: `self` is a plain method receiver.
    if (m_lambda_contexts.empty()) return nullptr;
    if (collect_crossed_lambda_contexts(m_symbols.current_struct_scope()).empty()) {
        return nullptr;
    }

    // Ensure every enclosing lambda context has self captured (implicit
    // ref-self) so the chain works for nested closures. If the innermost
    // already has an explicit [copy self] / [weak self], its existing entry's
    // type drives the rewrite — but the chain still needs the OUTER contexts'
    // implicit refs to be populated.
    u32 last_idx = static_cast<u32>(m_lambda_contexts.size()) - 1;
    LambdaCaptureContext& innermost = *m_lambda_contexts[last_idx];
    if (last_idx > 0) {
        ensure_self_captured_through(last_idx - 1, struct_type, expr->loc);
    }
    if (!innermost.has_self_capture) {
        ensure_self_captured_through(last_idx, struct_type, expr->loc);
    }

    CaptureInfo& info = innermost.captures[innermost.self_capture_index];

    // Rewrite the ExprThis in-place to `__env.__self`. The env field type
    // drives the resulting expr's resolved_type (ref Self for implicit /
    // `[ref]`-equivalent, value Self for [copy], weak Self for [weak]).
    Type* env_ref = innermost.env_struct_type
        ? m_types.ref_type(innermost.env_struct_type)
        : nullptr;
    Expr* env_id = make_identifier_expr("__env"_sv, env_ref, expr->loc);

    expr->kind = AstKind::ExprGet;
    expr->get.object = env_id;
    expr->get.name = "__self"_sv;
    return info.type;
}

}
