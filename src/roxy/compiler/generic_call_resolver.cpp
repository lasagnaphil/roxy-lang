#include "roxy/compiler/generic_call_resolver.hpp"

#include "roxy/core/scoped_value.hpp"

namespace rx {

// ===== Generic Trait Bound Resolution =====

Span<TraitBound> GenericCallResolver::resolve_type_param_bounds(Span<TypeExpr*> bound_exprs, SourceLocation loc) {
    if (bound_exprs.size() == 0) return {};

    Vector<TraitBound> bounds;
    for (auto* bound_expr : bound_exprs) {
        // Reject reference kinds on bounds
        if (bound_expr->ref_kind != RefKind::None) {
            m_reporter.error(bound_expr->loc, "trait bounds cannot have reference qualifiers");
            continue;
        }

        // Look up the trait by name
        Type* trait_type = m_type_env.trait_type_by_name(bound_expr->name);
        if (!trait_type) {
            m_reporter.error_fmt(bound_expr->loc, "unknown trait '{}' in type parameter bound", bound_expr->name);
            continue;
        }

        // Resolve type args for generic trait bounds (e.g., Add<i32>)
        Vector<Type*> resolved_type_args;
        for (auto* type_arg_expr : bound_expr->type_args) {
            Type* arg_type = m_context.resolve_type_expr(type_arg_expr);
            if (arg_type->is_error()) {
                // Already reported
                continue;
            }
            resolved_type_args.push_back(arg_type);
        }

        // Validate type arg count against trait's type params
        u32 expected_count = trait_type->trait_info.type_params.size();
        if (resolved_type_args.size() != expected_count) {
            m_reporter.error_fmt(bound_expr->loc, "trait '{}' expects {} type arguments but got {}",
                     bound_expr->name, expected_count, (u32)resolved_type_args.size());
            continue;
        }

        TraitBound bound;
        bound.trait = trait_type;
        bound.type_args = m_allocator.alloc_span(resolved_type_args);
        bounds.push_back(bound);
    }

    return m_allocator.alloc_span(bounds);
}

bool GenericCallResolver::resolve_template_bounds(Span<TypeParam> type_params, ResolvedTypeParams& out) {
    bool has_bounds = false;
    for (const auto& type_param : type_params) {
        if (type_param.bounds.size() > 0) { has_bounds = true; break; }
    }
    if (!has_bounds) return false;

    Vector<Span<TraitBound>> all_param_bounds;
    for (const auto& type_param : type_params) {
        all_param_bounds.push_back(resolve_type_param_bounds(type_param.bounds, type_param.loc));
    }
    out.param_bounds = m_allocator.alloc_span(all_param_bounds);
    return true;
}

void GenericCallResolver::resolve_generic_bounds() {
    for (const auto& entry : m_type_env.generics().generic_funs_map()) {
        ResolvedTypeParams resolved;
        if (resolve_template_bounds(entry.second->fun_decl.type_params, resolved)) {
            m_type_env.generics().set_fun_bounds(entry.first, resolved);
        }
    }
    for (const auto& entry : m_type_env.generics().generic_structs_map()) {
        ResolvedTypeParams resolved;
        if (resolve_template_bounds(entry.second->struct_decl.type_params, resolved)) {
            m_type_env.generics().set_struct_bounds(entry.first, resolved);
        }
    }
}

bool GenericCallResolver::check_type_arg_bounds(StringView template_name, Span<Type*> type_args,
                                              const ResolvedTypeParams* bounds, SourceLocation loc) {
    if (!bounds) return true;  // No bounds to check

    bool all_ok = true;
    for (u32 i = 0; i < type_args.size() && i < bounds->param_bounds.size(); i++) {
        Type* concrete_type = type_args[i];
        for (const auto& bound : bounds->param_bounds[i]) {
            // Substitute TypeParam types in bound.type_args with concrete type args
            Vector<Type*> substituted_type_args;
            for (auto* bound_type_arg : bound.type_args) {
                if (bound_type_arg->is_type_param()) {
                    u32 param_index = bound_type_arg->type_param_info.index;
                    if (param_index < type_args.size()) {
                        substituted_type_args.push_back(type_args[param_index]);
                    } else {
                        substituted_type_args.push_back(bound_type_arg);
                    }
                } else {
                    substituted_type_args.push_back(bound_type_arg);
                }
            }

            Span<Type*> subst_args = m_allocator.alloc_span(substituted_type_args);
            bool satisfies = m_types.implements_trait(concrete_type, bound.trait, subst_args);

            if (!satisfies) {
                // Render the required trait (with substituted type args) for the
                // error message: "Trait" or "Trait<A, B>".
                StringView trait_str = bound.trait->trait_info.name;
                if (subst_args.size() > 0) {
                    String args;
                    for (u32 j = 0; j < subst_args.size(); j++) {
                        if (j > 0) args.append(", ", 2);
                        type_to_string(subst_args[j], args);
                    }
                    trait_str = format_to_arena(m_allocator, "{}<{}>",
                                                bound.trait->trait_info.name, args);
                }

                m_reporter.error_fmt(loc,
                    "type '{}' does not implement trait '{}' required by type parameter bound on '{}'",
                    m_checker.type_string(concrete_type), trait_str, template_name);
                all_ok = false;
            }
        }
    }
    return all_ok;
}

// ============================================================================
// Phase B: Definition-site checking of generic template bodies
// ============================================================================

Type* GenericCallResolver::substitute_trait_types(Type* type, Type* type_param, Type* found_in_trait) {
    if (!type) return type;
    if (type->is_self()) return type_param;
    if (type->is_type_param()) {
        // Substitute trait's own type params with the bound's type args
        u32 tp_index = type_param->type_param_info.index;
        if (tp_index < m_active_type_param_bounds.size()) {
            for (const auto& bound : m_active_type_param_bounds[tp_index]) {
                if (bound.trait == found_in_trait && bound.type_args.size() > type->type_param_info.index) {
                    return bound.type_args[type->type_param_info.index];
                }
            }
        }
        // If the type param matches one of our active type params, keep it as-is
        return type;
    }
    return type;
}

const TraitMethodInfo* GenericCallResolver::lookup_type_param_method(
    Type* type_param_type, StringView method_name, Type** found_in_trait) {

    u32 param_index = type_param_type->type_param_info.index;
    if (param_index >= m_active_type_param_bounds.size()) return nullptr;

    for (const auto& bound : m_active_type_param_bounds[param_index]) {
        TraitTypeInfo& trait_info = bound.trait->trait_info;
        // Search methods (including those on this trait)
        for (const auto& method : trait_info.methods) {
            if (method.name == method_name) {
                if (found_in_trait) *found_in_trait = bound.trait;
                return &method;
            }
        }
        // Also check parent trait methods
        Type* parent = trait_info.parent;
        while (parent && parent->is_trait()) {
            for (const auto& method : parent->trait_info.methods) {
                if (method.name == method_name) {
                    if (found_in_trait) *found_in_trait = parent;
                    return &method;
                }
            }
            parent = parent->trait_info.parent;
        }
    }
    return nullptr;
}

Type* GenericCallResolver::analyze_type_param_method_call(
    Expr* expr, CallExpr& call_expr, GetExpr& get_expr, Type* obj_type,
    Type* type_param_type, const TraitMethodInfo* trait_method, Type* found_in_trait) {

    auto substitute = [&](Type* t) -> Type* {
        return substitute_trait_types(t, type_param_type, found_in_trait);
    };

    // Check argument count
    if (call_expr.arguments.size() != trait_method->param_types.size()) {
        m_reporter.error_fmt(expr->loc, "method '{}' expects {} arguments but got {}",
                 trait_method->name, trait_method->param_types.size(), call_expr.arguments.size());
        return substitute(trait_method->return_type);
    }

    // Type-check arguments with substituted parameter types
    for (u32 i = 0; i < call_expr.arguments.size(); i++) {
        Type* arg_type = m_context.analyze_expr(call_expr.arguments[i].expr);
        Type* param_type = substitute(trait_method->param_types[i]);
        if (!arg_type->is_error() && !param_type->is_error()) {
            m_checker.check_assignable(param_type, arg_type, call_expr.arguments[i].expr->loc);
        }
    }

    get_expr.object->resolved_type = obj_type;
    return substitute(trait_method->return_type);
}

void GenericCallResolver::analyze_generic_template_body(Decl* decl) {
    FunDecl& fun_decl = decl->fun_decl;
    StringView func_name = fun_decl.name;

    // Get resolved bounds for this template
    const ResolvedTypeParams* bounds = m_type_env.generics().get_fun_bounds(func_name);

    // Only check bodies of bounded templates (at least one type param has bounds)
    if (!bounds) return;
    bool has_any_bound = false;
    for (u32 i = 0; i < bounds->param_bounds.size(); i++) {
        if (bounds->param_bounds[i].size() > 0) { has_any_bound = true; break; }
    }
    if (!has_any_bound) return;

    // Skip forward declarations (no body) and native functions
    if (!fun_decl.body) return;
    if (fun_decl.is_native) return;

    // Set the bounds context (restored on exit by these guards).
    ScopedValue bounds_guard(m_active_type_param_bounds);
    ScopedValue params_guard(m_active_type_params);
    m_active_type_param_bounds = bounds->param_bounds;
    m_active_type_params = fun_decl.type_params;

    // Phase B is check-only, but the walkers REWRITE the tree they analyze
    // (lambda captures rewrite identifiers to __env reads, generic TypeExprs
    // are mangled in place — the single-shot analysis rule in ast.hpp), and
    // the template's pristine AST is the clone source for every later
    // instantiation. Walk a throwaway identity-substitution clone (no type
    // params bound → a structurally identical copy) so the template stays
    // reusable.
    GenericInstantiator& generics = m_type_env.generics();
    TypeSubstitution identity_substitution{};
    Stmt* checked_body = generics.clone_stmt(fun_decl.body, identity_substitution);

    // Resolve return type (may reference type params → resolves to TypeParam
    // via resolve_type_expr) from a cloned TypeExpr — resolution mutates
    // generic TypeExprs in place.
    Type* return_type = fun_decl.return_type
        ? m_context.resolve_type_expr(
              generics.substitute_type_expr(fun_decl.return_type, identity_substitution))
        : m_types.void_type();

    // Fresh per-function context and lifetime state (same pattern as
    // analyze_fun_body — every body-analysis entry point pushes one).
    FunctionContextScope context_scope(m_function_context, m_lifetimes);

    // Push function scope with return type
    m_symbols.push_function_scope(return_type);

    // Define parameters in scope (cloned TypeExprs, same reason as above)
    for (u32 i = 0; i < fun_decl.params.size(); i++) {
        Param& param = fun_decl.params[i];
        Type* param_type = m_context.resolve_type_expr(
            generics.substitute_type_expr(param.type, identity_substitution));
        m_symbols.define_parameter(param.name, param_type, decl->loc, i,
                                   param.modifier != ParamModifier::None);
    }

    // Analyze the cloned body. Lambdas synthesized during this walk are
    // Phase-B throwaways (each real instantiation synthesizes its own from
    // its own clone) — truncate them so the IR builder never sees
    // TypeParam-typed closure envs.
    u32 synthetic_count_before = m_synthetic_decls.size();
    m_context.analyze_stmt(checked_body);
    m_synthetic_decls.resize(synthetic_count_before);

    m_symbols.pop_scope();
    // context_scope / params_guard / bounds_guard restore on return.
}

// Resolve `name` against the active template's type parameters (set while a
// bounded generic template body is being checked).
Type* GenericCallResolver::resolve_active_type_param(StringView name) {
    for (u32 i = 0; i < m_active_type_params.size(); i++) {
        if (name == m_active_type_params[i].name) {
            return m_types.type_param(m_active_type_params[i].name, i);
        }
    }
    return nullptr;
}

// ============================================================================
// Generic type argument inference
// ============================================================================

bool GenericCallResolver::unify_type_expr(TypeExpr* pattern, Type* concrete,
                                        Span<TypeParam> type_params,
                                        Vector<Type*>& bindings) {
    if (!pattern || !concrete || concrete->is_error()) return false;

    // Check if pattern name matches a type parameter
    for (u32 i = 0; i < type_params.size(); i++) {
        if (pattern->name == type_params[i].name && pattern->type_args.size() == 0) {
            // Default IntLiteral to i32 when binding generic type params
            if (concrete->is_int_literal()) concrete = m_types.i32_type();
            // This is a type parameter reference
            if (bindings[i] == nullptr) {
                bindings[i] = concrete;
                return true;
            }
            // Already bound — check consistency
            return bindings[i] == concrete;
        }
    }

    // Reference types: uniq/ref/weak
    if (pattern->ref_kind != RefKind::None) {
        if (pattern->ref_kind == RefKind::Uniq && concrete->kind == TypeKind::Uniq) {
            // Create a sub-pattern without the ref wrapper
            TypeExpr inner_pattern = *pattern;
            inner_pattern.ref_kind = RefKind::None;
            return unify_type_expr(&inner_pattern, concrete->ref_info.inner_type,
                                   type_params, bindings);
        }
        if (pattern->ref_kind == RefKind::Ref && concrete->kind == TypeKind::Ref) {
            TypeExpr inner_pattern = *pattern;
            inner_pattern.ref_kind = RefKind::None;
            return unify_type_expr(&inner_pattern, concrete->ref_info.inner_type,
                                   type_params, bindings);
        }
        if (pattern->ref_kind == RefKind::Weak && concrete->kind == TypeKind::Weak) {
            TypeExpr inner_pattern = *pattern;
            inner_pattern.ref_kind = RefKind::None;
            return unify_type_expr(&inner_pattern, concrete->ref_info.inner_type,
                                   type_params, bindings);
        }
        return false;
    }

    // List<T> pattern against List type
    if (pattern->name == "List" && pattern->type_args.size() == 1 && concrete->is_list()) {
        return unify_type_expr(pattern->type_args[0], concrete->list_info.element_type,
                               type_params, bindings);
    }

    // Map<K, V> pattern against Map type
    if (pattern->name == "Map" && pattern->type_args.size() == 2 && concrete->is_map()) {
        return unify_type_expr(pattern->type_args[0], concrete->map_info.key_type,
                               type_params, bindings)
            && unify_type_expr(pattern->type_args[1], concrete->map_info.value_type,
                               type_params, bindings);
    }

    // Coro<T> pattern against a coroutine type. Inference binds T from the
    // yield type; note that *passing* the coro still fails assignability
    // afterward (annotated Coro<T> resolves to the interned generic type,
    // while a coroutine value carries its per-function type — see
    // coroutine_type_for_func), but the diagnostic now points at that real
    // limitation instead of a bogus "cannot infer type arguments".
    if (pattern->name == "Coro" && pattern->type_args.size() == 1 && concrete->is_coroutine()) {
        return unify_type_expr(pattern->type_args[0], concrete->coro_info.yield_type,
                               type_params, bindings);
    }

    // Generic struct pattern: e.g., Box<T> against Box$i32
    if (pattern->type_args.size() > 0 && concrete->is_struct()) {
        GenericStructInstance* inst = m_type_env.generics().find_struct_instance_by_type(concrete);
        if (inst) {
            // Verify original template name matches
            Decl* original = inst->original_decl;
            if (original->struct_decl.name != pattern->name) return false;

            // Match type arg count
            if (inst->substitution.concrete_types.size() != pattern->type_args.size())
                return false;

            // Recurse into each type arg
            for (u32 i = 0; i < pattern->type_args.size(); i++) {
                if (!unify_type_expr(pattern->type_args[i],
                                     inst->substitution.concrete_types[i],
                                     type_params, bindings))
                    return false;
            }
            return true;
        }
        return false;
    }

    // Function-kind pattern: `fun(P1, P2) -> R` against a Function concrete type
    if (pattern->kind == TypeExprKind::Function && concrete->kind == TypeKind::Function) {
        Span<Type*> concrete_params = concrete->func_info.param_types;
        if (pattern->type_args.size() != concrete_params.size()) return false;
        for (u32 i = 0; i < pattern->type_args.size(); i++) {
            if (!unify_type_expr(pattern->type_args[i], concrete_params[i],
                                 type_params, bindings)) {
                return false;
            }
        }
        Type* concrete_ret = concrete->func_info.return_type;
        if (pattern->return_type) {
            if (!concrete_ret) return false;
            if (!unify_type_expr(pattern->return_type, concrete_ret,
                                 type_params, bindings)) return false;
        } else {
            // Pattern omits return type ⇒ void
            if (concrete_ret && !concrete_ret->is_void()) return false;
        }
        return true;
    }

    // Concrete name match (primitives, structs, enums)
    if (pattern->type_args.size() == 0) {
        // Resolve the pattern name to a type and compare
        Type* pattern_type = m_type_env.type_by_name(pattern->name);
        if (pattern_type && pattern_type == concrete) return true;

        return false;
    }

    return false;
}

InferredTypeArgs GenericCallResolver::infer_type_args_from_call(
        Span<TypeParam> type_params, Span<Param> params,
        Span<CallArg> args, SourceLocation loc) {
    InferredTypeArgs result;
    result.success = false;
    result.type_args.resize(type_params.size());
    for (u32 i = 0; i < type_params.size(); i++) result.type_args[i] = nullptr;

    // Arg count mismatch — cannot infer
    if (args.size() != params.size()) return result;

    // Analyze each argument to get its concrete type, then unify
    for (u32 i = 0; i < args.size(); i++) {
        Type* arg_type = m_context.analyze_expr(args[i].expr);
        if (!arg_type || arg_type->is_error()) return result;

        if (!unify_type_expr(params[i].type, arg_type, type_params, result.type_args)) {
            return result;
        }
    }

    // Check that all type params were resolved
    for (u32 i = 0; i < type_params.size(); i++) {
        if (result.type_args[i] == nullptr) return result;
    }

    result.success = true;
    return result;
}

InferredTypeArgs GenericCallResolver::infer_type_args_from_fields(
        Span<TypeParam> type_params, Span<FieldDecl> template_fields,
        Span<FieldInit> literal_fields, SourceLocation loc) {
    InferredTypeArgs result;
    result.success = false;
    result.type_args.resize(type_params.size());
    for (u32 i = 0; i < type_params.size(); i++) result.type_args[i] = nullptr;

    // For each literal field, find the matching template field and unify
    for (u32 i = 0; i < literal_fields.size(); i++) {
        // Find matching template field by name
        TypeExpr* field_type_expr = nullptr;
        for (u32 j = 0; j < template_fields.size(); j++) {
            if (template_fields[j].name == literal_fields[i].name) {
                field_type_expr = template_fields[j].type;
                break;
            }
        }
        if (!field_type_expr) return result;  // Unknown field

        Type* value_type = m_context.analyze_expr(literal_fields[i].value);
        if (!value_type || value_type->is_error()) return result;

        if (!unify_type_expr(field_type_expr, value_type, type_params, result.type_args)) {
            return result;
        }
    }

    // Check that all type params were resolved
    for (u32 i = 0; i < type_params.size(); i++) {
        if (result.type_args[i] == nullptr) return result;
    }

    result.success = true;
    return result;
}

Type* GenericCallResolver::analyze_generic_fun_call(Expr* expr, CallExpr& ce, StringView func_name) {
    Decl* template_decl = m_type_env.generics().get_generic_fun_decl(func_name);
    FunDecl& template_fun_decl = template_decl->fun_decl;

    // Validate type arg count
    if (ce.type_args.size() != template_fun_decl.type_params.size()) {
        m_reporter.error_fmt(expr->loc, "generic function '{}' expects {} type arguments but got {}",
                 func_name, template_fun_decl.type_params.size(), ce.type_args.size());
        return m_types.error_type();
    }

    // Resolve type args to concrete types
    Vector<Type*> type_arg_types;
    for (auto& type_arg : ce.type_args) {
        Type* arg_type = m_context.resolve_type_expr(type_arg);
        if (arg_type->is_error()) return m_types.error_type();
        type_arg_types.push_back(arg_type);
    }

    // Check trait bounds on type args
    Span<Type*> type_args = m_allocator.alloc_span(type_arg_types);
    const ResolvedTypeParams* bounds = m_type_env.generics().get_fun_bounds(func_name);
    if (!check_type_arg_bounds(func_name, type_args, bounds, expr->loc)) {
        return m_types.error_type();
    }

    // Instantiate the generic function and type-check the call against it.
    StringView mangled = m_type_env.generics().instantiate_fun(func_name, type_args);
    ce.mangled_name = mangled;
    GenericFunInstance* inst = m_type_env.generics().find_fun_instance(mangled);
    return check_instantiated_generic_call(expr, ce, func_name, inst, /*args_pre_analyzed=*/false);
}

Type* GenericCallResolver::analyze_generic_fun_call_inferred(Expr* expr, CallExpr& ce, StringView func_name) {
    Decl* template_decl = m_type_env.generics().get_generic_fun_decl(func_name);
    FunDecl& template_fun_decl = template_decl->fun_decl;

    // Infer type args from the call arguments (this analyzes each argument).
    InferredTypeArgs inferred = infer_type_args_from_call(
        template_fun_decl.type_params, template_fun_decl.params,
        ce.arguments, expr->loc);
    if (!inferred.success) {
        m_reporter.error_fmt(expr->loc,
            "cannot infer type arguments for generic function '{}'; "
            "provide explicit type arguments", func_name);
        return m_types.error_type();
    }

    // Check trait bounds on the inferred type args
    Span<Type*> type_args = m_allocator.alloc_span(inferred.type_args);
    const ResolvedTypeParams* bounds = m_type_env.generics().get_fun_bounds(func_name);
    if (!check_type_arg_bounds(func_name, type_args, bounds, expr->loc)) {
        return m_types.error_type();
    }

    // Instantiate and type-check. Arguments were already analyzed during
    // inference, so the shared tail reads their resolved types directly.
    StringView mangled = m_type_env.generics().instantiate_fun(func_name, type_args);
    ce.mangled_name = mangled;
    GenericFunInstance* inst = m_type_env.generics().find_fun_instance(mangled);
    return check_instantiated_generic_call(expr, ce, func_name, inst, /*args_pre_analyzed=*/true);
}

Type* GenericCallResolver::check_instantiated_generic_call(
        Expr* expr, CallExpr& ce, StringView func_name,
        GenericFunInstance* inst, bool args_pre_analyzed) {
    FunDecl& inst_fun_decl = inst->instantiated_decl->fun_decl;

    if (ce.arguments.size() != inst_fun_decl.params.size()) {
        m_reporter.error_fmt(expr->loc, "function '{}' expects {} arguments but got {}",
                 func_name, inst_fun_decl.params.size(), ce.arguments.size());
        return m_types.error_type();
    }

    // Resolve each parameter type from the instantiated function and
    // type-check the corresponding argument.
    Vector<Type*> resolved_param_types;
    resolved_param_types.reserve(ce.arguments.size());
    for (u32 i = 0; i < ce.arguments.size(); i++) {
        CallArg& arg = ce.arguments[i];
        // On the inference path the arguments were already analyzed in
        // infer_type_args_from_call; re-analyzing would be redundant.
        Type* arg_type = args_pre_analyzed ? arg.expr->resolved_type
                                           : m_context.analyze_expr(arg.expr);

        Type* param_type = nullptr;
        if (inst_fun_decl.params[i].type) {
            param_type = m_context.resolve_type_expr(inst_fun_decl.params[i].type);
        }
        resolved_param_types.push_back(param_type ? param_type : m_types.error_type());

        // Generic-template-ref arg against the substituted param type.
        if (param_type && coerce_generic_template_ref(arg.expr, param_type)) {
            arg_type = arg.expr->resolved_type;
        }

        if (param_type && !param_type->is_error() && arg_type && !arg_type->is_error()) {
            m_checker.check_assignable(param_type, arg_type, arg.expr->loc);
            m_checker.coerce_int_literal(arg.expr, param_type);
        }

        // Move semantics: passing an owned arg to a noncopyable param consumes it.
        // Mirrors check_call_args; the non-generic path goes through that helper.
        if (param_type && param_type->noncopyable()
            && arg.modifier == ParamModifier::None) {
            m_lifetimes.consume_noncopyable(arg.expr, arg.expr->loc);
        }
    }

    // Resolve return type from the instantiated function.
    Type* return_type = m_types.void_type();
    if (inst_fun_decl.return_type) {
        return_type = m_context.resolve_type_expr(inst_fun_decl.return_type);
    }

    // Record the instantiated function type on the callee so the IR builder
    // can read param types for post-call move/nullify decisions on
    // noncopyable arguments.
    if (ce.callee) {
        ce.callee->resolved_type = m_types.function_type(
            m_allocator.alloc_span(resolved_param_types), return_type);
    }

    return return_type;
}

// ===== Generic-template refs in value position =====

bool GenericCallResolver::coerce_generic_template_ref(Expr* expr, Type* expected) {
    if (!expr || expr->kind != AstKind::ExprIdentifier) return true;
    IdentifierExpr& id = expr->identifier;
    if (!id.is_generic_template_ref) return true;
    if (!expected || !expected->is_function()) {
        m_reporter.error_fmt(expr->loc,
            "cannot use generic function '{}' as a value here; it needs a "
            "concrete function-type context (e.g. a typed variable or a "
            "typed function parameter) to bind the type parameters", id.name);
        return false;
    }

    Decl* template_decl = m_type_env.generics().get_generic_fun_decl(id.name);
    if (!template_decl || template_decl->kind != AstKind::DeclFun) {
        m_reporter.error_fmt(expr->loc, "internal error: missing template decl for '{}'", id.name);
        return false;
    }
    FunDecl& template_fun_decl = template_decl->fun_decl;
    FunctionTypeInfo& expected_fti = expected->func_info;

    // Param-count mismatch ⇒ no plausible binding.
    if (template_fun_decl.params.size() != expected_fti.param_types.size()) {
        m_reporter.error_fmt(expr->loc,
            "generic function '{}' has {} parameters but expected function "
            "type takes {}", id.name,
            template_fun_decl.params.size(), expected_fti.param_types.size());
        return false;
    }

    // Bind type params by unifying each template param TypeExpr against the
    // corresponding concrete param type, then the return TypeExpr against the
    // concrete return type. unify_type_expr already handles Function-kind
    // patterns so nested fun(T)->T params bind too.
    Vector<Type*> bindings;
    bindings.resize(template_fun_decl.type_params.size());
    for (u32 i = 0; i < bindings.size(); i++) bindings[i] = nullptr;
    for (u32 i = 0; i < template_fun_decl.params.size(); i++) {
        if (!unify_type_expr(template_fun_decl.params[i].type,
                             expected_fti.param_types[i],
                             template_fun_decl.type_params, bindings)) {
            m_reporter.error_fmt(expr->loc,
                "cannot bind type parameters of '{}' against expected "
                "function type at parameter {}", id.name, i);
            return false;
        }
    }
    if (template_fun_decl.return_type) {
        Type* concrete_ret = expected_fti.return_type
            ? expected_fti.return_type : m_types.void_type();
        if (!unify_type_expr(template_fun_decl.return_type, concrete_ret,
                             template_fun_decl.type_params, bindings)) {
            m_reporter.error_fmt(expr->loc,
                "cannot bind type parameters of '{}' against expected return type",
                id.name);
            return false;
        }
    }
    for (u32 i = 0; i < bindings.size(); i++) {
        if (!bindings[i]) {
            m_reporter.error_fmt(expr->loc,
                "cannot infer type parameter '{}' of generic function '{}'",
                template_fun_decl.type_params[i].name, id.name);
            return false;
        }
    }

    // Check trait bounds, instantiate, and stash the monomorphized name.
    Span<Type*> type_args = m_allocator.alloc_span(bindings);
    const ResolvedTypeParams* bounds = m_type_env.generics().get_fun_bounds(id.name);
    if (!check_type_arg_bounds(id.name, type_args, bounds, expr->loc)) {
        return false;
    }
    StringView mangled = m_type_env.generics().instantiate_fun(id.name, type_args);
    id.mangled_name = mangled;
    id.is_generic_template_ref = false;
    expr->resolved_type = expected;
    return true;
}

Type* GenericCallResolver::resolve_explicit_generic_template_ref(Expr* expr) {
    IdentifierExpr& id = expr->identifier;
    Decl* template_decl = m_type_env.generics().get_generic_fun_decl(id.name);
    if (!template_decl || template_decl->kind != AstKind::DeclFun) {
        m_reporter.error_fmt(expr->loc, "internal error: missing template decl for '{}'", id.name);
        return m_types.error_type();
    }
    FunDecl& template_fun_decl = template_decl->fun_decl;
    if (id.generic_args.size() != template_fun_decl.type_params.size()) {
        m_reporter.error_fmt(expr->loc,
            "generic function '{}' expects {} type arguments but got {}",
            id.name, template_fun_decl.type_params.size(), id.generic_args.size());
        return m_types.error_type();
    }

    Vector<Type*> type_arg_types;
    type_arg_types.reserve(id.generic_args.size());
    for (auto* arg_expr : id.generic_args) {
        Type* arg_type = m_context.resolve_type_expr(arg_expr);
        if (arg_type->is_error()) return m_types.error_type();
        type_arg_types.push_back(arg_type);
    }

    Span<Type*> type_args = m_allocator.alloc_span(type_arg_types);
    const ResolvedTypeParams* bounds = m_type_env.generics().get_fun_bounds(id.name);
    if (!check_type_arg_bounds(id.name, type_args, bounds, expr->loc)) {
        return m_types.error_type();
    }

    StringView mangled = m_type_env.generics().instantiate_fun(id.name, type_args);
    id.mangled_name = mangled;

    // Build the instantiated function type from the post-substitution decl,
    // matching what gen_function_ref reads off expr->resolved_type.
    GenericFunInstance* inst = m_type_env.generics().find_fun_instance(mangled);
    FunDecl& inst_decl = inst->instantiated_decl->fun_decl;
    Vector<Type*> param_types;
    param_types.reserve(inst_decl.params.size());
    for (auto& p : inst_decl.params) {
        param_types.push_back(m_context.resolve_type_expr(p.type));
    }
    Type* ret_type = inst_decl.return_type
        ? m_context.resolve_type_expr(inst_decl.return_type) : m_types.void_type();
    Type* fn_type = m_types.function_type(
        m_allocator.alloc_span(param_types), ret_type);
    expr->resolved_type = fn_type;
    return fn_type;
}

}
