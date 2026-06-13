#include "roxy/compiler/ir_builder.hpp"
#include "roxy/compiler/operator_traits.hpp"
#include "roxy/compiler/generics.hpp"
#include "roxy/compiler/module_registry.hpp"
#include "roxy/core/format.hpp"
#include "roxy/vm/binding/registry.hpp"
#include "roxy/vm/map.hpp"

#include <cassert>
#include <climits>
#include <cstring>

namespace rx {

// get_type_slot_count is declared in types.hpp and defined in types.cpp.

IRBuilder::IRBuilder(BumpAllocator& allocator, TypeEnv& type_env, NativeRegistry& registry,
                     SymbolTable& symbols, ModuleRegistry& module_registry)
    : m_allocator(allocator)
    , m_type_env(type_env)
    , m_types(type_env.types())
    , m_registry(registry)
    , m_symbols(symbols)
    , m_module_registry(module_registry)
    , m_current_func(nullptr)
    , m_current_block(nullptr)
    , m_has_error(false)
    , m_error(nullptr)
{
}

void IRBuilder::report_error(const char* message) {
    if (!m_has_error) {
        m_has_error = true;
        m_error = message;
    }
}

IRModule* IRBuilder::build(Program* program, Span<Decl*> synthetic_decls) {
    m_has_error = false;
    m_error = nullptr;
    m_module_name = program->module_name;
    m_module = m_allocator.emplace<IRModule>();

    // Maps a struct (or generic-instance mangled) name -> whether it has a
    // user-defined default constructor. Written by the user-decl and generic
    // struct-member phases; read by the synthesized-default-ctor phase.
    tsl::robin_map<StringView, bool> has_default_ctor;

    // Each phase appends to m_module and records failures via m_has_error; bail out
    // between phases so a later phase never runs on a half-built module.
    build_user_decls(program, has_default_ctor);
    if (m_has_error) return nullptr;

    build_synthetic_decls(synthetic_decls);
    if (m_has_error) return nullptr;

    build_generic_fun_instances();
    if (m_has_error) return nullptr;

    build_generic_struct_ctors_dtors(has_default_ctor);
    if (m_has_error) return nullptr;

    build_synthesized_default_ctors(program, has_default_ctor);
    if (m_has_error) return nullptr;

    build_generic_struct_methods();
    if (m_has_error) return nullptr;

    build_synthesized_default_dtors(program);
    if (m_has_error) return nullptr;

    build_coroutine_cleanup_wrappers();
    if (m_has_error) return nullptr;

    collect_backend_types(program);
    return m_module;
}

void IRBuilder::build_user_decls(Program* program, tsl::robin_map<StringView, bool>& has_default_ctor) {
    for (auto* decl : program->declarations) {
        if (!decl) continue;

        if (decl->kind == AstKind::DeclFun) {
            // Skip generic function templates (they are instantiated separately)
            if (decl->fun_decl.type_params.size() > 0) continue;
            if (!decl->fun_decl.is_native && decl->fun_decl.body) {
                IRFunction* func = build_function(&decl->fun_decl);
                m_module->functions.push_back(func);
            }
        }
        else if (decl->kind == AstKind::DeclStruct) {
            // Skip generic struct templates
            if (decl->struct_decl.type_params.size() > 0) continue;
            // Build methods
            StructDecl& struct_decl = decl->struct_decl;
            for (auto* method : struct_decl.methods) {
                if (method && !method->is_native && method->body) {
                    IRFunction* func = build_function(method);
                    m_module->functions.push_back(func);
                }
            }
        }
        else if (decl->kind == AstKind::DeclConstructor) {
            // Skip generic struct constructor templates (handled below with instances)
            if (decl->constructor_decl.type_params.size() > 0) continue;
            // Build constructor
            ConstructorDecl& constructor_decl = decl->constructor_decl;
            Type* struct_type = m_type_env.named_type_by_name(constructor_decl.struct_name);
            if (struct_type && constructor_decl.body) {
                IRFunction* func = build_constructor(&constructor_decl, struct_type);
                m_module->functions.push_back(func);
                // Track if this is a default constructor (no name)
                if (constructor_decl.name.empty()) {
                    has_default_ctor[constructor_decl.struct_name] = true;
                }
            }
        }
        else if (decl->kind == AstKind::DeclDestructor) {
            // Skip generic struct destructor templates (handled below with instances)
            if (decl->destructor_decl.type_params.size() > 0) continue;
            // Build destructor
            DestructorDecl& destructor_decl = decl->destructor_decl;
            Type* struct_type = m_type_env.named_type_by_name(destructor_decl.struct_name);
            if (struct_type && destructor_decl.body) {
                IRFunction* func = build_destructor(&destructor_decl, struct_type);
                m_module->functions.push_back(func);
            }
        }
        else if (decl->kind == AstKind::DeclMethod) {
            // Build method - skip trait method declarations (struct_name is a trait, not a struct)
            MethodDecl& method_decl = decl->method_decl;
            // Skip generic struct method templates (handled below with instances)
            if (method_decl.type_params.size() > 0) continue;
            Type* struct_type = m_type_env.named_type_by_name(method_decl.struct_name);
            if (struct_type && struct_type->is_struct() && method_decl.body) {
                IRFunction* func = build_method(&method_decl, struct_type);
                m_module->functions.push_back(func);
            }
        }
    }
}

void IRBuilder::build_synthetic_decls(Span<Decl*> synthetic_decls) {
    // Process synthetic (injected default method, lifted lambda) declarations
    for (auto* decl : synthetic_decls) {
        if (!decl) continue;
        if (decl->kind == AstKind::DeclMethod) {
            MethodDecl& method_decl = decl->method_decl;
            Type* struct_type = m_type_env.named_type_by_name(method_decl.struct_name);
            if (struct_type && struct_type->is_struct() && method_decl.body) {
                IRFunction* func = build_method(&method_decl, struct_type);
                m_module->functions.push_back(func);
            }
        } else if (decl->kind == AstKind::DeclFun) {
            if (!decl->fun_decl.is_native && decl->fun_decl.body) {
                IRFunction* func = build_function(&decl->fun_decl);
                m_module->functions.push_back(func);
            }
        }
    }
}

void IRBuilder::build_generic_fun_instances() {
    // Process generic function instances. Each instance is emitted from its
    // template's defining module only (so the body resolves against that
    // module's symbol table), avoiding both duplicate emission across
    // modules and cross-module symbol-resolution failures. Instances whose
    // template_module is empty (e.g. before the cross-module pipeline ran,
    // or in single-module compilations) fall through to the current module.
    for (auto* instance : m_type_env.generics().all_fun_instances()) {
        if (!instance->is_analyzed || !instance->instantiated_decl) continue;
        bool owns =
            instance->template_module.empty() ||
            m_module_name.empty() ||
            instance->template_module == m_module_name;
        if (!owns) continue;
        IRFunction* func = build_function(&instance->instantiated_decl->fun_decl);
        m_module->functions.push_back(func);
    }
}

void IRBuilder::build_generic_struct_ctors_dtors(tsl::robin_map<StringView, bool>& has_default_ctor) {
    // Generate constructors/destructors for generic struct instances
    for (auto* instance : m_type_env.generics().all_struct_instances()) {
        if (!instance->is_analyzed || !instance->concrete_type) continue;

        for (Decl* ctor_decl : instance->instantiated_constructors) {
            ConstructorDecl& ctor = ctor_decl->constructor_decl;
            if (ctor.body) {
                IRFunction* func = build_constructor(&ctor, instance->concrete_type);
                m_module->functions.push_back(func);
                if (ctor.name.empty()) {
                    has_default_ctor[instance->mangled_name] = true;
                }
            }
        }

        for (Decl* dtor_decl : instance->instantiated_destructors) {
            DestructorDecl& dtor = dtor_decl->destructor_decl;
            if (dtor.body) {
                IRFunction* func = build_destructor(&dtor, instance->concrete_type);
                m_module->functions.push_back(func);
            }
        }
    }
}

void IRBuilder::build_synthesized_default_ctors(Program* program,
                                                const tsl::robin_map<StringView, bool>& has_default_ctor) {
    // Generate synthesized default constructors for structs without user-defined ones
    for (auto* decl : program->declarations) {
        if (!decl) continue;

        if (decl->kind == AstKind::DeclStruct) {
            // Skip generic struct templates
            if (decl->struct_decl.type_params.size() > 0) continue;
            StructDecl& struct_decl = decl->struct_decl;
            // Check if this struct already has a user-defined default constructor
            if (has_default_ctor.find(struct_decl.name) == has_default_ctor.end()) {
                Type* struct_type = m_type_env.named_type_by_name(struct_decl.name);
                if (struct_type) {
                    IRFunction* func = build_synthesized_default_constructor(struct_type);
                    m_module->functions.push_back(func);
                }
            }
        }
    }

    if (m_has_error) return;

    // Generate synthesized default constructors for generic struct instances
    for (auto* instance : m_type_env.generics().all_struct_instances()) {
        if (instance->is_analyzed && instance->concrete_type) {
            // Check if there's a user-defined default constructor for this instance
            if (has_default_ctor.find(instance->mangled_name) == has_default_ctor.end()) {
                IRFunction* func = build_synthesized_default_constructor(instance->concrete_type);
                m_module->functions.push_back(func);
            }
        }
    }
}

void IRBuilder::build_generic_struct_methods() {
    // Generate external methods for generic struct instances
    for (auto* instance : m_type_env.generics().all_struct_instances()) {
        if (!instance->is_analyzed || !instance->concrete_type) continue;

        for (Decl* method_decl : instance->instantiated_methods) {
            MethodDecl& method = method_decl->method_decl;
            if (method.body) {
                IRFunction* func = build_method(&method, instance->concrete_type);
                m_module->functions.push_back(func);
            }
        }
    }
}

void IRBuilder::build_synthesized_default_dtors(Program* program) {
    // Generate synthesized default destructors for structs with uniq fields
    for (auto* decl : program->declarations) {
        if (!decl) continue;

        if (decl->kind == AstKind::DeclStruct) {
            // Skip generic struct templates
            if (decl->struct_decl.type_params.size() > 0) continue;
            Type* struct_type = m_type_env.named_type_by_name(decl->struct_decl.name);
            if (!struct_type || !struct_type->is_struct()) continue;

            // Check for synthetic default destructor (decl == nullptr)
            for (const auto& dtor : struct_type->struct_info.destructors) {
                if (dtor.name.empty() && dtor.decl == nullptr) {
                    IRFunction* func = build_synthesized_default_destructor(struct_type);
                    m_module->functions.push_back(func);
                    break;
                }
            }
        }
    }

    if (m_has_error) return;

    // Generate synthesized default destructors for generic struct instances with uniq fields
    for (auto* instance : m_type_env.generics().all_struct_instances()) {
        if (instance->is_analyzed && instance->concrete_type) {
            Type* concrete_type = instance->concrete_type;
            if (!concrete_type->is_struct()) continue;

            // Check for synthetic default destructor (decl == nullptr)
            for (const auto& dtor : concrete_type->struct_info.destructors) {
                if (dtor.name.empty() && dtor.decl == nullptr) {
                    IRFunction* func = build_synthesized_default_destructor(concrete_type);
                    m_module->functions.push_back(func);
                    break;
                }
            }
        }
    }

    // Synthesized closure-env structs aren't in program->declarations, so build
    // their destructors here. An env gets one only when it has cleanup-needing
    // captures (a noncopyable/ref capture made backfill_lambda_env attach a
    // synthetic default destructor). The closure delete dispatches it by type_id.
    for (Type* env_type : m_env_struct_types) {
        if (!env_type || !env_type->is_struct()) continue;
        for (const auto& dtor : env_type->struct_info.destructors) {
            if (dtor.name.empty() && dtor.decl == nullptr) {
                IRFunction* func = build_synthesized_default_destructor(env_type);
                m_module->functions.push_back(func);
                break;
            }
        }
    }
}

void IRBuilder::build_coroutine_cleanup_wrappers() {
    // Generate cleanup wrapper functions for noncopyable List/Map types in coroutines
    tsl::robin_map<Type*, bool> seen_types;
    Vector<IRFunction*> wrappers;
    u32 wrapper_index = 0;

    for (auto* func : m_module->functions) {
        if (!func->is_coroutine) continue;

        // Scan cleanup_info for noncopyable List/Map types
        for (const auto& cleanup : func->cleanup_info) {
            if (!cleanup.type) continue;
            if (cleanup.type->is_container() &&
                cleanup.type->noncopyable() &&
                seen_types.find(cleanup.type) == seen_types.end()) {
                seen_types[cleanup.type] = true;
                IRFunction* wrapper = build_cleanup_wrapper(cleanup.type, wrapper_index++);
                wrappers.push_back(wrapper);
                m_module->cleanup_wrappers[cleanup.type] = wrapper->name;
            }
        }

        // Scan parameters for noncopyable List/Map types
        for (const auto& param : func->params) {
            if (!param.type) continue;
            if (param.type->is_container() &&
                param.type->noncopyable() &&
                seen_types.find(param.type) == seen_types.end()) {
                seen_types[param.type] = true;
                IRFunction* wrapper = build_cleanup_wrapper(param.type, wrapper_index++);
                wrappers.push_back(wrapper);
                m_module->cleanup_wrappers[param.type] = wrapper->name;
            }
        }
    }

    // Add wrappers after iteration to avoid invalidating the iterator
    for (auto* wrapper : wrappers) {
        m_module->functions.push_back(wrapper);
    }
}

void IRBuilder::collect_backend_types(Program* program) {
    // Collect struct and enum types for C backend code generation
    for (auto* decl : program->declarations) {
        if (!decl) continue;
        if (decl->kind == AstKind::DeclStruct) {
            // Skip generic struct templates (they are instantiated separately)
            if (decl->struct_decl.type_params.size() > 0) continue;
            Type* struct_type = m_type_env.named_type_by_name(decl->struct_decl.name);
            if (struct_type) {
                m_module->struct_types.push_back(struct_type);
            }
        }
        else if (decl->kind == AstKind::DeclEnum) {
            Type* enum_type = m_type_env.named_type_by_name(decl->enum_decl.name);
            if (enum_type) {
                m_module->enum_types.push_back(enum_type);
            }
        }
    }

    // Collect monomorphized generic struct instances
    for (auto* instance : m_type_env.generics().all_struct_instances()) {
        if (instance->is_analyzed && instance->concrete_type) {
            m_module->struct_types.push_back(instance->concrete_type);
        }
    }
}

IRFunction* IRBuilder::build_function(FunDecl* decl) {
    m_current_func = m_allocator.emplace<IRFunction>();
    // Non-pub functions are scoped to their module so they don't collide at link time.
    // "main" is the program entry point convention — leave it un-mangled so the host
    // can still invoke it via vm_call(&vm, "main", {}).
    if (!decl->is_pub && decl->name != StringView("main", 4)) {
        m_current_func->name = mangle_module_local(decl->name);
    } else {
        m_current_func->name = decl->name;
    }
    m_current_func->is_pub = decl->is_pub;
    // Source line for AOT `#line` directives. Use the body's first line —
    // typically the same as the function header or the next line after.
    if (decl->body) m_current_func->source_line = decl->body->loc.line;

    // Set up parameters
    setup_parameters(decl->params);

    // Resolve return type - check the symbol table first (semantic analysis already resolved it)
    if (decl->return_type) {
        // Look up the function's resolved type from the symbol table
        Symbol* func_sym = m_symbols.lookup(decl->name);
        if (func_sym && func_sym->type && func_sym->type->is_function()) {
            m_current_func->return_type = func_sym->type->func_info.return_type;
        } else {
            m_current_func->return_type = m_type_env.type_by_name(decl->return_type->name);
            if (!m_current_func->return_type) {
                m_current_func->return_type = m_types.void_type();
            }
            m_current_func->return_type = apply_ref_kind(m_current_func->return_type, decl->return_type->ref_kind);
        }
    } else {
        m_current_func->return_type = m_types.void_type();
    }

    // Detect coroutine function (returns Coro<T>)
    if (m_current_func->return_type && m_current_func->return_type->is_coroutine()) {
        m_current_func->is_coroutine = true;
        m_current_func->coro_type = m_current_func->return_type;
        m_current_func->coro_yield_type = m_current_func->return_type->coro_info.yield_type;
        m_current_func->coro_struct_type = m_current_func->return_type->coro_info.generated_struct_type;
    }

    // Check for large struct return - add hidden output pointer as last parameter
    if (m_current_func->returns_large_struct()) {
        BlockParam hidden_param;
        hidden_param.value = m_current_func->new_value();
        hidden_param.type = m_current_func->return_type;  // Pointer to struct
        hidden_param.name = "__ret_ptr";
        m_current_func->params.push_back(hidden_param);
        m_current_func->param_is_ptr.push_back(true);
    }

    // Begin function body
    begin_function_body(true);  // skip hidden return pointer

    // Generate body
    if (decl->body && decl->body->kind == AstKind::StmtBlock) {
        BlockStmt& block = decl->body->block;
        for (auto* decl : block.declarations) {
            gen_decl(decl);
        }
    }

    // End function body
    end_function_body();

    IRFunction* result = m_current_func;
    m_current_func = nullptr;
    m_current_block = nullptr;
    return result;
}

IRFunction* IRBuilder::build_constructor(ConstructorDecl* decl, Type* struct_type) {
    m_current_func = m_allocator.emplace<IRFunction>();
    m_current_func->name = mangle_constructor(decl->struct_name, decl->name);
    m_current_func->is_pub = decl->is_pub;
    if (decl->body) m_current_func->source_line = decl->body->loc.line;

    // Set up parameters with 'self' as first parameter
    setup_parameters(decl->params, struct_type);

    // Constructors return void
    m_current_func->return_type = m_types.void_type();

    // Begin function body
    begin_function_body(false);

    // Check if struct has a parent - if so, we may need to call parent constructor
    Type* parent_type = struct_type->struct_info.parent;

    // Detect if first statement is an explicit super() call
    bool has_explicit_super = false;
    if (decl->body && decl->body->kind == AstKind::StmtBlock) {
        BlockStmt& block = decl->body->block;
        if (block.declarations.size() > 0) {
            Decl* first_decl = block.declarations[0];
            if (first_decl && first_decl->kind == AstKind::StmtExpr) {
                Expr* first_expr = first_decl->stmt.expr_stmt.expr;
                if (first_expr && first_expr->kind == AstKind::ExprCall) {
                    CallExpr& call = first_expr->call;
                    if (call.callee && call.callee->kind == AstKind::ExprSuper) {
                        has_explicit_super = true;
                    }
                }
            }
        }
    }

    // If struct has parent and no explicit super(), call parent's default constructor
    if (parent_type && !has_explicit_super) {
        StringView parent_ctor_name = mangle_constructor(parent_type->struct_info.name);
        Span<ValueId> ctor_args = alloc_span<ValueId>(1);
        ctor_args[0] = m_current_func->params[0].value;  // 'self' is first parameter
        emit_call(parent_ctor_name, ctor_args, m_types.void_type());
    }

    // Zero-init this struct's own slot range before the user body runs.
    // `self.field = …` inside the body goes through gen_assign_expr, which
    // emits a destroy-of-the-old-value preamble for uniq/noncopyable fields —
    // at constructor entry those fields hold whatever bytes the caller left
    // in the return slot (often a pointer from a previous call that has
    // already been freed on a reused local_stack slot). A bulk zero clears
    // all own slots at once, which:
    //   * makes the destroy-old preamble a safe `Delete(null)` no-op;
    //   * doesn't need per-field / per-variant iteration, so variant fields
    //     (whose slots alias across cases) and copyable fields all fall
    //     under the same blanket;
    //   * leaves inherited fields untouched — the parent constructor call
    //     above has already populated those.
    {
        u32 inherited_slot_count = parent_type ? parent_type->struct_info.slot_count : 0;
        u32 total_slots = struct_type->struct_info.slot_count;
        if (total_slots > inherited_slot_count) {
            emit_zero_slots(m_current_func->params[0].value,
                            inherited_slot_count,
                            total_slots - inherited_slot_count);
        }
    }

    // Generate body
    if (decl->body && decl->body->kind == AstKind::StmtBlock) {
        BlockStmt& block = decl->body->block;
        for (auto* decl : block.declarations) {
            gen_decl(decl);
        }
    }

    // End function body
    end_function_body();

    IRFunction* result = m_current_func;
    m_current_func = nullptr;
    m_current_block = nullptr;
    return result;
}

IRFunction* IRBuilder::build_destructor(DestructorDecl* decl, Type* struct_type) {
    m_current_func = m_allocator.emplace<IRFunction>();
    m_current_func->name = mangle_destructor(decl->struct_name, decl->name);
    m_current_func->is_pub = decl->is_pub;
    if (decl->body) m_current_func->source_line = decl->body->loc.line;

    // Set up parameters with 'self' as first parameter
    setup_parameters(decl->params, struct_type);

    // Destructors return void
    m_current_func->return_type = m_types.void_type();

    // Begin function body
    begin_function_body(false);

    // Generate body
    if (decl->body && decl->body->kind == AstKind::StmtBlock) {
        BlockStmt& block = decl->body->block;
        for (auto* decl : block.declarations) {
            gen_decl(decl);
        }
    }

    // After child destructor body, call parent's default destructor if present
    // Only chain default destructors (named destructors are called explicitly)
    Type* parent_type = struct_type->struct_info.parent;
    if (parent_type && decl->name.empty()) {
        StringView parent_dtor_name = mangle_destructor(parent_type->struct_info.name);
        Span<ValueId> dtor_args = alloc_span<ValueId>(1);
        dtor_args[0] = m_current_func->params[0].value;  // 'self' is first parameter
        emit_call(parent_dtor_name, dtor_args, m_types.void_type());
    }

    // For default destructors, clean up uniq fields after user body and parent chain
    if (decl->name.empty()) {
        emit_field_cleanup(m_current_func->params[0].value, struct_type);
    }

    // End function body
    end_function_body();

    IRFunction* result = m_current_func;
    m_current_func = nullptr;
    m_current_block = nullptr;
    return result;
}

IRFunction* IRBuilder::build_method(MethodDecl* decl, Type* struct_type) {
    m_current_func = m_allocator.emplace<IRFunction>();
    m_current_func->name = mangle_method(decl->struct_name, decl->name);
    m_current_func->is_pub = decl->is_pub;
    if (decl->body) m_current_func->source_line = decl->body->loc.line;

    // Set up parameters with 'self' as first parameter
    setup_parameters(decl->params, struct_type);

    // Resolve return type
    if (decl->return_type) {
        m_current_func->return_type = m_type_env.type_by_name(decl->return_type->name);
        if (!m_current_func->return_type) {
            m_current_func->return_type = m_types.void_type();
        }
        m_current_func->return_type = apply_ref_kind(m_current_func->return_type, decl->return_type->ref_kind);
    } else {
        m_current_func->return_type = m_types.void_type();
    }

    // Check for large struct return - add hidden output pointer as last parameter
    if (m_current_func->returns_large_struct()) {
        BlockParam hidden_param;
        hidden_param.value = m_current_func->new_value();
        hidden_param.type = m_current_func->return_type;
        hidden_param.name = "__ret_ptr";
        m_current_func->params.push_back(hidden_param);
        m_current_func->param_is_ptr.push_back(true);
    }

    // Begin function body
    begin_function_body(true);  // skip hidden return pointer

    // Generate body
    if (decl->body && decl->body->kind == AstKind::StmtBlock) {
        BlockStmt& block = decl->body->block;
        for (auto* decl : block.declarations) {
            gen_decl(decl);
        }
    }

    // End function body
    end_function_body();

    IRFunction* result = m_current_func;
    m_current_func = nullptr;
    m_current_block = nullptr;
    return result;
}

IRFunction* IRBuilder::build_synthesized_default_constructor(Type* struct_type) {
    m_current_func = m_allocator.emplace<IRFunction>();

    StructTypeInfo& struct_type_info = struct_type->struct_info;
    m_current_func->name = mangle_constructor(struct_type_info.name);
    m_current_func->is_pub = struct_type_info.decl
        && struct_type_info.decl->struct_decl.is_pub;

    // Set up parameters - only 'self'
    setup_parameters({}, struct_type);

    // Constructor returns void
    m_current_func->return_type = m_types.void_type();

    // Begin function body
    begin_function_body(false);

    // Get 'self' parameter value
    BlockParam& self_param = m_current_func->params[0];

    // If struct has parent, call parent's default constructor first
    Type* parent_type = struct_type->struct_info.parent;
    if (parent_type) {
        StringView parent_ctor_name = mangle_constructor(parent_type->struct_info.name);
        Span<ValueId> ctor_args = alloc_span<ValueId>(1);
        ctor_args[0] = self_param.value;
        emit_call(parent_ctor_name, ctor_args, m_types.void_type());
    }

    // Get the number of inherited fields (from parent)
    u32 inherited_field_count = parent_type ? parent_type->struct_info.fields.size() : 0;

    // Generate field initialization code (only for own fields, not inherited)
    StructDecl& struct_decl = struct_type_info.decl->struct_decl;
    ValueId self_ptr = self_param.value;

    // Helper lambda to zero-initialize a field
    auto zero_init_field = [&](const FieldInfo& field_info) -> ValueId {
        if (field_info.type->is_bool()) {
            return emit_const_bool(false);
        } else if (field_info.type->is_integer() || field_info.type->is_enum()) {
            return emit_const_int(0, field_info.type);
        } else if (field_info.type->is_float()) {
            return emit_const_float(0.0, field_info.type);
        } else if (field_info.type->kind == TypeKind::String) {
            return emit_const_string("");
        } else {
            return emit_const_null();
        }
    };

    // Initialize regular fields from struct_decl.fields
    for (const auto& field : struct_decl.fields) {
        // Find the corresponding FieldInfo in struct_type_info.fields
        const FieldInfo* field_info = struct_type_info.find_field(field.name);
        if (!field_info) continue;

        ValueId value;
        if (field.default_value) {
            value = gen_expr(field.default_value);
        } else if (field_info->type && field_info->type->is_struct()) {
            // For nested structs, recursively zero-init
            u32 nested_slots = field_info->type->struct_info.slot_count;
            ValueId nested_ptr = emit_stack_alloc(nested_slots, field_info->type);
            // Zero the nested struct fields
            StructTypeInfo& nested_struct_type_info = field_info->type->struct_info;
            StructDecl& nested_struct_decl = nested_struct_type_info.decl->struct_decl;
            for (const auto& nested_field : nested_struct_decl.fields) {
                const FieldInfo* nested_field_info = nested_struct_type_info.find_field(nested_field.name);
                if (!nested_field_info) continue;
                ValueId nval;
                if (nested_field.default_value) {
                    nval = gen_expr(nested_field.default_value);
                } else {
                    nval = zero_init_field(*nested_field_info);
                }
                emit_set_field(nested_ptr, nested_field_info->name, nested_field_info->slot_offset, nested_field_info->slot_count, nval, nested_field_info->type);
            }
            // Copy nested struct to field
            ValueId field_addr = emit_get_field_addr(self_ptr, field_info->name, field_info->slot_offset, field_info->type);
            emit_struct_copy(field_addr, nested_ptr, nested_slots);
            continue;
        } else {
            value = zero_init_field(*field_info);
        }

        // For struct-typed fields with default values, use StructCopy
        if (field_info->type && field_info->type->is_struct() && field.default_value) {
            ValueId field_addr = emit_get_field_addr(self_ptr, field_info->name, field_info->slot_offset, field_info->type);
            emit_struct_copy(field_addr, value, field_info->slot_count);
        } else {
            emit_set_field(self_ptr, field_info->name, field_info->slot_offset, field_info->slot_count, value, field_info->type);
        }
    }

    // Initialize discriminant fields from when clauses (zero-init them)
    for (const auto& wfd : struct_decl.when_clauses) {
        const FieldInfo* field_info = struct_type_info.find_field(wfd.discriminant_name);
        if (!field_info) continue;

        // Discriminants are zero-initialized (first enum variant)
        ValueId value = zero_init_field(*field_info);
        emit_set_field(self_ptr, field_info->name, field_info->slot_offset, field_info->slot_count, value, field_info->type);
    }

    // Zero the whole union region of each when-clause. Stack allocation
    // doesn't clear memory, so variant fields in the union slot can carry
    // whatever bytes the previous owner of that local_stack region left
    // behind. A later `self.variant_field = …` (in caller code) emits
    // destroy-old on those stale bytes and crashes in
    // `slab_allocator::free_in_slab`. Zeroing the union once covers every
    // variant's fields — no per-variant iteration needed.
    for (const auto& clause : struct_type_info.when_clauses) {
        if (clause.union_slot_count == 0) continue;
        emit_zero_slots(self_ptr, clause.union_slot_offset, clause.union_slot_count);
    }

    // End function body (will add implicit return)
    end_function_body();

    IRFunction* result = m_current_func;
    m_current_func = nullptr;
    m_current_block = nullptr;
    return result;
}

IRFunction* IRBuilder::build_synthesized_default_destructor(Type* struct_type) {
    m_current_func = m_allocator.emplace<IRFunction>();

    StructTypeInfo& struct_type_info = struct_type->struct_info;
    m_current_func->name = mangle_destructor(struct_type_info.name);
    m_current_func->is_pub = struct_type_info.decl
        && struct_type_info.decl->struct_decl.is_pub;

    // Set up parameters - only 'self'
    setup_parameters({}, struct_type);

    // Destructor returns void
    m_current_func->return_type = m_types.void_type();

    // Begin function body
    begin_function_body(false);

    ValueId self_ptr = m_current_func->params[0].value;

    // Chain to parent's default destructor if present
    Type* parent_type = struct_type->struct_info.parent;
    if (parent_type) {
        for (const auto& dtor : parent_type->struct_info.destructors) {
            if (dtor.name.empty()) {
                StringView parent_dtor_name = mangle_destructor(parent_type->struct_info.name);
                Span<ValueId> dtor_args = alloc_span<ValueId>(1);
                dtor_args[0] = self_ptr;
                emit_call(parent_dtor_name, dtor_args, m_types.void_type());
                break;
            }
        }
    }

    // Clean up uniq fields
    emit_field_cleanup(self_ptr, struct_type);

    // End function body
    end_function_body();

    IRFunction* result = m_current_func;
    m_current_func = nullptr;
    m_current_block = nullptr;
    return result;
}

IRFunction* IRBuilder::build_cleanup_wrapper(Type* noncopyable_type, u32 wrapper_index) {
    m_current_func = m_allocator.emplace<IRFunction>();

    // Name: __cleanup_wrapper_<index>
    m_current_func->name = intern_format("__cleanup_wrapper_{}", wrapper_index);

    // Single parameter: the list/map pointer
    m_current_func->return_type = m_types.void_type();

    BlockParam param;
    param.value = m_current_func->new_value();
    param.type = noncopyable_type;
    param.name = StringView("ptr", 3);
    m_current_func->params.push_back(param);
    m_current_func->param_is_ptr.push_back(false);

    // Create entry block with the parameter as a block arg
    m_current_block = create_block(StringView("entry", 5));
    m_current_block->params.push_back(param);

    ValueId param_val = param.value;

    // Emit typed delete — runtime handles container iteration and element cleanup
    IRInst* del_inst = emit_inst(IROp::Delete, noncopyable_type);
    if (del_inst) {
        del_inst->unary = param_val;
    }

    // Set Return terminator directly (avoid finish_block_return which calls emit_ref_param_decrements).
    if (m_current_block && m_current_block->terminator.kind == TerminatorKind::None) {
        m_current_block->terminator.kind = TerminatorKind::Return;
        m_current_block->terminator.return_value = ValueId::invalid();
    }

    IRFunction* result = m_current_func;
    m_current_func = nullptr;
    m_current_block = nullptr;
    return result;
}

// Block management

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

IRInst* IRBuilder::emit_inst(IROp op, Type* result_type) {
    if (!m_current_block) return nullptr;

    IRInst* inst = m_allocator.emplace<IRInst>();
    inst->op = op;
    inst->result = m_current_func->new_value();
    inst->type = result_type;
    inst->source_line = m_current_source_line;
    m_current_func->values_by_id[inst->result.id] = inst;
    m_current_block->instructions.push_back(inst);
    return inst;
}

ValueId IRBuilder::emit_const_null() {
    IRInst* inst = emit_inst(IROp::ConstNull, m_types.nil_type());
    return inst ? inst->result : ValueId::invalid();
}

ValueId IRBuilder::emit_const_bool(bool value) {
    IRInst* inst = emit_inst(IROp::ConstBool, m_types.bool_type());
    if (inst) {
        inst->const_data.bool_val = value;
        return inst->result;
    }
    return ValueId::invalid();
}

ValueId IRBuilder::emit_const_int(i64 value, Type* type) {
    if (!type) type = m_types.i64_type();
    IRInst* inst = emit_inst(IROp::ConstInt, type);
    if (inst) {
        inst->const_data.int_val = value;
        return inst->result;
    }
    return ValueId::invalid();
}

ValueId IRBuilder::emit_const_float(f64 value, Type* type) {
    if (!type) type = m_types.f64_type();

    // Check if this is an f32 - if so, emit ConstF
    if (type->kind == TypeKind::F32) {
        IRInst* inst = emit_inst(IROp::ConstF, type);
        if (inst) {
            inst->const_data.f32_val = static_cast<f32>(value);
            return inst->result;
        }
        return ValueId::invalid();
    }

    // f64 case
    IRInst* inst = emit_inst(IROp::ConstD, type);
    if (inst) {
        inst->const_data.f64_val = value;
        return inst->result;
    }
    return ValueId::invalid();
}

ValueId IRBuilder::emit_const_string(StringView value) {
    IRInst* inst = emit_inst(IROp::ConstString, m_types.string_type());
    if (inst) {
        inst->const_data.string_val = value;
        return inst->result;
    }
    return ValueId::invalid();
}

ValueId IRBuilder::try_fold_binary(IROp op, ValueId left, ValueId right, Type* result_type) {
    if (!m_current_func) return ValueId::invalid();
    IRInst* l = m_current_func->inst_for(left);
    IRInst* r = m_current_func->inst_for(right);
    if (!l || !r) return ValueId::invalid();

    switch (op) {
    case IROp::AddI:
    case IROp::SubI:
    case IROp::MulI:
    case IROp::DivI:
    case IROp::ModI: {
        if (l->op != IROp::ConstInt || r->op != IROp::ConstInt) return ValueId::invalid();
        i64 a = l->const_data.int_val;
        i64 b = r->const_data.int_val;
        u64 ua = static_cast<u64>(a);
        u64 ub = static_cast<u64>(b);
        i64 result;
        switch (op) {
            case IROp::AddI: result = static_cast<i64>(ua + ub); break;
            case IROp::SubI: result = static_cast<i64>(ua - ub); break;
            case IROp::MulI: result = static_cast<i64>(ua * ub); break;
            case IROp::DivI:
                if (b == 0 || (a == INT64_MIN && b == -1)) return ValueId::invalid();
                result = a / b;
                break;
            case IROp::ModI:
                if (b == 0 || (a == INT64_MIN && b == -1)) return ValueId::invalid();
                result = a % b;
                break;
            default: return ValueId::invalid();
        }
        return emit_const_int(result, result_type);
    }

    case IROp::EqI:
    case IROp::NeI:
    case IROp::LtI:
    case IROp::LeI:
    case IROp::GtI:
    case IROp::GeI: {
        if (l->op != IROp::ConstInt || r->op != IROp::ConstInt) return ValueId::invalid();
        i64 a = l->const_data.int_val;
        i64 b = r->const_data.int_val;
        bool result;
        switch (op) {
            case IROp::EqI: result = a == b; break;
            case IROp::NeI: result = a != b; break;
            case IROp::LtI: result = a < b; break;
            case IROp::LeI: result = a <= b; break;
            case IROp::GtI: result = a > b; break;
            case IROp::GeI: result = a >= b; break;
            default: return ValueId::invalid();
        }
        return emit_const_bool(result);
    }

    case IROp::BitAnd:
    case IROp::BitOr:
    case IROp::BitXor: {
        if (l->op != IROp::ConstInt || r->op != IROp::ConstInt) return ValueId::invalid();
        u64 a = static_cast<u64>(l->const_data.int_val);
        u64 b = static_cast<u64>(r->const_data.int_val);
        u64 result = (op == IROp::BitAnd) ? (a & b)
                   : (op == IROp::BitOr)  ? (a | b)
                                          : (a ^ b);
        return emit_const_int(static_cast<i64>(result), result_type);
    }

    case IROp::Shl:
    case IROp::Shr: {
        if (l->op != IROp::ConstInt || r->op != IROp::ConstInt) return ValueId::invalid();
        u64 a = static_cast<u64>(l->const_data.int_val);
        u64 b = static_cast<u64>(r->const_data.int_val) & 63;
        i64 result = (op == IROp::Shl) ? static_cast<i64>(a << b)
                                       : (l->const_data.int_val >> b);  // arithmetic shift on signed
        return emit_const_int(result, result_type);
    }

    // f32 / f64 arithmetic — IEEE-754 host assumed.
    case IROp::AddF:
    case IROp::SubF:
    case IROp::MulF:
    case IROp::DivF: {
        if (l->op != IROp::ConstF || r->op != IROp::ConstF) return ValueId::invalid();
        f32 a = l->const_data.f32_val;
        f32 b = r->const_data.f32_val;
        f32 result = (op == IROp::AddF) ? (a + b)
                   : (op == IROp::SubF) ? (a - b)
                   : (op == IROp::MulF) ? (a * b)
                                        : (a / b);
        return emit_const_float(static_cast<f64>(result), result_type);
    }

    case IROp::AddD:
    case IROp::SubD:
    case IROp::MulD:
    case IROp::DivD: {
        if (l->op != IROp::ConstD || r->op != IROp::ConstD) return ValueId::invalid();
        f64 a = l->const_data.f64_val;
        f64 b = r->const_data.f64_val;
        f64 result = (op == IROp::AddD) ? (a + b)
                   : (op == IROp::SubD) ? (a - b)
                   : (op == IROp::MulD) ? (a * b)
                                        : (a / b);
        return emit_const_float(result, result_type);
    }

    case IROp::And:
    case IROp::Or: {
        if (l->op != IROp::ConstBool || r->op != IROp::ConstBool) return ValueId::invalid();
        bool a = l->const_data.bool_val;
        bool b = r->const_data.bool_val;
        return emit_const_bool(op == IROp::And ? (a && b) : (a || b));
    }

    default:
        return ValueId::invalid();
    }
}

ValueId IRBuilder::try_simplify_binary(IROp op, ValueId left, ValueId right, Type* result_type) {
    if (!m_current_func) return ValueId::invalid();
    IRInst* l = m_current_func->inst_for(left);
    IRInst* r = m_current_func->inst_for(right);

    auto is_ci = [](IRInst* i, i64 val) {
        return i && i->op == IROp::ConstInt && i->const_data.int_val == val;
    };

    switch (op) {
    case IROp::AddI:
        if (is_ci(r, 0)) return left;
        if (is_ci(l, 0)) return right;
        return ValueId::invalid();

    case IROp::SubI:
        if (is_ci(r, 0)) return left;
        if (left == right) return emit_const_int(0, result_type);
        return ValueId::invalid();

    case IROp::MulI:
        if (is_ci(r, 0)) return emit_const_int(0, result_type);
        if (is_ci(l, 0)) return emit_const_int(0, result_type);
        if (is_ci(r, 1)) return left;
        if (is_ci(l, 1)) return right;
        if (is_ci(r, 2)) return emit_binary(IROp::AddI, left, left, result_type);
        if (is_ci(l, 2)) return emit_binary(IROp::AddI, right, right, result_type);
        return ValueId::invalid();

    case IROp::DivI:
        if (is_ci(r, 1)) return left;
        return ValueId::invalid();

    case IROp::BitAnd:
        if (is_ci(r, 0)) return emit_const_int(0, result_type);
        if (is_ci(l, 0)) return emit_const_int(0, result_type);
        if (is_ci(r, -1)) return left;
        if (is_ci(l, -1)) return right;
        return ValueId::invalid();

    case IROp::BitOr:
        if (is_ci(r, 0)) return left;
        if (is_ci(l, 0)) return right;
        if (is_ci(r, -1)) return emit_const_int(-1, result_type);
        if (is_ci(l, -1)) return emit_const_int(-1, result_type);
        return ValueId::invalid();

    case IROp::BitXor:
        if (is_ci(r, 0)) return left;
        if (is_ci(l, 0)) return right;
        if (left == right) return emit_const_int(0, result_type);
        return ValueId::invalid();

    case IROp::Shl:
    case IROp::Shr:
        if (is_ci(r, 0)) return left;
        return ValueId::invalid();

    default:
        return ValueId::invalid();
    }
}

ValueId IRBuilder::emit_binary(IROp op, ValueId left, ValueId right, Type* result_type) {
    if (ValueId folded = try_fold_binary(op, left, right, result_type); folded.is_valid()) {
        return folded;
    }
    if (ValueId simplified = try_simplify_binary(op, left, right, result_type); simplified.is_valid()) {
        return simplified;
    }
    IRInst* inst = emit_inst(op, result_type);
    if (inst) {
        inst->binary.left = left;
        inst->binary.right = right;
        return inst->result;
    }
    return ValueId::invalid();
}

ValueId IRBuilder::try_fold_unary(IROp op, ValueId operand, Type* result_type) {
    if (!m_current_func) return ValueId::invalid();
    IRInst* o = m_current_func->inst_for(operand);
    if (!o) return ValueId::invalid();

    switch (op) {
    case IROp::NegI: {
        if (o->op != IROp::ConstInt) return ValueId::invalid();
        u64 v = static_cast<u64>(o->const_data.int_val);
        return emit_const_int(static_cast<i64>(0 - v), result_type);
    }
    case IROp::NegF: {
        if (o->op != IROp::ConstF) return ValueId::invalid();
        return emit_const_float(static_cast<f64>(-o->const_data.f32_val), result_type);
    }
    case IROp::NegD: {
        if (o->op != IROp::ConstD) return ValueId::invalid();
        return emit_const_float(-o->const_data.f64_val, result_type);
    }
    case IROp::Not: {
        if (o->op != IROp::ConstBool) return ValueId::invalid();
        return emit_const_bool(!o->const_data.bool_val);
    }
    case IROp::BitNot: {
        if (o->op != IROp::ConstInt) return ValueId::invalid();
        u64 v = static_cast<u64>(o->const_data.int_val);
        return emit_const_int(static_cast<i64>(~v), result_type);
    }
    default:
        return ValueId::invalid();
    }
}

ValueId IRBuilder::try_simplify_unary(IROp op, ValueId operand, Type* result_type) {
    (void)result_type;
    if (!m_current_func) return ValueId::invalid();
    IRInst* o = m_current_func->inst_for(operand);
    if (!o) return ValueId::invalid();

    // Double-negation: only safe for integer / bool / bitwise. Float Neg is
    // skipped because -(-0.0) = 0.0 distinguishes from -0.0 in IEEE-754.
    if ((op == IROp::NegI && o->op == IROp::NegI) ||
        (op == IROp::Not && o->op == IROp::Not) ||
        (op == IROp::BitNot && o->op == IROp::BitNot)) {
        return o->unary;
    }
    return ValueId::invalid();
}

ValueId IRBuilder::emit_unary(IROp op, ValueId operand, Type* result_type) {
    if (ValueId folded = try_fold_unary(op, operand, result_type); folded.is_valid()) {
        return folded;
    }
    if (ValueId simplified = try_simplify_unary(op, operand, result_type); simplified.is_valid()) {
        return simplified;
    }
    IRInst* inst = emit_inst(op, result_type);
    if (inst) {
        inst->unary = operand;
        return inst->result;
    }
    return ValueId::invalid();
}

ValueId IRBuilder::emit_copy(ValueId value, Type* type) {
    IRInst* inst = emit_inst(IROp::Copy, type);
    if (inst) {
        inst->unary = value;
        return inst->result;
    }
    return ValueId::invalid();
}

ValueId IRBuilder::emit_call(StringView func_name, Span<ValueId> args, Type* result_type) {
    IRInst* inst = emit_inst(IROp::Call, result_type);
    if (inst) {
        inst->call.func_name = func_name;
        inst->call.args = args;
        inst->call.native_index = 0;
        return inst->result;
    }
    return ValueId::invalid();
}

ValueId IRBuilder::emit_call_native(StringView func_name, Span<ValueId> args, Type* result_type, u32 native_index) {
    IRInst* inst = emit_inst(IROp::CallNative, result_type);
    if (inst) {
        inst->call.func_name = func_name;
        inst->call.args = args;
        inst->call.native_index = native_index;
        return inst->result;
    }
    return ValueId::invalid();
}

ValueId IRBuilder::emit_call_external(StringView module_name, StringView func_name, Span<ValueId> args, Type* result_type) {
    IRInst* inst = emit_inst(IROp::CallExternal, result_type);
    if (inst) {
        inst->call_external.module_name = module_name;
        inst->call_external.func_name = func_name;
        inst->call_external.args = args;
        return inst->result;
    }
    return ValueId::invalid();
}

ValueId IRBuilder::emit_index_get(ValueId container, ValueId index, ContainerKind kind, Type* result_type) {
    IRInst* inst = emit_inst(IROp::IndexGet, result_type);
    if (inst) {
        inst->index_data.container = container;
        inst->index_data.index = index;
        inst->index_data.value = ValueId::invalid();
        inst->index_data.kind = kind;
        return inst->result;
    }
    return ValueId::invalid();
}

void IRBuilder::emit_index_set(ValueId container, ValueId index, ValueId value, ContainerKind kind) {
    IRInst* inst = emit_inst(IROp::IndexSet, m_types.void_type());
    if (inst) {
        inst->index_data.container = container;
        inst->index_data.index = index;
        inst->index_data.value = value;
        inst->index_data.kind = kind;
    }
}

ValueId IRBuilder::emit_new(StringView type_name, Span<ValueId> args, Type* result_type) {
    IRInst* inst = emit_inst(IROp::New, result_type);
    if (inst) {
        inst->new_data.type_name = type_name;
        inst->new_data.args = args;
        return inst->result;
    }
    return ValueId::invalid();
}

ValueId IRBuilder::emit_stack_alloc(u32 slot_count, Type* result_type) {
    IRInst* inst = emit_inst(IROp::StackAlloc, result_type);
    if (inst) {
        inst->stack_alloc.slot_count = slot_count;
        return inst->result;
    }
    return ValueId::invalid();
}

ValueId IRBuilder::emit_get_field(ValueId object, StringView field_name, u32 slot_offset, u32 slot_count, Type* result_type) {
    IRInst* inst = emit_inst(IROp::GetField, result_type);
    if (inst) {
        inst->field.object = object;
        inst->field.field_name = field_name;
        inst->field.slot_offset = slot_offset;
        inst->field.slot_count = slot_count;
        return inst->result;
    }
    return ValueId::invalid();
}

ValueId IRBuilder::emit_get_field_addr(ValueId object, StringView field_name, u32 slot_offset, Type* result_type) {
    IRInst* inst = emit_inst(IROp::GetFieldAddr, result_type);
    if (inst) {
        inst->field.object = object;
        inst->field.field_name = field_name;
        inst->field.slot_offset = slot_offset;
        inst->field.slot_count = 0;  // Not used for address computation
        return inst->result;
    }
    return ValueId::invalid();
}

ValueId IRBuilder::emit_set_field(ValueId object, StringView field_name, u32 slot_offset, u32 slot_count, ValueId value, Type* result_type) {
    IRInst* inst = emit_inst(IROp::SetField, result_type);
    if (inst) {
        inst->field.object = object;
        inst->field.field_name = field_name;
        inst->field.slot_offset = slot_offset;
        inst->field.slot_count = slot_count;
        inst->store_value = value;
        return inst->result;
    }
    return ValueId::invalid();
}


ValueId IRBuilder::emit_load_ptr(ValueId ptr, u32 slot_count, Type* result_type) {
    IRInst* inst = emit_inst(IROp::LoadPtr, result_type);
    if (inst) {
        inst->load_ptr.ptr = ptr;
        inst->load_ptr.slot_count = slot_count;
        return inst->result;
    }
    return ValueId::invalid();
}

ValueId IRBuilder::emit_store_ptr(ValueId ptr, ValueId value, u32 slot_count, Type* result_type) {
    IRInst* inst = emit_inst(IROp::StorePtr, result_type);
    if (inst) {
        inst->store_ptr.ptr = ptr;
        inst->store_ptr.value = value;
        inst->store_ptr.slot_count = slot_count;
        return inst->result;
    }
    return ValueId::invalid();
}

void IRBuilder::emit_struct_copy(ValueId dest_ptr, ValueId source_ptr, u32 slot_count) {
    IRInst* inst = emit_inst(IROp::StructCopy, nullptr);
    if (inst) {
        inst->struct_copy.dest_ptr = dest_ptr;
        inst->struct_copy.source_ptr = source_ptr;
        inst->struct_copy.slot_count = slot_count;
    }
}

void IRBuilder::emit_ref_inc(ValueId ptr) {
    IRInst* inst = emit_inst(IROp::RefInc, m_types.void_type());
    if (inst) inst->unary = ptr;
}

void IRBuilder::emit_ref_dec(ValueId ptr) {
    IRInst* inst = emit_inst(IROp::RefDec, m_types.void_type());
    if (inst) inst->unary = ptr;
}

void IRBuilder::emit_ref_param_decrements() {
    // Emit RefDec for all ref-typed parameters before function exit (normal
    // path). The exception-unwind path is covered separately by RefDec cleanup
    // records (end_function_body) — the two are mutually exclusive per control
    // path, so a param is decremented exactly once.
    for (const RefParamInfo& param : m_ref_params) {
        emit_ref_dec(param.value);
    }
}

ValueId IRBuilder::emit_weak_create(ValueId ptr, Type* weak_type) {
    return emit_unary(IROp::WeakCreate, ptr, weak_type);
}

ValueId IRBuilder::maybe_wrap_weak(ValueId value, Type* source_type, Type* target_type) {
    if (!source_type || !target_type) return value;
    // A function value is a heap env pointer with a header, so `fun -> weak fun`
    // is created the same way as uniq/ref -> weak.
    if (target_type->kind == TypeKind::Weak &&
        (source_type->kind == TypeKind::Uniq || source_type->kind == TypeKind::Ref ||
         source_type->kind == TypeKind::Function)) {
        return emit_weak_create(value, target_type);
    }
    return value;
}

// Statement generation

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

void IRBuilder::gen_if_stmt(Stmt* stmt) {
    IfStmt& is = stmt->if_stmt;

    // Detect else-if chains and use flat codegen to avoid quadratic compilation
    if (is.else_branch && is.else_branch->kind == AstKind::StmtIf) {
        gen_if_else_chain(stmt);
        return;
    }

    // Evaluate condition
    ValueId cond = gen_expr(is.condition);

    // Collect variables assigned in both branches
    Vector<StringView> then_modified, else_modified;
    collect_assigned_vars(is.then_branch, then_modified);
    if (is.else_branch) {
        collect_assigned_vars(is.else_branch, else_modified);
    }

    // Find variables that are assigned in either branch and exist before the if
    // These need phi nodes (block params) at the merge point
    Vector<StringView> phi_vars;
    for (const auto& name : then_modified) {
        LocalVar* lv = find_local(name);
        if (lv && lv->value.is_valid()) {
            bool found = false;
            for (const auto& pv : phi_vars) {
                if (pv == name) { found = true; break; }
            }
            if (!found) phi_vars.push_back(name);
        }
    }
    for (const auto& name : else_modified) {
        LocalVar* lv = find_local(name);
        if (lv && lv->value.is_valid()) {
            bool found = false;
            for (const auto& pv : phi_vars) {
                if (pv == name) { found = true; break; }
            }
            if (!found) phi_vars.push_back(name);
        }
    }

    // Create blocks
    IRBlock* then_block = create_block("then");
    IRBlock* else_block = is.else_branch ? create_block("else") : nullptr;
    IRBlock* merge_block = create_block("endif");

    // Create block params on merge block for phi variables
    struct PhiInfo {
        StringView name;
        Type* type;
        ValueId merge_param;
        ValueId original_value;
    };
    Vector<PhiInfo> phi_info;
    for (const auto& pv : phi_vars) {
        LocalVar* lv = find_local(pv);
        if (lv) {
            ValueId param = m_current_func->new_value();
            merge_block->params.push_back({param, lv->type, pv});
            phi_info.push_back({pv, lv->type, param, lv->value});
        }
    }

    // Branch based on condition
    if (else_block) {
        finish_block_branch(cond, then_block->id, else_block->id);
    } else {
        // No else branch - pass original values as args to merge_block
        Vector<BlockArgPair> fallthrough_args;
        for (const auto& pi : phi_info) {
            fallthrough_args.push_back({pi.original_value});
        }
        finish_block_branch(cond, then_block->id, merge_block->id, {}, alloc_span(fallthrough_args));
    }

    // Save pre-if local-scope and is_moved state. We must roll the IR builder's
    // bookkeeping (nullify-replace, owned_local.is_moved) back across terminating
    // branches so the merge block — reachable only via the surviving paths —
    // doesn't see consumed/nil-replaced locals from a branch that returned. This
    // mirrors the semantic-side definite-termination move-state merging.
    Vector<tsl::robin_map<StringView, LocalVar>> saved_scopes_pre;
    saved_scopes_pre.reserve(m_local_scopes.size());
    for (auto& scope : m_local_scopes) {
        saved_scopes_pre.push_back(scope);
    }
    Vector<bool> saved_is_moved_pre;
    saved_is_moved_pre.reserve(m_owned_locals.size());
    for (auto& info : m_owned_locals) {
        saved_is_moved_pre.push_back(info.is_moved);
    }

    auto restore_pre_if_state = [&]() {
        // Runs at most once per if (the else path and the no-else path are
        // mutually exclusive), so move the snapshot back instead of deep-copying
        // every scope map. Do not call this more than once.
        m_local_scopes = std::move(saved_scopes_pre);
        for (u32 i = 0; i < saved_is_moved_pre.size() && i < m_owned_locals.size(); i++) {
            m_owned_locals[i].is_moved = saved_is_moved_pre[i];
        }
    };

    // Generate then branch
    set_current_block(then_block);
    gen_stmt(is.then_branch);
    bool then_terminated = !m_current_block || m_current_block->terminator.kind != TerminatorKind::None;
    if (!then_terminated) {
        // Build args for merge block
        Vector<BlockArgPair> then_args;
        for (const auto& pi : phi_info) {
            ValueId val = lookup_local(pi.name);
            then_args.push_back({val});
        }
        finish_block_goto(merge_block->id, alloc_span(then_args));
    }

    // Snapshot post-then state in case the else branch terminates and we need to
    // restore the then-branch's mutations for code after the merge.
    Vector<tsl::robin_map<StringView, LocalVar>> saved_scopes_post_then;
    Vector<bool> saved_is_moved_post_then;
    if (else_block && !then_terminated) {
        saved_scopes_post_then.reserve(m_local_scopes.size());
        for (auto& scope : m_local_scopes) {
            saved_scopes_post_then.push_back(scope);
        }
        saved_is_moved_post_then.reserve(m_owned_locals.size());
        for (auto& info : m_owned_locals) {
            saved_is_moved_post_then.push_back(info.is_moved);
        }
    }

    // Generate else branch
    bool else_terminated = false;
    if (else_block) {
        // Restore pre-if state so the else branch sees original values
        restore_pre_if_state();

        set_current_block(else_block);
        gen_stmt(is.else_branch);
        else_terminated = !m_current_block || m_current_block->terminator.kind != TerminatorKind::None;
        if (!else_terminated) {
            // Build args for merge block
            Vector<BlockArgPair> else_args;
            for (const auto& pi : phi_info) {
                ValueId val = lookup_local(pi.name);
                else_args.push_back({val});
            }
            finish_block_goto(merge_block->id, alloc_span(else_args));
        }
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
            restore_pre_if_state();
        }
    } else if (then_terminated && !else_terminated) {
        // Current state is post-else, which is correct.
    } else if (else_terminated && !then_terminated) {
        // Restore post-then snapshot (used only here — move it back).
        m_local_scopes = std::move(saved_scopes_post_then);
        for (u32 i = 0; i < saved_is_moved_post_then.size() && i < m_owned_locals.size(); i++) {
            m_owned_locals[i].is_moved = saved_is_moved_post_then[i];
        }
    }

    // Continue with merge block
    set_current_block(merge_block);

    // Bind variables to merge block params (phi results)
    for (const auto& pi : phi_info) {
        define_local(pi.name, pi.merge_param, pi.type);
    }
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

    // 2. Find phi vars: modified vars that exist before the if chain
    Vector<StringView> phi_vars;
    for (const auto& name : all_modified) {
        LocalVar* local_var = find_local(name);
        if (local_var && local_var->value.is_valid()) {
            bool found = false;
            for (const auto& existing : phi_vars) {
                if (existing == name) { found = true; break; }
            }
            if (!found) phi_vars.push_back(name);
        }
    }

    // 3. Create merge block with parameters for phi vars
    IRBlock* merge_block = create_block("endif");

    struct PhiInfo {
        StringView name;
        Type* type;
        ValueId merge_param;
        ValueId original_value;
    };
    Vector<PhiInfo> phi_info;
    for (const auto& phi_var : phi_vars) {
        LocalVar* local_var = find_local(phi_var);
        if (local_var) {
            ValueId param = m_current_func->new_value();
            merge_block->params.push_back({param, local_var->type, phi_var});
            phi_info.push_back({phi_var, local_var->type, param, local_var->value});
        }
    }

    // 4. Save variable state ONCE before any branch
    Vector<tsl::robin_map<StringView, LocalVar>> saved_scopes;
    saved_scopes.reserve(m_local_scopes.size());
    for (auto& scope : m_local_scopes) {
        saved_scopes.push_back(scope);
    }

    // Save is_moved state for owned locals
    Vector<bool> saved_is_moved;
    for (auto& info : m_owned_locals) {
        saved_is_moved.push_back(info.is_moved);
    }

    // 5. Create body blocks for each branch + optional default
    Vector<IRBlock*> body_blocks;
    for (u32 i = 0; i < branches.size(); i++) {
        body_blocks.push_back(create_block("then"));
    }
    IRBlock* default_block = nullptr;
    if (default_body) {
        default_block = create_block("else");
    }

    // 6. Generate comparison chain: evaluate each condition, branch to body or next check
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
            Vector<BlockArgPair> fallthrough_args;
            for (const auto& pi : phi_info) {
                fallthrough_args.push_back({pi.original_value});
            }
            finish_block_branch(cond, body_blocks[i]->id, fallthrough_block->id,
                                {}, alloc_span(fallthrough_args));
        } else {
            finish_block_branch(cond, body_blocks[i]->id, fallthrough_block->id);
        }

        // Set next check block as current if there are more branches
        if (i + 1 < branches.size()) {
            set_current_block(fallthrough_block);
        }
    }

    // 7. Generate branch bodies
    for (u32 i = 0; i < branches.size(); i++) {
        // Restore scopes so this branch sees original values
        m_local_scopes.clear_keep_capacity();
        for (auto& scope : saved_scopes) {
            m_local_scopes.push_back(scope);
        }

        // Restore is_moved state
        for (u32 j = 0; j < saved_is_moved.size() && j < m_owned_locals.size(); j++) {
            m_owned_locals[j].is_moved = saved_is_moved[j];
        }

        set_current_block(body_blocks[i]);
        gen_stmt(branches[i].body);

        // Jump to merge block with phi args if not terminated
        if (m_current_block && m_current_block->terminator.kind == TerminatorKind::None) {
            Vector<BlockArgPair> args;
            for (const auto& pi : phi_info) {
                ValueId val = lookup_local(pi.name);
                args.push_back({val});
            }
            finish_block_goto(merge_block->id, alloc_span(args));
        }
    }

    // 8. Generate default body if present
    if (default_block) {
        // Restore scopes
        m_local_scopes.clear_keep_capacity();
        for (auto& scope : saved_scopes) {
            m_local_scopes.push_back(scope);
        }

        // Restore is_moved state
        for (u32 j = 0; j < saved_is_moved.size() && j < m_owned_locals.size(); j++) {
            m_owned_locals[j].is_moved = saved_is_moved[j];
        }

        set_current_block(default_block);
        gen_stmt(default_body);

        // Jump to merge block with phi args if not terminated
        if (m_current_block && m_current_block->terminator.kind == TerminatorKind::None) {
            Vector<BlockArgPair> args;
            for (const auto& pi : phi_info) {
                ValueId val = lookup_local(pi.name);
                args.push_back({val});
            }
            finish_block_goto(merge_block->id, alloc_span(args));
        }
    }

    // 9. Continue from merge block, bind phi results.
    // Restore pre-chain local-scope SSA bindings so the last branch's
    // mutations (e.g. struct-literal nullify-replacing a moved local to nil)
    // don't leak into post-chain code. Phi vars are immediately rebound from
    // merge_param below; non-phi vars must agree across surviving paths by
    // construction (semantic forbids divergent moves on them). is_moved is
    // intentionally left alone — see the rationale in gen_try_stmt and
    // gen_when_stmt.
    m_local_scopes.clear_keep_capacity();
    for (auto& scope : saved_scopes) {
        m_local_scopes.push_back(scope);
    }

    set_current_block(merge_block);
    for (const auto& pi : phi_info) {
        define_local(pi.name, pi.merge_param, pi.type);
    }
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

// A `ref`-typed expression "hands off" a borrow count when it is the result of
// a call: by the counting convention (gen_return_stmt) every ref-returning
// function returns with exactly one count handed to the caller. All other ref
// sources (identifiers, borrowed subscripts, `ref x`, field reads) carry no
// count of their own, so binding from them is a fresh borrow that increments.
static bool is_ref_handoff_source(Expr* init) {
    return init && init->kind == AstKind::ExprCall;
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
                    ? find_owned_local(rs.value->identifier.name) : nullptr;
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
                emit_ref_inc(val);
            }
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
            // Call the destructor
            StringView dtor_name = mangle_destructor(struct_type_info.name, ds.destructor_name);

            // Build arguments: 'self' pointer + destructor arguments
            Vector<ValueId> call_args;
            call_args.push_back(val);  // 'self' pointer

            for (auto& arg : ds.arguments) {
                if (arg.modifier != ParamModifier::None) {
                    // Pass address of lvalue for out/inout args
                    call_args.push_back(gen_lvalue_addr(arg.expr));
                } else {
                    call_args.push_back(gen_expr(arg.expr));
                }
            }

            emit_call(dtor_name, alloc_span(call_args), m_types.void_type());
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
    IRInst* inst = emit_inst(IROp::Delete, m_types.void_type());
    if (inst) {
        inst->unary = val;
    }

    // Mark the variable as moved so scope cleanup doesn't double-destroy. The memory
    // was just explicitly Delete'd, so leave the SSA register alone (null_ssa=false);
    // the Nullify annotation still ends the cleanup record scope.
    if (ds.expr->kind == AstKind::ExprIdentifier) {
        mark_moved_from(ds.expr->identifier.name, /*null_ssa=*/false);
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

    // 2. Find phi vars: modified vars that exist before the when
    Vector<StringView> phi_vars;
    for (const auto& name : modified_in_cases) {
        LocalVar* lv = find_local(name);
        if (lv && lv->value.is_valid()) {
            bool found = false;
            for (const auto& pv : phi_vars) {
                if (pv == name) { found = true; break; }
            }
            if (!found) phi_vars.push_back(name);
        }
    }

    // 3. Evaluate discriminant
    ValueId discrim = gen_expr(ws.discriminant);
    Type* discrim_type = ws.discriminant->resolved_type;

    // 4. Create merge block with parameters for phi vars
    IRBlock* merge_block = create_block("endwhen");

    struct PhiInfo {
        StringView name;
        Type* type;
        ValueId merge_param;
        ValueId original_value;
    };
    Vector<PhiInfo> phi_info;
    for (const auto& pv : phi_vars) {
        LocalVar* lv = find_local(pv);
        if (lv) {
            ValueId param = m_current_func->new_value();
            merge_block->params.push_back({param, lv->type, pv});
            phi_info.push_back({pv, lv->type, param, lv->value});
        }
    }

    // 5. Save variable state before any case (so all cases see original values)
    Vector<tsl::robin_map<StringView, LocalVar>> saved_scopes;
    saved_scopes.reserve(m_local_scopes.size());
    for (auto& scope : m_local_scopes) {
        saved_scopes.push_back(scope);
    }

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

    // Create else block if there's an else clause
    IRBlock* else_block = nullptr;
    if (ws.else_body.size() > 0) {
        else_block = create_block("when_else");
    }

    // 6. Generate the comparison chain
    for (u32 i = 0; i < ws.cases.size(); i++) {
        WhenCase& wc = ws.cases[i];
        CaseInfo& ci = case_infos[i];

        // Build condition: discriminant == case_name1 || discriminant == case_name2 || ...
        ValueId case_cond = ValueId::invalid();
        for (const auto& case_name : wc.case_names) {

            // Look up the enum variant value from symbol table
            i64 variant_value = 0;
            Symbol* sym = m_symbols.lookup(case_name);
            if (sym && sym->kind == SymbolKind::EnumVariant) {
                variant_value = sym->enum_variant.value;
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
            Vector<BlockArgPair> fallthrough_args;
            for (const auto& pi : phi_info) {
                fallthrough_args.push_back({pi.original_value});
            }
            finish_block_branch(case_cond, ci.body_block->id, fallthrough_block->id,
                                {}, alloc_span(fallthrough_args));
        } else {
            finish_block_branch(case_cond, ci.body_block->id, fallthrough_block->id);
        }

        // Set next block as current if there are more cases
        if (i + 1 < ws.cases.size()) {
            set_current_block(fallthrough_block);
        }
    }

    // 7. Generate case bodies
    for (u32 i = 0; i < ws.cases.size(); i++) {
        WhenCase& wc = ws.cases[i];
        CaseInfo& ci = case_infos[i];

        // Restore scope so this case sees original values
        m_local_scopes.clear_keep_capacity();
        for (auto& scope : saved_scopes) {
            m_local_scopes.push_back(scope);
        }

        set_current_block(ci.body_block);
        push_scope();

        // Generate body statements
        for (auto* d : wc.body) {
            gen_decl(d);
        }

        pop_scope();

        // Jump to merge block with phi args if not terminated
        if (m_current_block && m_current_block->terminator.kind == TerminatorKind::None) {
            Vector<BlockArgPair> args;
            for (const auto& pi : phi_info) {
                ValueId val = lookup_local(pi.name);
                args.push_back({val});
            }
            finish_block_goto(merge_block->id, alloc_span(args));
        }
    }

    // 8. Generate else body if present
    if (else_block) {
        // Restore scope
        m_local_scopes.clear_keep_capacity();
        for (auto& scope : saved_scopes) {
            m_local_scopes.push_back(scope);
        }

        set_current_block(else_block);
        push_scope();

        for (auto* d : ws.else_body) {
            gen_decl(d);
        }

        pop_scope();

        // Jump to merge block with phi args if not terminated
        if (m_current_block && m_current_block->terminator.kind == TerminatorKind::None) {
            Vector<BlockArgPair> args;
            for (const auto& pi : phi_info) {
                ValueId val = lookup_local(pi.name);
                args.push_back({val});
            }
            finish_block_goto(merge_block->id, alloc_span(args));
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
    m_local_scopes.clear_keep_capacity();
    for (auto& scope : saved_scopes) {
        m_local_scopes.push_back(scope);
    }

    set_current_block(merge_block);
    for (const auto& pi : phi_info) {
        define_local(pi.name, pi.merge_param, pi.type);
    }
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

    // Deduplicate and filter to existing variables
    struct PhiInfo {
        StringView name;
        Type* type;
        ValueId merge_param;
    };
    Vector<PhiInfo> phi_info;
    for (const auto& name : modified_vars) {
        LocalVar* local_var = find_local(name);
        if (!local_var || !local_var->value.is_valid()) continue;
        bool found = false;
        for (const auto& pi : phi_info) {
            if (pi.name == name) { found = true; break; }
        }
        if (!found) {
            phi_info.push_back({name, local_var->type, ValueId::invalid()});
        }
    }

    // Create after block with phi params
    IRBlock* after_block = create_block("try.after");
    for (auto& pi : phi_info) {
        pi.merge_param = m_current_func->new_value();
        after_block->params.push_back({pi.merge_param, pi.type, pi.name});
    }

    // Helper to build block args for jumping to after_block
    auto build_after_args = [&]() -> Span<BlockArgPair> {
        Vector<BlockArgPair> args;
        for (const auto& pi : phi_info) {
            ValueId val = lookup_local(pi.name);
            args.push_back({val});
        }
        return alloc_span(args);
    };

    // Snapshot pre-try local-scope SSA bindings. Each catch block must see the
    // locals as they were *before* the try body ran — otherwise the catch sees
    // rebindings (e.g. `r = throwing_call()` rebinds r to the call result's
    // SSA value, even when the call threw and never produced one), and the
    // after-block phi feeds those undefined values out.
    //
    // We deliberately do NOT snapshot m_owned_locals.is_moved here. That flag
    // governs IR-builder bookkeeping for runtime cleanup (whether to emit a
    // destroy at scope exit / before reassignment). Rolling it back would
    // re-enable the implicit-destroy preamble of `r = uniq T()` inside catch
    // for a uniq local already consumed in the try body, double-freeing the
    // already-dead slab slot. Use-after-move in catch is the semantic
    // analyzer's job to reject, not the IR builder's to repair.
    Vector<tsl::robin_map<StringView, LocalVar>> saved_scopes_pre;
    saved_scopes_pre.reserve(m_local_scopes.size());
    for (auto& scope : m_local_scopes) {
        saved_scopes_pre.push_back(scope);
    }
    auto restore_pre_try_state = [&]() {
        m_local_scopes.clear_keep_capacity();
        for (auto& scope : saved_scopes_pre) {
            m_local_scopes.push_back(scope);
        }
    };

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
            finish_block_goto(after_block->id, build_after_args());
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
        restore_pre_try_state();

        // Define catch variable in scope
        push_scope();
        define_local(clause.var_name, exc_param.value, exc_param.type);

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
                finish_block_goto(after_block->id, build_after_args());
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
        exc_param.name = StringView("__exc", 5);
        finally_catch_block->params.push_back(exc_param);

        set_current_block(finally_catch_block);

        // Restore pre-try state for the same reason as the typed catches.
        restore_pre_try_state();

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
    for (const auto& pi : phi_info) {
        define_local(pi.name, pi.merge_param, pi.type);
    }
}

// Expression generation

ValueId IRBuilder::gen_expr(Expr* expr) {
    if (!expr) return ValueId::invalid();

    switch (expr->kind) {
        case AstKind::ExprLiteral:
            return gen_literal_expr(expr);
        case AstKind::ExprIdentifier:
            return gen_identifier_expr(expr);
        case AstKind::ExprUnary:
            return gen_unary_expr(expr);
        case AstKind::ExprBinary:
            return gen_binary_expr(expr);
        case AstKind::ExprTernary:
            return gen_ternary_expr(expr);
        case AstKind::ExprCall:
            return gen_call_expr(expr);
        case AstKind::ExprIndex:
            return gen_index_expr(expr);
        case AstKind::ExprGet:
            return gen_get_expr(expr);
        case AstKind::ExprAssign:
            return gen_assign_expr(expr);
        case AstKind::ExprGrouping:
            return gen_grouping_expr(expr);
        case AstKind::ExprThis:
            return gen_this_expr(expr);
        case AstKind::ExprStructLiteral:
            return gen_struct_literal_expr(expr);
        case AstKind::ExprStaticGet:
            return gen_static_get_expr(expr);
        case AstKind::ExprStringInterp:
            return gen_string_interp_expr(expr);
        case AstKind::ExprLambda:
            return gen_lambda_expr(expr);
        default:
            report_error("Internal error: unknown expression kind in IR generation");
            return ValueId::invalid();
    }
}

ValueId IRBuilder::gen_lambda_expr(Expr* expr) {
    LambdaExpr& le = expr->lambda;

    // Record the env struct so a synthesized destructor is built for it (after
    // all bodies). A closure value is a uniq env pointer; on delete we dispatch
    // this destructor by the env's runtime type_id to clean its captures.
    if (le.env_struct_type && le.env_struct_type->is_struct()) {
        bool seen = false;
        for (Type* t : m_env_struct_types) {
            if (t == le.env_struct_type) { seen = true; break; }
        }
        if (!seen) m_env_struct_types.push_back(le.env_struct_type);
    }

    // The lambda expression's resolved type is `Function<sig>`. Lowering treats it
    // as a `uniq` pointer to the synthesized env struct.
    //
    // The synthesized call function is non-pub, so build_function applies
    // mangle_module_local to its IRFunction name. We must reference it by the
    // mangled name to find it in the bytecode lowering's m_func_indices map.
    //
    // Captures: emit gen_expr on a synthetic IdentifierExpr per capture (resolved
    // via the IR builder's local-scope map — captures live in the OUTER function
    // we're currently emitting IR for). For Move-mode captures, null-ify the
    // local + Nullify the cleanup-record register (mirrors arg-passing pattern in
    // gen_call_expr around line 3475-3494).
    Vector<ValueId> capture_values;
    capture_values.reserve(le.resolved_captures.size());
    for (const CaptureInfo& cap : le.resolved_captures) {
        // The capture's source expression was built at semantic time. For
        // top-level captures it's a direct IdentifierExpr; for nested closures
        // it's an ExprGet on the enclosing lambda's __env. For self captures
        // it's an ExprThis (or a synthesized struct literal for [copy self]).
        // gen_expr produces a value in the enclosing function's IR scope.
        ValueId v = gen_expr(cap.source_expr);

        // Runtime heap check for self captures whose receiver may be stack-
        // allocated (copyable struct + ref/weak self capture). The check fires
        // on the source pointer before we store it into the env field.
        if (cap.needs_heap_check) {
            IRInst* assert_inst = emit_inst(IROp::AssertHeap, m_types.void_type());
            if (assert_inst) assert_inst->unary = v;
        }

        capture_values.push_back(v);

        if (cap.mode == CaptureMode::Move) {
            // The capture transfers ownership of the outer value into the env.
            // Suppress scope-exit cleanup of the source so it isn't freed twice.
            // A direct capture sources a local (mark it moved); a *transitive*
            // move sources an enclosing env field (__env.c) — null that field so
            // the enclosing env's destructor doesn't re-delete the moved-out
            // value. (Closure-env cleanup now runs, so this double-free, formerly
            // masked by the inert closure delete, must be prevented.)
            mark_moved_from(cap.name);
            nullify_moved_field_source(cap.source_expr);
        }
    }

    IRInst* inst = emit_inst(IROp::Closure, expr->resolved_type);
    if (!inst) return ValueId::invalid();
    inst->closure.env_struct_name = le.env_struct_name;
    inst->closure.call_function_name = mangle_module_local(le.call_function_name);
    inst->closure.captures = m_allocator.alloc_span(capture_values);
    return inst->result;
}

ValueId IRBuilder::gen_function_ref(Expr* expr, const FunctionRefTarget& target) {
    if (!target.function_type || !target.function_type->is_function()) {
        report_error("Internal error: gen_function_ref target has non-function type");
        return ValueId::invalid();
    }

    // Cache key: target.name is unique per IR-level function (mangled scripts,
    // monomorphized generics) or per native registry entry, so it dedupes
    // across alias paths. Imported script trampolines key on the unique
    // module::name form composed below.
    StringView cache_key = target.name;
    StringView ext_module_name;
    if (target.kind == FunctionRefTarget::Kind::ImportedScript) {
        u32 mod_len = target.module_name.size();
        u32 nm_len = target.name.size();
        u32 total = mod_len + 2 + nm_len;
        char* buf = reinterpret_cast<char*>(m_allocator.alloc_bytes(total, 1));
        memcpy(buf, target.module_name.data(), mod_len);
        buf[mod_len] = ':'; buf[mod_len + 1] = ':';
        memcpy(buf + mod_len + 2, target.name.data(), nm_len);
        cache_key = StringView(buf, total);
        ext_module_name = target.module_name;
    }

    auto cache_it = m_function_refs.find(cache_key);
    StringView env_struct_name;
    StringView trampoline_name;
    if (cache_it != m_function_refs.end()) {
        env_struct_name = cache_it->second.env_struct_name;
        trampoline_name = cache_it->second.trampoline_name;
    } else {
        u32 ref_id = m_funref_id_counter++;

        // Synthesize an empty env struct (just `__call_idx: u32`). Same shape
        // as a zero-capture lambda's env, just a different name so each
        // function-ref maps to its own type_id.
        env_struct_name = intern_format("__funref_{}_env", ref_id);

        Type* env_type = m_types.struct_type(env_struct_name, nullptr);
        FieldInfo* fields = reinterpret_cast<FieldInfo*>(
            m_allocator.alloc_bytes(sizeof(FieldInfo), alignof(FieldInfo)));
        fields[0].name = StringView("__call_idx", 10);
        fields[0].type = m_types.u32_type();
        fields[0].is_pub = false;
        fields[0].index = 0;
        fields[0].slot_offset = 0;
        fields[0].slot_count = 1;
        env_type->struct_info.fields = Span<FieldInfo>(fields, 1);
        env_type->struct_info.slot_count = 1;
        env_type->struct_info.constructors = Span<ConstructorInfo>();
        env_type->struct_info.destructors = Span<DestructorInfo>();
        env_type->struct_info.methods = Span<MethodInfo>();
        env_type->struct_info.when_clauses = Span<WhenClauseInfo>();
        env_type->struct_info.implemented_traits = Span<TraitImplRecord>();
        env_type->struct_info.parent = nullptr;
        env_type->struct_info.module_name = StringView(nullptr, 0);
        m_type_env.register_named_type(env_struct_name, env_type);

        // Synthesize the trampoline IRFunction. Following the convention in
        // build_function (line 365-374), non-pub functions get
        // mangle_module_local applied; "main" stays unmangled. The trampoline
        // is non-pub by construction, so always mangle.
        StringView raw_tramp_name = intern_format("__funref_{}_call", ref_id);
        trampoline_name = mangle_module_local(raw_tramp_name);

        FunctionTypeInfo& fti = target.function_type->func_info;
        Type* ref_env_type = m_types.ref_type(env_type);

        // Build the IRFunction in a hand-rolled fashion (mirrors the pattern
        // in coroutine_lowering.cpp lines 996-1021). We must NOT use IRBuilder's
        // emit_* helpers because those mutate m_current_func / m_current_block,
        // which are owned by the caller's enclosing function-IR generation.
        IRFunction* tramp = m_allocator.emplace<IRFunction>();
        tramp->name = trampoline_name;
        tramp->return_type = fti.return_type;

        // Param 0: __env: ref EnvType (unused inside the body).
        BlockParam env_param;
        env_param.value = tramp->new_value();
        env_param.type = ref_env_type;
        env_param.name = StringView("__env", 5);
        tramp->params.push_back(env_param);
        tramp->param_is_ptr.push_back(false);

        // Params 1..N: forwarded arguments named arg0, arg1, ...
        Vector<ValueId> forwarded_arg_values;
        for (u32 i = 0; i < fti.param_types.size(); i++) {
            StringView arg_name = intern_format("arg{}", i);

            BlockParam p;
            p.value = tramp->new_value();
            p.type = fti.param_types[i];
            p.name = arg_name;
            tramp->params.push_back(p);
            tramp->param_is_ptr.push_back(false);
            forwarded_arg_values.push_back(p.value);
        }

        // Single entry block: <body call>; Return result. The body op is
        // chosen by target kind so the same trampoline shell forwards to
        // script / native / external functions uniformly.
        IRBlock* entry = m_allocator.emplace<IRBlock>();
        entry->id = BlockId{0};
        entry->name = StringView("entry", 5);
        tramp->blocks.push_back(entry);

        IRInst* call_inst = m_allocator.emplace<IRInst>();
        call_inst->type = fti.return_type;
        call_inst->result = tramp->new_value();
        tramp->values_by_id[call_inst->result.id] = call_inst;
        switch (target.kind) {
            case FunctionRefTarget::Kind::Script:
                call_inst->op = IROp::Call;
                call_inst->call.func_name = target.name;
                call_inst->call.args = m_allocator.alloc_span(forwarded_arg_values);
                call_inst->call.native_index = 0;
                break;
            case FunctionRefTarget::Kind::Native:
            case FunctionRefTarget::Kind::ImportedNative:
                call_inst->op = IROp::CallNative;
                call_inst->call.func_name = target.name;
                call_inst->call.args = m_allocator.alloc_span(forwarded_arg_values);
                call_inst->call.native_index = target.native_index;
                break;
            case FunctionRefTarget::Kind::ImportedScript:
                call_inst->op = IROp::CallExternal;
                call_inst->call_external.module_name = ext_module_name;
                call_inst->call_external.func_name = target.name;
                call_inst->call_external.args = m_allocator.alloc_span(forwarded_arg_values);
                break;
        }
        entry->instructions.push_back(call_inst);

        // Return the call's result (or void).
        entry->terminator.kind = TerminatorKind::Return;
        if (fti.return_type && !fti.return_type->is_void()) {
            entry->terminator.return_value = call_inst->result;
        } else {
            entry->terminator.return_value = ValueId::invalid();
        }

        m_module->functions.push_back(tramp);

        FunctionRefInfo info{env_struct_name, trampoline_name};
        m_function_refs[cache_key] = info;
    }

    IRInst* inst = emit_inst(IROp::Closure, expr->resolved_type);
    if (!inst) return ValueId::invalid();
    inst->closure.env_struct_name = env_struct_name;
    inst->closure.call_function_name = trampoline_name;
    inst->closure.captures = Span<ValueId>();
    return inst->result;
}

ValueId IRBuilder::gen_literal_expr(Expr* expr) {
    LiteralExpr& lit = expr->literal;

    switch (lit.literal_kind) {
        case LiteralKind::Nil:
            return emit_const_null();
        case LiteralKind::Bool:
            return emit_const_bool(lit.bool_value);
        case LiteralKind::I32:
        case LiteralKind::I64:
        case LiteralKind::U32:
        case LiteralKind::U64: {
            // Safety net: if IntLiteral wasn't concretized, default to i32
            Type* int_type = expr->resolved_type;
            if (int_type && int_type->is_int_literal()) {
                int_type = m_types.i32_type();
            }
            return emit_const_int(lit.int_value, int_type);
        }
        case LiteralKind::F32:
        case LiteralKind::F64:
            return emit_const_float(lit.float_value, expr->resolved_type);
        case LiteralKind::String:
            return emit_const_string(lit.string_value);
    }
    report_error("Internal error: unknown literal kind in IR generation");
    return ValueId::invalid();
}

ValueId IRBuilder::gen_identifier_expr(Expr* expr) {
    IdentifierExpr& id = expr->identifier;

    // Function reference: if the identifier doesn't resolve to a local but does
    // resolve to a top-level function symbol, materialize a closure value
    // wrapping a synthesized trampoline. gen_call_expr's direct-call path
    // doesn't recurse here for callee identifiers, so this only fires in value
    // contexts (var init, argument passing, return, struct literal field, ...).
    if (LocalVar* lv = find_local(id.name); !lv) {
        // Generic-template ref: semantic analysis stashed the monomorphized
        // name on the identifier post-coercion. The instantiated function
        // type lives in expr->resolved_type. Apply module-local mangling
        // when the template is non-pub, mirroring what build_function does
        // when emitting the IR for the instance.
        if (id.mangled_name.size() > 0) {
            FunctionRefTarget target;
            target.kind = FunctionRefTarget::Kind::Script;
            target.function_type = expr->resolved_type;
            bool template_is_pub = false;
            if (Decl* tdecl = m_type_env.generics().get_generic_fun_decl(id.name);
                tdecl && tdecl->kind == AstKind::DeclFun) {
                template_is_pub = tdecl->fun_decl.is_pub;
            }
            target.name = template_is_pub
                ? id.mangled_name
                : mangle_module_local(id.mangled_name);
            return gen_function_ref(expr, target);
        }
        Symbol* sym = m_symbols.lookup(id.name);
        if (sym && sym->kind == SymbolKind::Function) {
            FunctionRefTarget target;
            target.function_type = sym->type;
            bool is_native = sym->decl && sym->decl->kind == AstKind::DeclFun
                && sym->decl->fun_decl.is_native;
            if (is_native) {
                target.kind = FunctionRefTarget::Kind::Native;
                target.name = sym->name;
                i32 idx = m_registry.get_index(sym->name);
                if (idx < 0) {
                    report_error("Internal error: native function not in registry");
                    return ValueId::invalid();
                }
                target.native_index = static_cast<u32>(idx);
            } else {
                target.kind = FunctionRefTarget::Kind::Script;
                target.name = sym->name;
                if (!sym->is_pub && sym->name != StringView("main", 4)) {
                    target.name = mangle_module_local(sym->name);
                }
            }
            return gen_function_ref(expr, target);
        }
        if (sym && sym->kind == SymbolKind::ImportedFunction) {
            FunctionRefTarget target;
            target.function_type = sym->type;
            if (sym->imported_func.is_native) {
                target.kind = FunctionRefTarget::Kind::ImportedNative;
                target.name = sym->imported_func.original_name;
                i32 idx = m_registry.get_index(target.name);
                if (idx < 0) {
                    report_error("Internal error: imported native not in registry");
                    return ValueId::invalid();
                }
                target.native_index = static_cast<u32>(idx);
            } else {
                target.kind = FunctionRefTarget::Kind::ImportedScript;
                target.name = sym->imported_func.original_name;
                target.module_name = sym->imported_func.module_name;
            }
            return gen_function_ref(expr, target);
        }
    }

    ValueId val = lookup_local(id.name);

    // If this is a pointer parameter (out/inout), handle specially
    if (m_param_is_ptr.count(id.name)) {
        Type* type = expr->resolved_type;

        // For struct types, the pointer IS what we need for field access.
        // Don't dereference - struct operations (GET_FIELD, SET_FIELD) expect pointers.
        if (type && type->is_struct()) {
            return val;
        }

        // For primitive types and pointer-sized types, dereference the pointer to get the value.
        // get_type_slot_count covers every non-struct width (incl. weak=4, uniq/ref/fn=2);
        // structs returned above. 0 means null/opaque type — preserve the prior 1-slot default.
        u32 slot_count = get_type_slot_count(type);
        if (slot_count == 0) slot_count = 1;
        return emit_load_ptr(val, slot_count, type);
    }

    return val;
}

ValueId IRBuilder::gen_unary_expr(Expr* expr) {
    UnaryExpr& unary_expr = expr->unary;

    // Check for struct unary operator trait dispatch
    Type* operand_type = unary_expr.operand->resolved_type;
    if (operand_type && operand_type->is_struct()) {
        const char* method_name_str = unary_op_to_trait_method(unary_expr.op);
        if (method_name_str) {
            StringView method_name(method_name_str, static_cast<u32>(strlen(method_name_str)));
            Type* found_in = nullptr;
            const MethodInfo* mi = lookup_method_in_hierarchy(operand_type, method_name, &found_in);
            if (mi && found_in) {
                ValueId self_ptr = gen_lvalue_addr(unary_expr.operand);
                StringView mangled = mangle_method(found_in->struct_info.name, method_name);
                Span<ValueId> args = alloc_span<ValueId>(1);
                args[0] = self_ptr;
                return emit_call(mangled, args, expr->resolved_type);
            }
        }
    }

    // ref expr: borrow a reference — just pass through the value with ref type
    if (unary_expr.op == UnaryOp::Ref) {
        ValueId operand = gen_expr(unary_expr.operand);
        Type* result_type = expr->resolved_type;
        // uniq and ref have the same runtime representation, just reinterpret the type
        return emit_unary(IROp::Copy, operand, result_type);
    }

    ValueId operand = gen_expr(unary_expr.operand);
    Type* result_type = expr->resolved_type;

    IROp op = get_unary_op(unary_expr.op, operand_type);
    return emit_unary(op, operand, result_type);
}

ValueId IRBuilder::gen_binary_expr(Expr* expr) {
    BinaryExpr& binary_expr = expr->binary;

    // Reference/nil comparison: ref/uniq/weak == nil or != nil
    Type* left_type = binary_expr.left->resolved_type;
    Type* right_type = binary_expr.right->resolved_type;
    if ((binary_expr.op == BinaryOp::Equal || binary_expr.op == BinaryOp::NotEqual) &&
        ((left_type && left_type->is_reference() && right_type && right_type->is_nil()) ||
         (left_type && left_type->is_nil() && right_type && right_type->is_reference()))) {
        ValueId ref_val, null_val;
        if (left_type->is_reference()) {
            ref_val = gen_expr(binary_expr.left);
            null_val = emit_const_null();
        } else {
            null_val = emit_const_null();
            ref_val = gen_expr(binary_expr.right);
        }
        IROp cmp_op = (binary_expr.op == BinaryOp::Equal) ? IROp::EqI : IROp::NeI;
        return emit_binary(cmp_op, ref_val, null_val, m_types.bool_type());
    }

    // Check for string operations - rewrite to native function calls
    if (left_type && left_type->kind == TypeKind::String) {
        ValueId left = gen_expr(binary_expr.left);
        ValueId right = gen_expr(binary_expr.right);

        StringView func_name;
        Type* result_type = nullptr;
        i32 native_idx = -1;

        switch (binary_expr.op) {
            case BinaryOp::Add:
                func_name = "str_concat";
                result_type = m_types.string_type();
                native_idx = m_registry.get_index(func_name);
                break;
            case BinaryOp::Equal:
                func_name = "str_eq";
                result_type = m_types.bool_type();
                native_idx = m_registry.get_index(func_name);
                break;
            case BinaryOp::NotEqual:
                func_name = "str_ne";
                result_type = m_types.bool_type();
                native_idx = m_registry.get_index(func_name);
                break;
            default:
                // Unsupported string operation - fall through to regular handling
                // (will likely cause a type error)
                break;
        }

        if (native_idx >= 0) {
            Span<ValueId> args = alloc_span<ValueId>(2);
            args[0] = left;
            args[1] = right;
            return emit_call_native(func_name, args, result_type, static_cast<u8>(native_idx));
        }
    }

    // Check for struct operator trait dispatch
    if (left_type && left_type->is_struct()) {
        const char* method_name_str = binary_op_to_trait_method(binary_expr.op);
        if (method_name_str) {
            StringView method_name(method_name_str, static_cast<u32>(strlen(method_name_str)));
            Type* found_in = nullptr;
            const MethodInfo* mi = lookup_method_in_hierarchy(left_type, method_name, &found_in);
            if (mi && found_in) {
                // Generate a method call: left.method(right)
                ValueId self_ptr = gen_lvalue_addr(binary_expr.left);
                ValueId right_val = gen_expr(binary_expr.right);

                StringView mangled = mangle_method(found_in->struct_info.name, method_name);

                Span<ValueId> args = alloc_span<ValueId>(2);
                args[0] = self_ptr;
                args[1] = right_val;

                return emit_call(mangled, args, expr->resolved_type);
            }
        }
    }

    // Short-circuit evaluation for && and ||
    if (binary_expr.op == BinaryOp::And) {
        ValueId left = gen_expr(binary_expr.left);

        IRBlock* right_block = create_block("and.rhs");
        IRBlock* merge_block = create_block("and.end");

        // Merge block receives the result via a block argument
        ValueId result_param = m_current_func->new_value();
        merge_block->params.push_back({result_param, m_types.bool_type(), {}});

        // If left is false, short-circuit: result is false
        ValueId false_val = emit_const_bool(false);
        Span<BlockArgPair> short_circuit_args = alloc_span<BlockArgPair>(1);
        short_circuit_args[0] = {false_val};
        finish_block_branch(left, right_block->id, merge_block->id, {}, short_circuit_args);

        // Evaluate right side, pass result to merge
        set_current_block(right_block);
        ValueId right = gen_expr(binary_expr.right);
        Span<BlockArgPair> right_args = alloc_span<BlockArgPair>(1);
        right_args[0] = {right};
        finish_block_goto(merge_block->id, right_args);

        set_current_block(merge_block);
        return result_param;
    }
    else if (binary_expr.op == BinaryOp::Or) {
        ValueId left = gen_expr(binary_expr.left);

        IRBlock* right_block = create_block("or.rhs");
        IRBlock* merge_block = create_block("or.end");

        // Merge block receives the result via a block argument
        ValueId result_param = m_current_func->new_value();
        merge_block->params.push_back({result_param, m_types.bool_type(), {}});

        // If left is true, short-circuit: result is true
        ValueId true_val = emit_const_bool(true);
        Span<BlockArgPair> short_circuit_args = alloc_span<BlockArgPair>(1);
        short_circuit_args[0] = {true_val};
        finish_block_branch(left, merge_block->id, right_block->id, short_circuit_args, {});

        // Evaluate right side, pass result to merge
        set_current_block(right_block);
        ValueId right = gen_expr(binary_expr.right);
        Span<BlockArgPair> right_args = alloc_span<BlockArgPair>(1);
        right_args[0] = {right};
        finish_block_goto(merge_block->id, right_args);

        set_current_block(merge_block);
        return result_param;
    }

    // Regular binary operations
    ValueId left = gen_expr(binary_expr.left);
    ValueId right = gen_expr(binary_expr.right);
    Type* result_type = expr->resolved_type;
    Type* operand_type = binary_expr.left->resolved_type;

    // Check if it's a comparison or arithmetic operation
    IROp op;
    switch (binary_expr.op) {
        case BinaryOp::Equal:
        case BinaryOp::NotEqual:
        case BinaryOp::Less:
        case BinaryOp::LessEq:
        case BinaryOp::Greater:
        case BinaryOp::GreaterEq:
            op = get_comparison_op(binary_expr.op, operand_type);
            break;
        default:
            op = get_binary_op(binary_expr.op, operand_type);
            break;
    }

    return emit_binary(op, left, right, result_type);
}

ValueId IRBuilder::gen_ternary_expr(Expr* expr) {
    TernaryExpr& ternary_expr = expr->ternary;

    ValueId cond = gen_expr(ternary_expr.condition);

    IRBlock* then_block = create_block("tern.then");
    IRBlock* else_block = create_block("tern.else");
    IRBlock* merge_block = create_block("tern.end");

    // Merge block takes the ternary result as a phi parameter so both branches
    // can contribute their value. Previously this returned else_val directly,
    // leaving the merge block's register undefined on the then-path and
    // producing garbage at runtime.
    Type* result_type = expr->resolved_type;
    ValueId phi = m_current_func->new_value();
    merge_block->params.push_back({phi, result_type, StringView()});

    finish_block_branch(cond, then_block->id, else_block->id);

    // Then branch
    set_current_block(then_block);
    ValueId then_val = gen_expr(ternary_expr.then_expr);
    {
        Vector<BlockArgPair> args;
        args.push_back({then_val});
        finish_block_goto(merge_block->id, alloc_span(args));
    }

    // Else branch
    set_current_block(else_block);
    ValueId else_val = gen_expr(ternary_expr.else_expr);
    {
        Vector<BlockArgPair> args;
        args.push_back({else_val});
        finish_block_goto(merge_block->id, alloc_span(args));
    }

    set_current_block(merge_block);
    return phi;
}

ValueId IRBuilder::emit_call_resolved(StringView name, Span<ValueId> args, Type* result_type) {
    i32 native_idx = m_registry.get_index(name);
    if (native_idx >= 0) {
        return emit_call_native(name, args, result_type, static_cast<u8>(native_idx));
    }
    return emit_call(name, args, result_type);
}

ValueId IRBuilder::emit_call_indirect(ValueId callee_val, Span<ValueId> args, Type* result_type) {
    IRInst* call_inst = emit_inst(IROp::CallIndirect, result_type);
    if (!call_inst) return ValueId::invalid();
    call_inst->call_indirect.callee = callee_val;
    call_inst->call_indirect.args = args;
    return call_inst->result;
}

Span<ValueId> IRBuilder::prepend_self(ValueId self, Span<ValueId> args, ValueId output_ptr) {
    bool has_output = output_ptr.is_valid();
    u32 extra = has_output ? 2 : 1;
    Span<ValueId> result = alloc_span<ValueId>(static_cast<u32>(args.size()) + extra);
    result[0] = self;
    for (u32 i = 0; i < args.size(); i++) {
        result[i + 1] = args[i];
    }
    if (has_output) {
        result[args.size() + 1] = output_ptr;
    }
    return result;
}

i32 IRBuilder::find_method_fn_index(Type* struct_type, StringView method_name) {
    if (!m_module) return -1;
    Type* found_in = nullptr;
    const MethodInfo* method_info = m_types.lookup_method(struct_type, method_name, &found_in);
    if (!method_info || !found_in) return -1;
    StringView mangled = mangle_method(found_in->struct_info.name, method_name);
    for (u32 fi = 0; fi < m_module->functions.size(); fi++) {
        if (m_module->functions[fi]->name == mangled) {
            return static_cast<i32>(fi);
        }
    }
    return -1;
}

ValueId IRBuilder::gen_list_constructor(Expr* expr) {
    // List<T>() / List<T>(cap): two-step allocate+init matching struct constructors.
    //   1. Call alloc native (element layout args) to get the list pointer
    //   2. Call the constructor method with [self, user_args...]
    CallExpr& call_expr = expr->call;
    Type* list_type = call_expr.callee->resolved_type;

    // Generate user arguments first
    u32 user_argc = static_cast<u32>(call_expr.arguments.size());
    Span<ValueId> user_args = alloc_span<ValueId>(user_argc);
    for (u32 i = 0; i < user_argc; i++) {
        user_args[i] = gen_expr(call_expr.arguments[i].expr);
    }

    // Step 1: Allocate empty list with element_slot_count and element_is_inline args
    StringView alloc_name = list_type->list_info.alloc_native_name;
    i32 alloc_idx = m_registry.get_index(alloc_name);
    Type* elem_type = list_type->list_info.element_type;
    u32 esc = get_type_slot_count(elem_type);
    bool is_inline = !elem_type->is_struct();
    ValueId esc_val = emit_const_int(static_cast<i64>(esc), m_types.i32_type());
    ValueId inline_val = emit_const_int(is_inline ? 1 : 0, m_types.i32_type());
    Span<ValueId> alloc_args = alloc_span<ValueId>(2);
    alloc_args[0] = esc_val;
    alloc_args[1] = inline_val;
    ValueId list_ptr = emit_call_native(alloc_name, alloc_args, expr->resolved_type,
                                        static_cast<u8>(alloc_idx));

    // Step 2: Call constructor method with [self, user_args...]
    StringView ctor_name = call_expr.mangled_name;  // "List$$new"
    i32 ctor_idx = m_registry.get_index(ctor_name);
    Span<ValueId> ctor_args = prepend_self(list_ptr, user_args);
    emit_call_native(ctor_name, ctor_args, m_types.void_type(), static_cast<u8>(ctor_idx));

    return list_ptr;
}

ValueId IRBuilder::gen_map_constructor(Expr* expr) {
    // Map<K,V>() / Map<K,V>(cap): like the List constructor but injects a hidden
    // key_kind argument, and for struct keys passes the user's hash/eq fn indices.
    CallExpr& call_expr = expr->call;
    Type* map_resolved_type = call_expr.callee->resolved_type;

    // Generate user arguments first (0 or 1 capacity arg)
    u32 user_argc = static_cast<u32>(call_expr.arguments.size());
    Span<ValueId> user_args = alloc_span<ValueId>(user_argc);
    for (u32 i = 0; i < user_argc; i++) {
        user_args[i] = gen_expr(call_expr.arguments[i].expr);
    }

    // Step 1: Allocate empty map with key/value layout. Both keys and values
    // support variable slot counts; for primitive keys the layout is 2-slot inline
    // (the u64 register packs the value), for struct keys the layout matches the
    // struct's slot count and the runtime expects a pointer to the bytes.
    StringView alloc_name = map_resolved_type->map_info.alloc_native_name;
    i32 alloc_idx = m_registry.get_index(alloc_name);
    Type* key_type = map_resolved_type->map_info.key_type;
    Type* value_type = map_resolved_type->map_info.value_type;
    u32 ksc = key_type->is_struct() ? get_type_slot_count(key_type) : 2u;
    bool key_is_inline = !key_type->is_struct();
    u32 vsc = get_type_slot_count(value_type);
    bool value_is_inline = !value_type->is_struct();

    // For struct keys, look up the user's hash()/eq() methods and pass their bytecode
    // indices to the runtime (called via call_user_function during bucket probing).
    // -1 means "no custom impl, use bytewise dispatch". Custom dispatch fires only
    // when the struct EXPLICITLY implements both Hash and Eq via `for Hash` / `for Eq`
    // — just defining hash()/eq() is not enough, matching Rust's HashMap requiring
    // `impl Hash` + `impl Eq`.
    i32 hash_fn_index = -1;
    i32 eq_fn_index = -1;
    if (key_type->is_struct() && m_module) {
        Type* hash_trait = m_type_env.hash_type();
        Type* eq_trait = m_type_env.eq_type();
        if (hash_trait && m_types.implements_trait(key_type, hash_trait)) {
            hash_fn_index = find_method_fn_index(key_type, StringView("hash", 4));
        }
        if (eq_trait && m_types.implements_trait(key_type, eq_trait)) {
            eq_fn_index = find_method_fn_index(key_type, StringView("eq", 2));
        }
    }

    ValueId ksc_val = emit_const_int(static_cast<i64>(ksc), m_types.i32_type());
    ValueId kii_val = emit_const_int(key_is_inline ? 1 : 0, m_types.i32_type());
    ValueId vsc_val = emit_const_int(static_cast<i64>(vsc), m_types.i32_type());
    ValueId vii_val = emit_const_int(value_is_inline ? 1 : 0, m_types.i32_type());
    ValueId hash_val = emit_const_int(static_cast<i64>(hash_fn_index), m_types.i32_type());
    ValueId eq_val = emit_const_int(static_cast<i64>(eq_fn_index), m_types.i32_type());
    Span<ValueId> alloc_args = alloc_span<ValueId>(6);
    alloc_args[0] = ksc_val;
    alloc_args[1] = kii_val;
    alloc_args[2] = vsc_val;
    alloc_args[3] = vii_val;
    alloc_args[4] = hash_val;
    alloc_args[5] = eq_val;
    ValueId map_ptr = emit_call_native(alloc_name, alloc_args, expr->resolved_type,
                                       static_cast<u8>(alloc_idx));

    // Step 2: Call constructor with [self, key_kind, user_args...]. Determine
    // MapKeyKind from the key type.
    i32 key_kind_val = static_cast<i32>(MapKeyKind::Integer);  // default
    if (key_type->kind == TypeKind::F32) {
        key_kind_val = static_cast<i32>(MapKeyKind::Float32);
    } else if (key_type->kind == TypeKind::F64) {
        key_kind_val = static_cast<i32>(MapKeyKind::Float64);
    } else if (key_type->kind == TypeKind::String) {
        key_kind_val = static_cast<i32>(MapKeyKind::String);
    } else if (key_type->is_struct()) {
        key_kind_val = static_cast<i32>(MapKeyKind::Struct);
    }
    ValueId key_kind_const = emit_const_int(static_cast<i64>(key_kind_val), m_types.i32_type());

    StringView ctor_name = call_expr.mangled_name;  // "Map$$new"
    i32 ctor_idx = m_registry.get_index(ctor_name);
    // Constructor args: [self, key_kind, optional_capacity]
    Span<ValueId> ctor_args = alloc_span<ValueId>(user_argc + 2);
    ctor_args[0] = map_ptr;           // self
    ctor_args[1] = key_kind_const;    // key_kind (hidden)
    for (u32 i = 0; i < user_argc; i++) {
        ctor_args[i + 2] = user_args[i];
    }
    emit_call_native(ctor_name, ctor_args, m_types.void_type(), static_cast<u8>(ctor_idx));

    return map_ptr;
}

IRBuilder::CallLowering IRBuilder::lower_call_args(Expr* expr) {
    CallExpr& call_expr = expr->call;
    CallLowering lowered;

    // Callee returning a large struct gets a hidden output pointer (stack slot).
    Type* callee_return_type = expr->resolved_type;
    lowered.returns_large_struct = callee_return_type &&
        callee_return_type->is_struct() &&
        callee_return_type->struct_info.slot_count > 4;
    if (lowered.returns_large_struct) {
        lowered.output_ptr = emit_stack_alloc(callee_return_type->struct_info.slot_count,
                                              callee_return_type);
    }

    // Evaluate arguments - for out/inout args, pass address instead of value
    Span<ValueId> args = alloc_span<ValueId>(call_expr.arguments.size());
    for (u32 i = 0; i < call_expr.arguments.size(); i++) {
        CallArg& arg = call_expr.arguments[i];
        if (arg.modifier != ParamModifier::None) {
            // Pass address of lvalue
            args[i] = gen_lvalue_addr(arg.expr);

            // Track primitive inout/out identifiers for post-call reload. Structs are
            // modified in place through the pointer, so they need no reload.
            if (arg.modifier == ParamModifier::Inout || arg.modifier == ParamModifier::Out) {
                if (arg.expr->kind == AstKind::ExprIdentifier && !m_param_is_ptr.count(arg.expr->identifier.name)) {
                    Type* type = arg.expr->resolved_type;
                    if (type && type->is_struct()) {
                        continue;
                    }
                    // Structs skipped above; get_type_slot_count gives the correct width
                    // for every remaining type (weak=4, uniq/ref/list/map/string/fn=2).
                    u32 slot_count = get_type_slot_count(type);
                    if (slot_count == 0) slot_count = 1;
                    lowered.inout_args.push_back({arg.expr->identifier.name, args[i], type, slot_count});
                }
            }
        } else {
            args[i] = gen_expr(arg.expr);

            // Consume noncopyable temporaries (ownership transfers to callee).
            // Nullify is a compile-time annotation — it ends the cleanup record
            // scope so exception cleanup skips this value after the transfer.
            if (arg.expr->resolved_type && arg.expr->resolved_type->noncopyable()) {
                consume_temp_noncopyable(args[i]);
                // `f(o.field)`: null the moved-out field in the root (args[i]
                // already read its value above) so the root's destructor no-ops it.
                nullify_moved_field_source(arg.expr);
            }

            // Wrap uniq/ref → weak conversion for call arguments
            Type* callee_func_type = call_expr.callee->resolved_type;
            if (callee_func_type) callee_func_type = callee_func_type->base_type();
            if (callee_func_type && callee_func_type->is_function() &&
                i < callee_func_type->func_info.param_types.size()) {
                Type* param_type = callee_func_type->func_info.param_types[i];
                args[i] = maybe_wrap_weak(args[i], arg.expr->resolved_type, param_type);
            }
        }
    }
    lowered.args = args;

    // For large struct returns, append the output pointer to arguments
    if (lowered.returns_large_struct) {
        Span<ValueId> final_args = alloc_span<ValueId>(args.size() + 1);
        for (u32 i = 0; i < args.size(); i++) {
            final_args[i] = args[i];
        }
        final_args[args.size()] = lowered.output_ptr;
        lowered.final_args = final_args;
    } else {
        lowered.final_args = args;
    }

    return lowered;
}

ValueId IRBuilder::gen_call_direct(Expr* expr, const CallLowering& lowered) {
    CallExpr& call_expr = expr->call;
    Span<ValueId> final_args = lowered.final_args;

    // Original (unmangled) callee name from source. For generic calls this is the
    // template name ("helper"); the symbol table is keyed by the template name, not
    // the monomorphized name ("helper$i32"), so we look up via orig_name.
    StringView orig_name = call_expr.callee->identifier.name;
    // Use the mangled name for generic function calls (e.g., "identity$i32")
    StringView func_name = call_expr.mangled_name.size() > 0 ? call_expr.mangled_name : orig_name;
    StringView lookup_name = func_name;

    // Imported functions may have an alias; use the original name for native lookup.
    Symbol* sym = m_symbols.lookup(orig_name);
    if (sym && sym->kind == SymbolKind::ImportedFunction) {
        lookup_name = sym->imported_func.original_name;
    }

    ValueId result;
    // Indirect call: callee is a local holding a closure value (Function-typed).
    // Detect via the local scope map — symbol lookups don't see function-body locals.
    if (LocalVar* lv = find_local(orig_name); lv && lv->type && lv->type->base_type()->is_function()) {
        ValueId closure_val = gen_identifier_expr(call_expr.callee);
        result = emit_call_indirect(closure_val, final_args, expr->resolved_type);
    }
    // Native function
    else if (i32 native_idx = m_registry.get_index(lookup_name); native_idx >= 0) {
        result = emit_call_native(lookup_name, final_args, expr->resolved_type, static_cast<u8>(native_idx));
    } else {
        // Module-scope non-pub functions are mangled at definition (see build_function);
        // calls to them must use the mangled name so they resolve within the same module.
        StringView emit_name = func_name;
        // For generic calls the template lives in the GenericInstantiator rather than
        // the symbol table, so `sym` is null. Look up the template there for its is_pub
        // (build_function uses the same is_pub when emitting the instance).
        bool is_pub = false;
        bool is_function_symbol = false;
        if (sym && sym->kind == SymbolKind::Function) {
            is_function_symbol = true;
            is_pub = sym->is_pub;
        } else if (call_expr.mangled_name.size() > 0 &&
                   m_type_env.generics().is_generic_fun(orig_name)) {
            Decl* template_decl = m_type_env.generics().get_generic_fun_decl(orig_name);
            if (template_decl && template_decl->kind == AstKind::DeclFun) {
                is_function_symbol = true;
                is_pub = template_decl->fun_decl.is_pub;
            }
        }
        if (is_function_symbol && !is_pub
            && orig_name != StringView("main", 4)) {
            emit_name = mangle_module_local(func_name);
        }
        result = emit_call(emit_name, final_args, expr->resolved_type);
    }

    // For large struct returns, the result is the output pointer (already allocated)
    if (lowered.returns_large_struct) {
        result = lowered.output_ptr;
    }
    return result;
}

ValueId IRBuilder::gen_call_member(Expr* expr, const CallLowering& lowered) {
    CallExpr& call_expr = expr->call;
    GetExpr& get_expr = call_expr.callee->get;
    Span<ValueId> args = lowered.args;
    Span<ValueId> final_args = lowered.final_args;

    // Module-qualified call: module.function(). The object's resolved_type is null
    // for module references.
    if (get_expr.object->kind == AstKind::ExprIdentifier && get_expr.object->resolved_type == nullptr) {
        StringView module_name = get_expr.object->identifier.name;
        StringView func_name = get_expr.name;
        // The function name is just the member name for the native registry.
        i32 native_idx = m_registry.get_index(func_name);
        ValueId result;
        if (native_idx >= 0) {
            result = emit_call_native(func_name, final_args, expr->resolved_type, static_cast<u8>(native_idx));
        } else {
            result = emit_call_external(module_name, func_name, final_args, expr->resolved_type);
        }
        if (lowered.returns_large_struct) result = lowered.output_ptr;
        return result;
    }

    // Method call: obj.method(args)
    ValueId obj = gen_expr(get_expr.object);
    Type* obj_type = get_expr.object->resolved_type;
    Type* struct_type = obj_type ? obj_type->base_type() : nullptr;

    // Coro method call (resume/done — lowered functions): self is the coroutine object.
    if (struct_type && struct_type->is_coroutine()) {
        Span<ValueId> method_args = prepend_self(obj, args);
        return emit_call(call_expr.mangled_name, method_args, expr->resolved_type);
    }

    // "Field of function type": obj.callback(args) where callback is a struct field
    // whose type is fun(...) -> R. Checked before method dispatch since a field name
    // could collide with a method name (and we want the field).
    const FieldInfo* fn_field = (struct_type && struct_type->is_struct())
        ? struct_type->struct_info.find_field(get_expr.name) : nullptr;
    if (fn_field && fn_field->type && fn_field->type->base_type()->is_function()) {
        // Read the closure value from the field, then CALL_INDIRECT.
        ValueId closure_val = gen_expr(call_expr.callee);
        return emit_call_indirect(closure_val, final_args, expr->resolved_type);
    }

    // List/Map builtin native method.
    if (struct_type && struct_type->is_container()) {
        StringView native_name = call_expr.mangled_name;
        i32 native_idx = m_registry.get_index(native_name);
        Span<ValueId> method_args = prepend_self(obj, args);
        return emit_call_native(native_name, method_args, expr->resolved_type, static_cast<u8>(native_idx));
    }

    // User struct method. Look up the method in the type hierarchy to find where it's
    // defined, so mangling uses the correct struct name (important for inheritance).
    Type* method_owner = nullptr;
    StringView method_name = get_expr.name;
    if (struct_type && struct_type->is_struct()) {
        lookup_method_in_hierarchy(struct_type, get_expr.name, &method_owner);
        Type* name_type = method_owner ? method_owner : struct_type;
        method_name = mangle_method(name_type->struct_info.name, get_expr.name);
    }

    // [obj] + args, with a trailing output pointer when this returns a large struct
    // (output_ptr is invalid otherwise, so prepend_self appends nothing).
    Span<ValueId> method_args = prepend_self(obj, args, lowered.output_ptr);
    ValueId result = emit_call_resolved(method_name, method_args, expr->resolved_type);
    if (lowered.returns_large_struct) result = lowered.output_ptr;
    return result;
}

void IRBuilder::reload_inout_args(const CallLowering& lowered) {
    // After the call, reload inout variables from their stack addresses.
    for (const InoutArg& ia : lowered.inout_args) {
        ValueId new_val = emit_load_ptr(ia.addr, ia.slot_count, ia.type);
        define_local(ia.name, new_val, ia.type);
    }
}

void IRBuilder::mark_call_args_moved(Expr* expr) {
    CallExpr& call_expr = expr->call;
    // Mark owned args passed to owned params as moved (suppresses scope-exit cleanup;
    // for uniq, mark_moved_from also nulls the register). Skip inout/out: those pass a
    // pointer to the caller's slot, ownership stays with the caller — marking them moved
    // would trip a false use-after-move on the next loop iteration and (for noncopyable
    // types) null out a local the caller still owns.
    Type* callee_func_type = call_expr.callee->resolved_type;
    if (callee_func_type) callee_func_type = callee_func_type->base_type();
    if (!callee_func_type || !callee_func_type->is_function()) return;
    Span<Type*> param_types = callee_func_type->func_info.param_types;
    // Method calls include implicit 'self' as param_types[0]; user args start at 1.
    u32 param_offset = (call_expr.callee->kind == AstKind::ExprGet) ? 1 : 0;
    for (u32 i = 0; i < call_expr.arguments.size() && (i + param_offset) < param_types.size(); i++) {
        const CallArg& arg = call_expr.arguments[i];
        if (arg.modifier != ParamModifier::None) continue;
        if (arg.expr->kind != AstKind::ExprIdentifier) continue;
        Type* arg_type = arg.expr->resolved_type;
        Type* param_type = param_types[i + param_offset];
        if (arg_type && arg_type->noncopyable() && param_type && param_type->noncopyable()) {
            // For uniq: null-ify so DEL_OBJ on scope exit is a safe no-op. For value
            // structs: the bitwise copy IS the move, just suppress cleanup.
            mark_moved_from(arg.expr->identifier.name);
        }
    }
}

ValueId IRBuilder::gen_call_expr(Expr* expr) {
    CallExpr& call_expr = expr->call;
    Type* callee_type = call_expr.callee->resolved_type;

    // Type-driven early delegations when the callee is a bare identifier.
    if (call_expr.callee->kind == AstKind::ExprIdentifier && callee_type) {
        if (callee_type->is_primitive() && !callee_type->is_void()) {
            return gen_primitive_cast(expr);     // i32(x), f64(y), ...
        }
        if (callee_type->is_struct()) {
            return gen_constructor_call(expr);   // Foo(...)
        }
        if (callee_type->is_list()) {
            return gen_list_constructor(expr);   // List<T>() / List<T>(cap)
        }
        if (callee_type->is_map()) {
            return gen_map_constructor(expr);    // Map<K,V>() / Map<K,V>(cap)
        }
    }

    // Check if this is a named constructor call: Type.ctor_name(...)
    // The callee is a GetExpr where the object is a type name (not a variable)
    // For named constructors: ge.object is an identifier that resolves to a STRUCT TYPE (not a variable of struct type)
    // This is detected by checking if the identifier matches a type name
    if (call_expr.callee->kind == AstKind::ExprGet) {
        GetExpr& get_expr = call_expr.callee->get;
        if (get_expr.object->kind == AstKind::ExprIdentifier) {
            // Check if this is a type name (constructor) or a variable (method call)
            StringView name = get_expr.object->identifier.name;
            Type* named_type = m_type_env.named_type_by_name(name);
            if (named_type && named_type->is_struct()) {
                // This is a named constructor call: Type.ctor_name(...)
                return gen_constructor_call(expr);
            }
            // Otherwise, it's a method call on a variable - fall through
        }
    }

    // Check if this is a super call: super() / super.ctor_name() / super.method_name()
    if (call_expr.callee->kind == AstKind::ExprSuper) {
        return gen_super_call(expr);
    }

    // Lower arguments once (out/inout addresses, large-struct output pointer, weak
    // wrapping, temp consumption), then dispatch on the callee's shape.
    CallLowering lowered = lower_call_args(expr);

    ValueId result;
    if (call_expr.callee->kind == AstKind::ExprIdentifier) {
        result = gen_call_direct(expr, lowered);
    }
    else if (call_expr.callee->kind == AstKind::ExprGet) {
        result = gen_call_member(expr, lowered);
    }
    else if (callee_type && callee_type->base_type()->is_function()) {
        // General indirect call: callee is some Function-typed expression (call
        // result, index, field access, ...) — including a borrowed `ref fun`,
        // which shares the env-pointer representation. Evaluate and CALL_INDIRECT.
        ValueId closure_val = gen_expr(call_expr.callee);
        result = emit_call_indirect(closure_val, lowered.final_args, expr->resolved_type);
    }
    else {
        report_error("Internal error: unhandled call expression kind");
        return ValueId::invalid();
    }

    // If a dispatch helper bailed because the block was terminated, stop here to
    // match the original early-return (no inout reload / move-marking on a dead block).
    if (!m_current_block) return result;

    reload_inout_args(lowered);
    mark_call_args_moved(expr);
    return result;
}

ValueId IRBuilder::gen_index_expr(Expr* expr) {
    IndexExpr& index_expr = expr->index;

    Type* obj_type = index_expr.object->resolved_type;
    Type* base_type = obj_type ? obj_type->base_type() : nullptr;

    // Struct indexing: dispatch to "index" method
    if (base_type && base_type->is_struct()) {
        StringView method_name("index", 5);
        Type* found_in = nullptr;
        const MethodInfo* method_info = lookup_method_in_hierarchy(base_type, method_name, &found_in);
        if (method_info && found_in) {
            ValueId self_ptr = gen_lvalue_addr(index_expr.object);
            ValueId index_val = gen_expr(index_expr.index);
            StringView mangled = mangle_method(found_in->struct_info.name, method_name);
            Span<ValueId> args = alloc_span<ValueId>(2);
            args[0] = self_ptr;
            args[1] = index_val;
            return emit_call(mangled, args, expr->resolved_type);
        }
    }

    // List/Map indexing: emit IndexGet IR op
    if (base_type && base_type->is_container()) {
        ValueId obj = gen_expr(index_expr.object);
        ValueId index_val = gen_expr(index_expr.index);
        ContainerKind kind = base_type->is_list() ? ContainerKind::List : ContainerKind::Map;
        return emit_index_get(obj, index_val, kind, expr->resolved_type);
    }

    report_error("Internal error: index operation not supported");
    return ValueId::invalid();
}

ValueId IRBuilder::gen_get_expr(Expr* expr) {
    GetExpr& get_expr = expr->get;

    ValueId obj = gen_expr(get_expr.object);

    // Get the struct type from the object
    Type* obj_type = get_expr.object->resolved_type;
    Type* struct_type = obj_type ? obj_type->base_type() : nullptr;

    u32 slot_offset = 0;
    u32 slot_count = 1;
    Type* field_type = nullptr;
    bool is_variant_field = false;
    const WhenClauseInfo* when_clause = nullptr;
    const VariantInfo* variant = nullptr;

    if (struct_type && struct_type->is_struct()) {
        const FieldInfo* field_info = struct_type->struct_info.find_field(get_expr.name);
        if (field_info) {
            slot_offset = field_info->slot_offset;
            slot_count = field_info->slot_count;
            field_type = field_info->type;
        } else {
            // Check for variant field in when clauses
            const VariantFieldInfo* variant_field_info = struct_type->struct_info.find_variant_field(
                get_expr.name, &when_clause, &variant);
            if (variant_field_info) {
                is_variant_field = true;
                // Compute the actual offset: union_slot_offset + variant field's offset within the union
                slot_offset = when_clause->union_slot_offset + variant_field_info->slot_offset;
                slot_count = variant_field_info->slot_count;
                field_type = variant_field_info->type;
            }
        }
    }

    // If this is a variant field, emit runtime discriminant check
    if (is_variant_field && when_clause && variant) {
        // Load the discriminant
        ValueId disc = emit_get_field(obj, when_clause->discriminant_name,
                                      when_clause->discriminant_slot_offset, 1,
                                      when_clause->discriminant_type);

        // Create the expected discriminant value constant
        ValueId expected = emit_const_int(variant->discriminant_value,
                                          when_clause->discriminant_type);

        // Compare discriminant with expected value
        ValueId matches = emit_binary(IROp::EqI, disc, expected, m_types.bool_type());

        // Create pass and fail blocks
        IRBlock* pass_block = create_block("variant_check_pass");
        IRBlock* fail_block = create_block("variant_check_fail");

        // Branch based on discriminant check
        finish_block_branch(matches, pass_block->id, fail_block->id);

        // In fail block: emit unreachable (will lower to TRAP)
        set_current_block(fail_block);
        finish_block_unreachable();

        // Continue in pass block
        set_current_block(pass_block);
    }

    // If the field is a struct type, we need to compute its address (pointer)
    // instead of loading its value, since nested struct access needs the address.
    if (field_type && field_type->is_struct()) {
        return emit_get_field_addr(obj, get_expr.name, slot_offset, expr->resolved_type);
    }

    return emit_get_field(obj, get_expr.name, slot_offset, slot_count, expr->resolved_type);
}

ValueId IRBuilder::gen_assign_expr(Expr* expr) {
    AssignExpr& assign_expr = expr->assign;

    // Evaluate the RHS, then fold in any compound operator (`+=`, `-=`, ...).
    ValueId value = gen_expr(assign_expr.value);
    if (assign_expr.op != AssignOp::Assign) {
        bool handled = false;
        ValueId combined = gen_compound_assign(expr, value, handled);
        if (handled) return combined;  // struct trait dispatch did the whole assignment
        value = combined;
    }

    // Dispatch on the assignment target.
    switch (assign_expr.target->kind) {
        case AstKind::ExprIdentifier: return gen_assign_local(expr, value);
        case AstKind::ExprGet:        return gen_assign_field(expr, value);
        case AstKind::ExprIndex:      return gen_assign_index(expr, value);
        default:                      return value;
    }
}

ValueId IRBuilder::gen_compound_assign(Expr* expr, ValueId rhs, bool& handled) {
    AssignExpr& assign_expr = expr->assign;
    handled = false;
    Type* type = assign_expr.target->resolved_type;

    // Struct compound assignment trait dispatch (e.g. `a += b` -> Add's add_assign).
    if (type && type->is_struct()) {
        const char* method_name_str = assign_op_to_trait_method(assign_expr.op);
        if (method_name_str) {
            StringView method_name(method_name_str, static_cast<u32>(strlen(method_name_str)));
            Type* found_in = nullptr;
            const MethodInfo* mi = lookup_method_in_hierarchy(type, method_name, &found_in);
            if (mi && found_in) {
                ValueId self_ptr = gen_lvalue_addr(assign_expr.target);
                StringView mangled = mangle_method(found_in->struct_info.name, method_name);
                Span<ValueId> args = alloc_span<ValueId>(2);
                args[0] = self_ptr;
                args[1] = rhs;
                handled = true;
                return emit_call(mangled, args, m_types.void_type());
            }
        }
    }

    // Primitive compound assignment: fold the target's current value with the RHS.
    ValueId old_val = gen_expr(assign_expr.target);

    IROp op;
    switch (assign_expr.op) {
        case AssignOp::AddAssign:
            op = type->is_float() ? IROp::AddF : IROp::AddI;
            break;
        case AssignOp::SubAssign:
            op = type->is_float() ? IROp::SubF : IROp::SubI;
            break;
        case AssignOp::MulAssign:
            op = type->is_float() ? IROp::MulF : IROp::MulI;
            break;
        case AssignOp::DivAssign:
            op = type->is_float() ? IROp::DivF : IROp::DivI;
            break;
        case AssignOp::ModAssign:
            op = IROp::ModI;
            break;
        case AssignOp::BitAndAssign:
            op = IROp::BitAnd;
            break;
        case AssignOp::BitOrAssign:
            op = IROp::BitOr;
            break;
        case AssignOp::BitXorAssign:
            op = IROp::BitXor;
            break;
        case AssignOp::ShlAssign:
            op = IROp::Shl;
            break;
        case AssignOp::ShrAssign:
            op = IROp::Shr;
            break;
        default:
            op = IROp::Copy;
            break;
    }

    return emit_binary(op, old_val, rhs, type);
}

ValueId IRBuilder::gen_assign_local(Expr* expr, ValueId value) {
    AssignExpr& assign_expr = expr->assign;
    StringView name = assign_expr.target->identifier.name;

    // If this is a pointer parameter (out/inout), store through the pointer
    if (m_param_is_ptr.count(name)) {
        ValueId ptr = lookup_local(name);
        Type* type = expr->resolved_type;
        // get_type_slot_count handles structs (struct_info.slot_count) and every other
        // width uniformly. The old inline check omitted list/map/string/weak, under-counting
        // their slots when stored through an out/inout pointer.
        u32 slot_count = get_type_slot_count(type);
        if (slot_count == 0) slot_count = 1;
        return emit_store_ptr(ptr, value, slot_count, type);
    }

    // Auto-destroy old owned value before reassignment
    Type* target_type = assign_expr.target->resolved_type;

    // Ref reassignment (`r = other`): release the old borrow and acquire the new
    // one so the count stays balanced — the variable now borrows a different
    // object. Emitted before define_local so lookup_local still returns the old
    // value. Applies to ref locals and ref params uniformly. A call result
    // already carries a handed-off count, so it's adopted (no inc); any other
    // source is a fresh borrow (inc) — mirrors gen_var_decl.
    if (target_type && target_type->kind == TypeKind::Ref) {
        emit_ref_dec(lookup_local(name));
        if (!is_ref_handoff_source(assign_expr.value)) {
            emit_ref_inc(value);
        }
    }

    if (target_type && target_type->noncopyable()) {
        OwnedLocalInfo* owned_info = find_owned_local(name);
        if (owned_info && !owned_info->is_moved) {
            emit_implicit_destroy(*owned_info);
            owned_info->is_moved = false;  // Reset — new value is now live
        } else if (owned_info && owned_info->is_moved) {
            // Variable was moved but now being reassigned — make it live again
            owned_info->is_moved = false;
        }
    }

    // Wrap uniq/ref → weak conversion for local assignment
    value = maybe_wrap_weak(value, assign_expr.value->resolved_type, assign_expr.target->resolved_type);

    // For copyable struct rvalues that alias source storage, allocate fresh
    // storage and emit a StructCopy — mirrors the gen_var_decl fix. Struct
    // literals and calls already produce fresh storage, so skip them.
    // Only applies to simple `=`; compound ops on structs dispatch through
    // trait methods above, and primitive compound ops don't land here with
    // a struct target.
    bool value_is_fresh = assign_expr.value->kind == AstKind::ExprStructLiteral ||
                          assign_expr.value->kind == AstKind::ExprCall;
    if (assign_expr.op == AssignOp::Assign && target_type && target_type->is_struct()
        && !target_type->noncopyable() && !value_is_fresh) {
        u32 slot_count = target_type->struct_info.slot_count;
        ValueId fresh = emit_stack_alloc(slot_count, target_type);
        emit_struct_copy(fresh, value, slot_count);
        value = fresh;
    }

    // Normal variable assignment - in SSA, we create a new value
    define_local(name, value, expr->resolved_type);

    // If the RHS was a noncopyable temporary (e.g. `uniq T()`), transfer
    // its ownership to the target variable. Without this, the temp's
    // cleanup record at the current scope depth races with the
    // variable's cleanup record at the variable's (outer) scope depth —
    // harmless when register allocation aliases them and tombstoning
    // absorbs the double-delete, but catastrophic inside nested scopes
    // (e.g. a catch body) where the temp's scope pops before the
    // variable's value is observed, leaving the variable pointing at
    // freed memory. Matches the consume_temp_noncopyable(value, true)
    // call in gen_var_decl.
    if (assign_expr.target->resolved_type &&
        assign_expr.target->resolved_type->noncopyable()) {
        consume_temp_noncopyable(value, true);
    }

    // Move semantics: if value is a noncopyable identifier, mark source as moved.
    // Unlike field assignment, we pass nullify_record=false: the target variable now
    // shares the same SSA value/register as the source, so emitting a Nullify on that
    // register would corrupt the target. The SSA null-out of the source still happens.
    if (assign_expr.value->kind == AstKind::ExprIdentifier) {
        Type* value_type = assign_expr.value->resolved_type;
        if (value_type && value_type->noncopyable()) {
            mark_moved_from(assign_expr.value->identifier.name, /*null_ssa=*/true,
                            /*nullify_record=*/false);
        }
    }
    // `y = o.field`: null the moved-out source field in its root.
    if (assign_expr.value->resolved_type && assign_expr.value->resolved_type->noncopyable()) {
        nullify_moved_field_source(assign_expr.value);
    }

    return value;
}

ValueId IRBuilder::gen_assign_field(Expr* expr, ValueId value) {
    AssignExpr& assign_expr = expr->assign;
    GetExpr& get_expr = assign_expr.target->get;
    ValueId obj = gen_expr(get_expr.object);

    // Get the struct type from the object
    Type* obj_type = get_expr.object->resolved_type;
    Type* struct_type = obj_type ? obj_type->base_type() : nullptr;

    u32 slot_offset = 0;
    u32 slot_count = 1;
    Type* field_type = nullptr;
    bool is_variant_field = false;
    const WhenClauseInfo* when_clause = nullptr;
    const VariantInfo* variant = nullptr;

    if (struct_type && struct_type->is_struct()) {
        const FieldInfo* field_info = struct_type->struct_info.find_field(get_expr.name);
        if (field_info) {
            slot_offset = field_info->slot_offset;
            slot_count = field_info->slot_count;
            field_type = field_info->type;
        } else {
            // Check for variant field in when clauses
            const VariantFieldInfo* variant_field_info = struct_type->struct_info.find_variant_field(
                get_expr.name, &when_clause, &variant);
            if (variant_field_info) {
                is_variant_field = true;
                slot_offset = when_clause->union_slot_offset + variant_field_info->slot_offset;
                slot_count = variant_field_info->slot_count;
                field_type = variant_field_info->type;
            }
        }
    }

    // If this is a variant field, emit runtime discriminant check
    if (is_variant_field && when_clause && variant) {
        // Load the discriminant
        ValueId disc = emit_get_field(obj, when_clause->discriminant_name,
                                      when_clause->discriminant_slot_offset, 1,
                                      when_clause->discriminant_type);

        // Create the expected discriminant value constant
        ValueId expected = emit_const_int(variant->discriminant_value,
                                          when_clause->discriminant_type);

        // Compare discriminant with expected value
        ValueId matches = emit_binary(IROp::EqI, disc, expected, m_types.bool_type());

        // Create pass and fail blocks
        IRBlock* pass_block = create_block("variant_set_pass");
        IRBlock* fail_block = create_block("variant_set_fail");

        // Branch based on discriminant check
        finish_block_branch(matches, pass_block->id, fail_block->id);

        // In fail block: emit unreachable (will lower to TRAP)
        set_current_block(fail_block);
        finish_block_unreachable();

        // Continue in pass block
        set_current_block(pass_block);
    }

    // Destroy old field value before overwriting (prevents leaks for uniq/move-semantic fields)
    if (field_type && (field_type->kind == TypeKind::Uniq || field_type->noncopyable())) {
        emit_single_field_destroy(obj, get_expr.name, slot_offset, slot_count, field_type);
    }

    // Wrap uniq/ref → weak conversion for field assignment
    value = maybe_wrap_weak(value, assign_expr.value->resolved_type, field_type);

    // For struct-typed fields the rvalue is a struct pointer (per IR convention),
    // so we must copy slot-by-slot from the source struct. emit_set_field would
    // otherwise stuff the raw pointer bits into the field slots, silently
    // corrupting the field (e.g. losing nested when-discriminants). Mirror the
    // struct-literal initialization path for consistency.
    ValueId result;
    if (field_type && field_type->is_struct()) {
        ValueId field_addr = emit_get_field_addr(obj, get_expr.name, slot_offset, field_type);
        emit_struct_copy(field_addr, value, slot_count);
        result = value;
    } else {
        result = emit_set_field(obj, get_expr.name, slot_offset, slot_count, value, expr->resolved_type);
    }

    // Consume noncopyable temporaries assigned to fields
    if (field_type && field_type->noncopyable()) {
        consume_temp_noncopyable(value);
    }

    // Move semantics: if value is a uniq/move-semantic identifier, mark it as moved
    // Only when the field type also needs move semantics (not for weak ref fields)
    if (assign_expr.value->kind == AstKind::ExprIdentifier && field_type &&
        field_type->noncopyable()) {
        Type* value_type = assign_expr.value->resolved_type;
        if (value_type && value_type->noncopyable()) {
            mark_moved_from(assign_expr.value->identifier.name);
        }
    }

    // Field-move nullify: if the RHS is a field access on a local value-struct
    // (e.g. `self.things = src.items` where `src` is a by-value noncopyable
    // struct param), the semantic analyzer marked the RHS as moved, but nothing
    // has actually cleared the source field — the enclosing struct's destructor
    // would later free it a second time. Null the source field.
    if (field_type && field_type->noncopyable()) {
        nullify_moved_field_source(assign_expr.value);
    }

    return result;
}

ValueId IRBuilder::gen_assign_index(Expr* expr, ValueId value) {
    AssignExpr& assign_expr = expr->assign;
    IndexExpr& index_expr = assign_expr.target->index;

    Type* obj_type = index_expr.object->resolved_type;
    Type* container_type = obj_type ? obj_type->base_type() : nullptr;

    // Struct indexing: dispatch to "index_mut" method
    if (container_type && container_type->is_struct()) {
        StringView method_name("index_mut", 9);
        Type* found_in = nullptr;
        const MethodInfo* method_info = lookup_method_in_hierarchy(container_type, method_name, &found_in);
        if (method_info && found_in) {
            ValueId self_ptr = gen_lvalue_addr(index_expr.object);
            ValueId index_val = gen_expr(index_expr.index);
            StringView mangled = mangle_method(found_in->struct_info.name, method_name);
            Span<ValueId> args = alloc_span<ValueId>(3);
            args[0] = self_ptr;
            args[1] = index_val;
            args[2] = value;
            return emit_call(mangled, args, m_types.void_type());
        }
    }

    // List/Map indexing: emit IndexSet IR op
    if (container_type && container_type->is_container()) {
        bool is_list = container_type->is_list();
        Type* elem_type = is_list ? container_type->list_info.element_type
                                  : container_type->map_info.value_type;
        bool elem_noncopyable = elem_type && elem_type->noncopyable();
        ContainerKind kind = is_list ? ContainerKind::List : ContainerKind::Map;

        ValueId obj = gen_expr(index_expr.object);
        ValueId index_val = gen_expr(index_expr.index);

        // Destroy the overwritten element before storing, so a noncopyable old
        // element isn't leaked (mirrors emit_single_field_destroy for fields).
        // See docs/internals/lifetimes.md §9.
        if (elem_noncopyable && is_list) {
            // List: the index is always in bounds, so the old element always
            // exists — destroy it unconditionally.
            ValueId old = emit_index_get(obj, index_val, kind, elem_type);
            IRInst* del = emit_inst(IROp::Delete, elem_type);
            if (del) del->unary = old;
        } else if (elem_noncopyable) {
            // Map: a slot has an old value only for an already-present key, so
            // guard the destroy with a `contains` check (a new key destroys
            // nothing). Synthesizes: if (map.contains(key)) delete map[key];
            StringView contains_native;
            for (const MethodInfo& method : container_type->map_info.methods) {
                if (method.name == StringView("contains", 8)) {
                    contains_native = method.native_name;
                    break;
                }
            }
            i32 contains_idx = contains_native.empty()
                ? -1 : m_registry.get_index(contains_native);
            if (contains_idx >= 0) {
                Span<ValueId> contains_args = alloc_span<ValueId>(2);
                contains_args[0] = obj;
                contains_args[1] = index_val;
                ValueId present = emit_call_native(contains_native, contains_args,
                                                   m_types.bool_type(),
                                                   static_cast<u8>(contains_idx));
                IRBlock* destroy_block = create_block("map_set_destroy_old");
                IRBlock* merge_block = create_block("map_set_store");
                finish_block_branch(present, destroy_block->id, merge_block->id);

                set_current_block(destroy_block);
                ValueId old = emit_index_get(obj, index_val, kind, elem_type);
                IRInst* del = emit_inst(IROp::Delete, elem_type);
                if (del) del->unary = old;
                finish_block_goto(merge_block->id);

                set_current_block(merge_block);
            }
        }

        emit_index_set(obj, index_val, value, kind);

        // Consume the moved-in value so it isn't double-owned (container slot +
        // caller scope). consume_temp_noncopyable handles temporaries;
        // mark_moved_from handles a named noncopyable source.
        if (elem_noncopyable) {
            consume_temp_noncopyable(value);
            if (assign_expr.value->kind == AstKind::ExprIdentifier) {
                mark_moved_from(assign_expr.value->identifier.name);
            }
        }
        return value;
    }

    return value;
}

ValueId IRBuilder::gen_grouping_expr(Expr* expr) {
    return gen_expr(expr->grouping.expr);
}

ValueId IRBuilder::gen_this_expr(Expr* expr) {
    // 'self' is the first parameter in methods
    return lookup_local("self");
}

// Narrow an i64 value to `bits` bits, sign- or zero-extending as appropriate.
// Mirrors the runtime's TRUNC_S / TRUNC_U bytecode behavior.
static i64 narrow_int_to_width(i64 v, u8 bits, bool is_signed) {
    switch (bits) {
        case 8:  return is_signed ? static_cast<i64>(static_cast<i8>(v))  : static_cast<i64>(static_cast<u8>(v));
        case 16: return is_signed ? static_cast<i64>(static_cast<i16>(v)) : static_cast<i64>(static_cast<u16>(v));
        case 32: return is_signed ? static_cast<i64>(static_cast<i32>(v)) : static_cast<i64>(static_cast<u32>(v));
        default: return v;
    }
}

static u8 type_int_bits(TypeKind k) {
    switch (k) {
        case TypeKind::I8:  case TypeKind::U8:  return 8;
        case TypeKind::I16: case TypeKind::U16: return 16;
        case TypeKind::I32: case TypeKind::U32: return 32;
        case TypeKind::I64: case TypeKind::U64: return 64;
        default: return 64;
    }
}

ValueId IRBuilder::try_fold_cast(ValueId source, Type* source_type, Type* target_type) {
    if (!m_current_func || !source_type || !target_type) return ValueId::invalid();
    IRInst* o = m_current_func->inst_for(source);
    if (!o) return ValueId::invalid();

    bool src_int  = (o->op == IROp::ConstInt);
    bool src_bool = (o->op == IROp::ConstBool);
    bool src_f32  = (o->op == IROp::ConstF);
    bool src_f64  = (o->op == IROp::ConstD);
    if (!src_int && !src_bool && !src_f32 && !src_f64) return ValueId::invalid();

    // Target = bool: nonzero/true
    if (target_type->kind == TypeKind::Bool) {
        bool result = src_int  ? (o->const_data.int_val != 0)
                    : src_bool ? o->const_data.bool_val
                    : src_f32  ? (o->const_data.f32_val != 0.0f)
                               : (o->const_data.f64_val != 0.0);
        return emit_const_bool(result);
    }

    // Target = integer
    if (target_type->is_integer()) {
        i64 v = src_int  ? o->const_data.int_val
              : src_bool ? (o->const_data.bool_val ? 1 : 0)
              : src_f32  ? static_cast<i64>(o->const_data.f32_val)
                         : static_cast<i64>(o->const_data.f64_val);
        v = narrow_int_to_width(v, type_int_bits(target_type->kind), target_type->is_signed_integer());
        return emit_const_int(v, target_type);
    }

    // Target = f32 / f64
    if (target_type->is_float()) {
        f64 v;
        if (src_int) {
            v = source_type->is_unsigned_integer()
                ? static_cast<f64>(static_cast<u64>(o->const_data.int_val))
                : static_cast<f64>(o->const_data.int_val);
        } else if (src_bool) {
            v = o->const_data.bool_val ? 1.0 : 0.0;
        } else if (src_f32) {
            v = static_cast<f64>(o->const_data.f32_val);
        } else {
            v = o->const_data.f64_val;
        }
        return emit_const_float(v, target_type);
    }

    return ValueId::invalid();
}

ValueId IRBuilder::gen_primitive_cast(Expr* expr) {
    CallExpr& call_expr = expr->call;

    // Get target type from callee (set by semantic analysis)
    Type* target_type = call_expr.callee->resolved_type;

    // Get source value and type
    ValueId source = gen_expr(call_expr.arguments[0].expr);
    Type* source_type = call_expr.arguments[0].expr->resolved_type;

    // If same type, no-op
    if (source_type == target_type) {
        return source;
    }

    if (ValueId folded = try_fold_cast(source, source_type, target_type); folded.is_valid()) {
        return folded;
    }

    // Emit Cast instruction
    IRInst* inst = emit_inst(IROp::Cast, target_type);
    if (inst) {
        inst->cast.source = source;
        inst->cast.source_type = source_type;
        return inst->result;
    }
    return ValueId::invalid();
}

ValueId IRBuilder::gen_constructor_call(Expr* expr) {
    CallExpr& call_expr = expr->call;
    Type* result_type = expr->resolved_type;

    // Get struct type from callee (set by semantic analysis)
    // For Type(), callee is an identifier with struct type
    // For Type.ctor_name(), callee is a GetExpr with object having struct type
    Type* struct_type = nullptr;
    if (call_expr.callee->kind == AstKind::ExprIdentifier) {
        struct_type = call_expr.callee->resolved_type;
    } else if (call_expr.callee->kind == AstKind::ExprGet) {
        struct_type = call_expr.callee->get.object->resolved_type;
    }

    // Determine allocation mode and final struct type
    ValueId obj;
    if (call_expr.is_heap) {
        // uniq Type() - heap allocation
        // result_type is uniq<StructType>
        Span<ValueId> empty_args = {};
        obj = emit_new(struct_type->struct_info.name, empty_args, result_type);
        // Track temporary for exception cleanup via m_owned_locals.
        // Consumed when passed/assigned/returned (marked is_moved + Nullify).
        if (result_type && result_type->noncopyable() && m_current_block) {
            StringView temp_name = intern_format("__tmp{}", m_next_temp_id++);

            define_local(temp_name, obj, result_type);
            u32 scope_depth = static_cast<u32>(m_local_scopes.size());
            m_owned_locals.push_back({temp_name, result_type, scope_depth, false, true,
                                      m_current_block->id, obj});
        }
    } else {
        // Type() - stack allocation
        // result_type is StructType (value type)
        u32 slot_count = struct_type->struct_info.slot_count;
        obj = emit_stack_alloc(slot_count, struct_type);
    }

    // Call constructor (user-defined or synthesized)
    StructTypeInfo& struct_type_info = struct_type->struct_info;
    StringView ctor_name = mangle_constructor(struct_type_info.name, call_expr.constructor_name);

    // Build arguments: 'self' pointer + constructor arguments.
    //
    // For noncopyable args, ownership transfers to the callee. gen_call_expr
    // handles this with:
    //   (a) `consume_temp_noncopyable` right after evaluation — nullifies the
    //       temp's cleanup register so its caller-side Delete becomes a no-op.
    //   (b) post-call nullify-replace for identifier args — replaces the
    //       local's SSA binding with null and marks is_moved=true so scope
    //       cleanup skips it.
    //
    // Constructor calls need the same fixup; without it `uniq T.name(arg)`
    // where arg is a uniq-typed local (or a uniq rvalue temporary) leaves
    // the caller still pointing at the slot the callee now owns — when the
    // constructor stores arg into a field and the enclosing struct is
    // destroyed, the field cleanup frees the slot AND the caller's scope-exit
    // Delete frees it a second time (slab_allocator.cpp:286 ALIVE assert).
    Vector<ValueId> call_args;
    call_args.push_back(obj);  // 'self' pointer

    for (auto& arg : call_expr.arguments) {
        ValueId arg_val;
        if (arg.modifier != ParamModifier::None) {
            // Pass address of lvalue for out/inout args
            arg_val = gen_lvalue_addr(arg.expr);
        } else {
            arg_val = gen_expr(arg.expr);
            if (arg.expr->resolved_type && arg.expr->resolved_type->noncopyable()) {
                consume_temp_noncopyable(arg_val);
            }
        }
        call_args.push_back(arg_val);
    }

    emit_call(ctor_name, alloc_span(call_args), m_types.void_type());

    // Post-call nullify-replace for noncopyable identifier arguments.
    for (u32 i = 0; i < call_expr.arguments.size(); i++) {
        const CallArg& arg = call_expr.arguments[i];
        if (arg.modifier != ParamModifier::None) continue;
        if (arg.expr->kind != AstKind::ExprIdentifier) continue;
        Type* arg_type = arg.expr->resolved_type;
        if (!arg_type || !arg_type->noncopyable()) continue;
        mark_moved_from(arg.expr->identifier.name);
    }

    return obj;
}

ValueId IRBuilder::gen_super_call(Expr* expr) {
    CallExpr& call_expr = expr->call;
    SuperExpr& super_expr = call_expr.callee->super_expr;

    // Get the 'self' pointer
    ValueId self_ptr = lookup_local("self");

    // Get the parent struct type from the callee's resolved_type (ref<StructType>)
    Type* ref_type = call_expr.callee->resolved_type;
    Type* target_type = nullptr;
    if (ref_type && ref_type->is_reference()) {
        target_type = ref_type->ref_info.inner_type;
    }

    if (!target_type || !target_type->is_struct()) {
        report_error("Internal error: invalid super call target");
        return ValueId::invalid();
    }

    // Determine if this is a constructor call or method call
    // Constructor calls return void (expr->resolved_type is void)
    // Method calls return the method's return type
    bool is_constructor_call = expr->resolved_type && expr->resolved_type->kind == TypeKind::Void;

    StructTypeInfo& target_struct_type_info = target_type->struct_info;

    StringView call_name;
    if (is_constructor_call) {
        call_name = mangle_constructor(target_struct_type_info.name, super_expr.method_name);
    } else {
        call_name = mangle_method(target_struct_type_info.name, super_expr.method_name);
    }

    // Evaluate arguments
    Span<ValueId> args = alloc_span<ValueId>(call_expr.arguments.size());
    for (u32 i = 0; i < call_expr.arguments.size(); i++) {
        CallArg& arg = call_expr.arguments[i];
        if (arg.modifier != ParamModifier::None) {
            args[i] = gen_lvalue_addr(arg.expr);
        } else {
            args[i] = gen_expr(arg.expr);
        }
    }

    // Prepend self to arguments
    Span<ValueId> call_args = alloc_span<ValueId>(args.size() + 1);
    call_args[0] = self_ptr;
    for (u32 i = 0; i < args.size(); i++) {
        call_args[i + 1] = args[i];
    }

    // Check if the super method is a native function
    i32 native_idx = m_registry.get_index(call_name);
    if (native_idx >= 0) {
        return emit_call_native(call_name, call_args, expr->resolved_type,
                                static_cast<u8>(native_idx));
    }
    return emit_call(call_name, call_args, expr->resolved_type);
}

ValueId IRBuilder::gen_struct_literal_expr(Expr* expr) {
    StructLiteralExpr& sl = expr->struct_literal;
    Type* result_type = expr->resolved_type;

    // Determine struct type and allocation mode
    Type* struct_type;
    ValueId struct_ptr;

    if (sl.is_heap) {
        // uniq Type { ... } - heap allocation
        struct_type = result_type->ref_info.inner_type;
        Span<ValueId> empty_args = {};
        // Use mangled name for generic struct instances (e.g., "Box$i32")
        StringView type_name = sl.mangled_name.size() > 0 ? sl.mangled_name : sl.type_name;
        struct_ptr = emit_new(type_name, empty_args, result_type);
        // Track temporary for exception cleanup via m_owned_locals
        if (result_type && result_type->noncopyable() && m_current_block) {
            StringView temp_name = intern_format("__tmp{}", m_next_temp_id++);

            define_local(temp_name, struct_ptr, result_type);
            u32 scope_depth = static_cast<u32>(m_local_scopes.size());
            m_owned_locals.push_back({temp_name, result_type, scope_depth, false, true,
                                      m_current_block->id, struct_ptr});
        }
    } else {
        // Type { ... } - stack allocation
        struct_type = result_type;
        u32 slot_count = struct_type->struct_info.slot_count;
        struct_ptr = emit_stack_alloc(slot_count, struct_type);
    }

    // Build map of provided field initializers
    tsl::robin_map<StringView, Expr*> provided_fields;
    for (auto& field : sl.fields) {
        provided_fields[field.name] = field.value;
    }

    // Helper to find default value for a field by searching the inheritance chain
    auto find_field_default = [](Type* type, StringView field_name) -> Expr* {
        Type* current = type;
        while (current && current->is_struct()) {
            if (!current->struct_info.decl) {
                current = current->struct_info.parent;
                continue;
            }
            StructDecl& struct_decl = current->struct_info.decl->struct_decl;
            for (auto& field : struct_decl.fields) {
                if (field.name == field_name) {
                    return field.default_value;
                }
            }
            current = current->struct_info.parent;
        }
        return nullptr;
    };

    // Initialize regular fields (including discriminants which are in struct_info.fields)
    for (auto& field_info : struct_type->struct_info.fields) {
        Expr* value_expr = nullptr;

        auto it = provided_fields.find(field_info.name);
        if (it != provided_fields.end()) {
            value_expr = it->second;
        } else {
            // Use default value from struct declaration (searching inheritance chain)
            value_expr = find_field_default(struct_type, field_info.name);
        }

        ValueId value = gen_expr(value_expr);

        // Wrap uniq/ref → weak conversion for struct literal field
        if (value_expr) {
            value = maybe_wrap_weak(value, value_expr->resolved_type, field_info.type);
        }

        // For struct-typed fields, use StructCopy since the value is a pointer
        if (field_info.type && field_info.type->is_struct()) {
            // Get address of the field
            ValueId field_addr = emit_get_field_addr(struct_ptr, field_info.name, field_info.slot_offset, field_info.type);
            // Copy struct data from value (source pointer) to field_addr (dest pointer)
            emit_struct_copy(field_addr, value, field_info.slot_count);
        } else {
            emit_set_field(struct_ptr, field_info.name, field_info.slot_offset, field_info.slot_count, value, field_info.type);
        }

        // Consume noncopyable temporaries moved into struct fields
        if (field_info.type && field_info.type->noncopyable() && value_expr) {
            consume_temp_noncopyable(value);
        }

        // Nullify source variable when moving a noncopyable value into a regular field
        if (field_info.type && field_info.type->noncopyable() && value_expr) {
            if (value_expr->kind == AstKind::ExprIdentifier) {
                mark_moved_from(value_expr->identifier.name);
            }
            // `Foo { x = o.field }`: null the moved-out source field in its root.
            nullify_moved_field_source(value_expr);
        }
    }

    // Initialize variant fields from when clauses
    for (const auto& clause : struct_type->struct_info.when_clauses) {

        // For each variant in the clause
        for (const auto& variant : clause.variants) {

            // Initialize variant fields if they're provided
            for (const auto& variant_field_info : variant.fields) {

                auto it = provided_fields.find(variant_field_info.name);
                if (it != provided_fields.end()) {
                    Expr* value_expr = it->second;
                    ValueId value = gen_expr(value_expr);

                    // Compute the actual offset: union_slot_offset + variant field's offset
                    u32 actual_slot_offset = clause.union_slot_offset + variant_field_info.slot_offset;

                    // For struct-typed variant fields, use StructCopy
                    if (variant_field_info.type && variant_field_info.type->is_struct()) {
                        ValueId field_addr = emit_get_field_addr(struct_ptr, variant_field_info.name, actual_slot_offset, variant_field_info.type);
                        emit_struct_copy(field_addr, value, variant_field_info.slot_count);
                    } else {
                        emit_set_field(struct_ptr, variant_field_info.name, actual_slot_offset, variant_field_info.slot_count, value, variant_field_info.type);
                    }

                    // Consume noncopyable temporaries moved into variant fields
                    if (variant_field_info.type && variant_field_info.type->noncopyable()) {
                        consume_temp_noncopyable(value);
                    }

                    // Nullify source variable when moving a noncopyable value into a variant field
                    if (variant_field_info.type && variant_field_info.type->noncopyable()) {
                        if (value_expr->kind == AstKind::ExprIdentifier) {
                            mark_moved_from(value_expr->identifier.name);
                        }
                        nullify_moved_field_source(value_expr);
                    }
                }
            }
        }
    }

    return struct_ptr;
}

ValueId IRBuilder::gen_static_get_expr(Expr* expr) {
    StaticGetExpr& sge = expr->static_get;

    // Currently only enum variants use static get (Type::Variant)
    // Look up the enum variant symbol to get its value
    Symbol* sym = m_symbols.lookup(sge.member_name);
    if (sym && sym->kind == SymbolKind::EnumVariant) {
        // Emit the enum variant's integer value
        return emit_const_int(sym->enum_variant.value, expr->resolved_type);
    }

    // Should not reach here if semantic analysis passed
    report_error("Internal error: unexpected state in static get expression");
    return ValueId::invalid();
}

ValueId IRBuilder::gen_string_interp_expr(Expr* expr) {
    auto& string_interp = expr->string_interp;
    Type* string_type = m_types.string_type();

    // Build a flat list of string-valued ValueIds
    Vector<ValueId> string_parts;

    for (u32 i = 0; i < string_interp.parts.size(); i++) {
        // Add text part if non-empty
        if (string_interp.parts[i].size() > 0) {
            string_parts.push_back(emit_const_string(string_interp.parts[i]));
        }

        // Add expression part (if there is one — there are N expressions for N+1 parts)
        if (i < string_interp.expressions.size()) {
            Expr* sub = string_interp.expressions[i];
            Type* etype = sub->resolved_type;
            ValueId val = gen_expr(sub);

            if (etype->kind == TypeKind::String) {
                // String expression — use directly, no conversion needed
                string_parts.push_back(val);
            } else {
                // Need to call to_string native for this type
                const char* native_name = nullptr;
                switch (etype->kind) {
                    case TypeKind::Bool:   native_name = "bool$$to_string"; break;
                    case TypeKind::I32:    native_name = "i32$$to_string"; break;
                    case TypeKind::I64:    native_name = "i64$$to_string"; break;
                    case TypeKind::F32:    native_name = "f32$$to_string"; break;
                    case TypeKind::F64:    native_name = "f64$$to_string"; break;
                    default: break;
                }

                if (etype->is_enum()) {
                    // Enums use i32$$to_string on their underlying value
                    native_name = "i32$$to_string";
                }

                if (native_name) {
                    StringView name(native_name, static_cast<u32>(strlen(native_name)));
                    i32 native_idx = m_registry.get_index(name);
                    if (native_idx >= 0) {
                        Span<ValueId> args = alloc_span<ValueId>(1);
                        args[0] = val;
                        string_parts.push_back(
                            emit_call_native(name, args, string_type, static_cast<u8>(native_idx)));
                    }
                } else if (etype->is_struct()) {
                    // Struct with to_string method: call the mangled method.
                    // gen_expr already returns a struct pointer for struct rvalues — the
                    // lowering pass unpacks struct returns into stack-allocated pointers
                    // (see the note in gen_var_decl). Reusing `val` here avoids a second
                    // pass through gen_lvalue_addr, which doesn't accept call/index/method
                    // rvalues and would error with "expression is not a valid lvalue".
                    StringView mangled = mangle_method(etype->struct_info.name,
                                                       StringView("to_string", 9));
                    Span<ValueId> args = alloc_span<ValueId>(1);
                    args[0] = val;

                    i32 native_idx = m_registry.get_index(mangled);
                    if (native_idx >= 0) {
                        string_parts.push_back(
                            emit_call_native(mangled, args, string_type, static_cast<u8>(native_idx)));
                    } else {
                        string_parts.push_back(
                            emit_call(mangled, args, string_type));
                    }
                }
            }
        }
    }

    // Edge case: empty f-string or no parts
    if (string_parts.empty()) {
        return emit_const_string(StringView("", 0));
    }

    // Left-fold concatenation with str_concat
    ValueId result = string_parts[0];
    StringView concat_name("str_concat", 10);
    i32 concat_idx = m_registry.get_index(concat_name);

    for (u32 i = 1; i < string_parts.size(); i++) {
        Span<ValueId> args = alloc_span<ValueId>(2);
        args[0] = result;
        args[1] = string_parts[i];
        result = emit_call_native(concat_name, args, string_type, static_cast<u8>(concat_idx));
    }

    return result;
}

ValueId IRBuilder::gen_lvalue_addr(Expr* expr) {
    if (!expr) return ValueId::invalid();

    switch (expr->kind) {
        case AstKind::ExprIdentifier: {
            StringView name = expr->identifier.name;
            // If this is already a pointer parameter, return its value directly
            if (m_param_is_ptr.count(name)) {
                return lookup_local(name);
            }

            ValueId current_val = lookup_local(name);
            Type* type = expr->resolved_type;

            // For struct types, the variable is already a pointer to stack-allocated data.
            // Just return the existing pointer - no copy needed.
            if (type && type->is_struct()) {
                return current_val;
            }

            // For primitive types and list pointers, we need to:
            // 1. Allocate a stack slot
            // 2. Store the current value to the slot
            // 3. Return the address

            // Calculate slot count. Structs returned above; get_type_slot_count covers every
            // remaining width (weak=4, uniq/ref/list/map/string/fn=2). 0 => opaque, default 1.
            u32 slot_count = get_type_slot_count(type);
            if (slot_count == 0) slot_count = 1;

            // Allocate stack space
            ValueId addr = emit_stack_alloc(slot_count, type);

            // Store current value to the stack slot (using SetField with offset 0)
            emit_set_field(addr, name, 0, slot_count, current_val, type);

            return addr;
        }
        case AstKind::ExprGet: {
            // Field access - use GET_FIELD_ADDR
            GetExpr& get_expr = expr->get;
            ValueId obj = gen_expr(get_expr.object);

            // Get the struct type from the object
            Type* obj_type = get_expr.object->resolved_type;
            Type* struct_type = obj_type ? obj_type->base_type() : nullptr;

            u32 slot_offset = 0;
            if (struct_type && struct_type->is_struct()) {
                const FieldInfo* field_info = struct_type->struct_info.find_field(get_expr.name);
                if (field_info) {
                    slot_offset = field_info->slot_offset;
                }
            }

            return emit_get_field_addr(obj, get_expr.name, slot_offset, expr->resolved_type);
        }
        case AstKind::ExprGrouping:
            return gen_lvalue_addr(expr->grouping.expr);
        default:
            // Should not happen - semantic analysis validated lvalues
            report_error("Internal error: expression is not a valid lvalue");
            return ValueId::invalid();
    }
}

// Declaration generation

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
            if (!type->noncopyable()) {
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
            value = maybe_wrap_weak(value, var_decl.initializer->resolved_type, type);
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
        m_owned_locals.push_back({var_decl.name, type, scope_depth, false, false, current_block_id, value});

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
            emit_ref_inc(value);
        }
        u32 scope_depth = static_cast<u32>(m_local_scopes.size());
        BlockId current_block_id = m_current_block ? m_current_block->id : BlockId::invalid();
        m_owned_locals.push_back({var_decl.name, type, scope_depth, false, false,
                                  current_block_id, value, OwnedKind::RefBorrow});
    }
}

// Variable management

void IRBuilder::define_local(StringView name, ValueId value, Type* type) {
    if (m_local_scopes.empty()) return;

    // Search for an existing binding in outer scopes and update it
    // This is necessary for SSA - assignments should update the existing definition
    for (i32 i = static_cast<i32>(m_local_scopes.size()) - 1; i >= 0; i--) {
        auto it = m_local_scopes[i].find(name);
        if (it != m_local_scopes[i].end()) {
            m_local_scopes[i][name] = {value, type};
            return;
        }
    }

    // If no existing binding, add to innermost scope (new variable declaration)
    m_local_scopes.back()[name] = {value, type};
}

ValueId IRBuilder::lookup_local(StringView name) {
    // Search from innermost to outermost scope
    for (i32 i = static_cast<i32>(m_local_scopes.size()) - 1; i >= 0; i--) {
        auto it = m_local_scopes[i].find(name);
        if (it != m_local_scopes[i].end()) {
            return it->second.value;
        }
    }
    report_error("Internal error: undefined variable in IR generation");
    return ValueId::invalid();
}

void IRBuilder::push_scope() {
    m_local_scopes.push_back({});
}

void IRBuilder::pop_scope() {
    if (m_local_scopes.empty()) return;
    u32 depth = static_cast<u32>(m_local_scopes.size());

    // Record cleanup info for exception-path cleanup BEFORE emit_scope_cleanup.
    // Uses initial_value (the SSA value at declaration time) so lowering can map it
    // to the correct register. The null-check in the VM ensures that if the variable
    // was already cleaned up (register is 0), it's safely skipped.
    // Records are pushed in declaration order (forward); the VM's execute_cleanup
    // iterates in reverse to achieve LIFO cleanup order.
    {
        BlockId end_block = m_current_block ? m_current_block->id : BlockId::invalid();
        if (!m_current_block && !m_current_func->blocks.empty()) {
            end_block = m_current_func->blocks.back()->id;
        }
        // Find the first owned local in this scope
        u32 first_in_scope = 0;
        for (u32 i = 0; i < m_owned_locals.size(); i++) {
            if (m_owned_locals[i].scope_depth >= depth) {
                first_in_scope = i;
                break;
            }
            if (i == m_owned_locals.size() - 1) {
                first_in_scope = static_cast<u32>(m_owned_locals.size()); // none found
            }
        }
        for (u32 i = first_in_scope; i < m_owned_locals.size(); i++) {
            auto& info = m_owned_locals[i];
            if (info.scope_depth < depth) continue;
            if (info.start_block.is_valid() && end_block.is_valid() && info.initial_value.is_valid()) {
                IRCleanupKind kind = info.kind == OwnedKind::RefBorrow
                    ? IRCleanupKind::RefDec : IRCleanupKind::Delete;
                m_current_func->cleanup_info.push_back(
                    {info.initial_value, info.type, info.start_block, end_block, kind});
            }
        }
    }

    // Emit cleanup for live owned locals in this scope
    emit_scope_cleanup(depth);

    // Remove owned local tracking for this scope
    while (!m_owned_locals.empty() && m_owned_locals.back().scope_depth >= depth) {
        m_owned_locals.pop_back();
    }

    m_local_scopes.pop_back();
}

IRBuilder::LocalVar* IRBuilder::find_local(StringView name) {
    // Search from innermost to outermost scope
    for (i32 i = static_cast<i32>(m_local_scopes.size()) - 1; i >= 0; i--) {
        auto it = m_local_scopes[i].find(name);
        if (it != m_local_scopes[i].end()) {
            return &it.value();
        }
    }
    return nullptr;
}

IRBuilder::OwnedLocalInfo* IRBuilder::find_owned_local(StringView name) {
    for (auto& info : m_owned_locals) {
        if (info.name == name) {
            return &info;
        }
    }
    return nullptr;
}

void IRBuilder::consume_temp_noncopyable(ValueId val, bool adopted_by_variable) {
    // Find the temporary in m_owned_locals by ValueId (temporaries have __tmp names).
    // Only matches temporaries, not named variables that happen to share the same ValueId.
    for (i32 i = static_cast<i32>(m_owned_locals.size()) - 1; i >= 0; i--) {
        auto& info = m_owned_locals[i];
        if (info.initial_value.id == val.id && !info.is_moved && info.is_temporary) {
            info.is_moved = true;
            // When adopted by a variable (same register), the variable's cleanup record
            // handles destruction — no Nullify needed. Otherwise, emit a Nullify annotation
            // so the bytecode builder ends the cleanup record scope at this point.
            if (!adopted_by_variable) {
                IRInst* nullify = emit_inst(IROp::Nullify, m_types.void_type());
                if (nullify) nullify->unary = val;
            }
            // Update the local mapping to null so yield/block-arg captures see null
            // instead of the stale pointer (prevents double-free in coroutines).
            ValueId null_val = emit_const_null();
            define_local(info.name, null_val, info.type);
            return;
        }
    }
    // Not found — val is not a tracked temporary (e.g., named variable, or copyable type)
}

void IRBuilder::mark_moved_from(StringView name, bool null_ssa, bool nullify_record) {
    OwnedLocalInfo* owned_info = find_owned_local(name);
    if (!owned_info || owned_info->is_moved) return;

    // For uniq sources, re-point the SSA name at null so future reads (and the
    // scope-exit Delete) see null instead of the moved-out pointer. Value-struct
    // sources keep their register — the bitwise copy already transferred them.
    if (null_ssa && owned_info->type && owned_info->type->kind == TypeKind::Uniq) {
        ValueId null_val = emit_const_null();
        define_local(name, null_val, owned_info->type);
    }

    // Zero the cleanup record's register so exception-path cleanup skips it.
    if (nullify_record && owned_info->initial_value.is_valid()) {
        IRInst* nullify = emit_inst(IROp::Nullify, m_types.void_type());
        if (nullify) nullify->unary = owned_info->initial_value;
    }

    owned_info->is_moved = true;
}

void IRBuilder::nullify_moved_field_source(Expr* consumed) {
    if (!consumed || consumed->kind != AstKind::ExprGet) return;
    Type* field_type = consumed->resolved_type;
    if (!field_type || !field_type->noncopyable()) return;

    GetExpr& src_get = consumed->get;
    Type* src_obj_type = src_get.object->resolved_type;
    Type* src_struct_type = src_obj_type ? src_obj_type->base_type() : nullptr;
    if (!src_struct_type || !src_struct_type->is_struct()) return;
    const FieldInfo* src_field = src_struct_type->struct_info.find_field(src_get.name);
    if (!src_field) return;

    ValueId src_obj_ptr = gen_expr(src_get.object);
    ValueId null_val = emit_const_null();
    emit_set_field(src_obj_ptr, src_field->name, src_field->slot_offset,
                   src_field->slot_count, null_val, m_types.void_type());
}

void IRBuilder::emit_implicit_destroy(OwnedLocalInfo& info) {
    if (info.is_moved) return;
    if (!m_current_block) return;  // Block already terminated

    ValueId current_value = lookup_local(info.name);

    // Ref borrow: decrement its count rather than destroy the pointee. The
    // owner is freed elsewhere; this just releases this binding's borrow.
    if (info.kind == OwnedKind::RefBorrow) {
        emit_ref_dec(current_value);
        // Narrow the exception cleanup record to end at this RefDec so the
        // unwind path doesn't double-decrement after the normal-path RefDec
        // (mirrors the owned-local Nullify below).
        if (info.initial_value.is_valid()) {
            IRInst* nullify = emit_inst(IROp::Nullify, m_types.void_type());
            if (nullify) nullify->unary = info.initial_value;
        }
        info.is_moved = true;
        return;
    }

    // Emit a single typed Delete — the runtime handles null checks,
    // destructor calls, container element iteration, and freeing.
    IRInst* inst = emit_inst(IROp::Delete, info.type);
    if (inst) {
        inst->unary = current_value;
    }

    // Null-ify heap-allocated values to prevent double-cleanup from exception handler.
    // Use a Nullify annotation (not a runtime ConstNull) so the bytecode builder
    // narrows the cleanup record scope instead of zeroing the register.
    if (info.type->kind == TypeKind::Uniq || info.type->is_list()
        || info.type->is_map() || info.type->is_coroutine()) {
        if (info.initial_value.is_valid()) {
            IRInst* nullify = emit_inst(IROp::Nullify, m_types.void_type());
            if (nullify) nullify->unary = info.initial_value;
        }
    }

    info.is_moved = true;  // Prevent double-destroy
}

void IRBuilder::emit_single_field_destroy(ValueId obj_ptr, StringView field_name,
                                          u32 slot_offset, u32 slot_count, Type* field_type) {
    // For struct fields stored as addresses (value-type structs), use GetFieldAddr
    if (field_type->is_struct() && field_type->noncopyable()) {
        ValueId field_addr = emit_get_field_addr(obj_ptr, field_name,
            slot_offset, field_type);
        IRInst* inst = emit_inst(IROp::Delete, field_type);
        if (inst) inst->unary = field_addr;
        return;
    }

    // For pointer-valued fields (uniq, list, map): load the pointer and emit typed Delete
    ValueId field_val = emit_get_field(obj_ptr, field_name,
        slot_offset, slot_count, field_type);
    IRInst* inst = emit_inst(IROp::Delete, field_type);
    if (inst) inst->unary = field_val;
}

void IRBuilder::emit_field_cleanup(ValueId self_ptr, Type* struct_type) {
    StructTypeInfo& struct_info = struct_type->struct_info;
    // Process regular fields in reverse order (LIFO, like C++ member destruction)
    for (i32 i = static_cast<i32>(struct_info.fields.size()) - 1; i >= 0; i--) {
        const FieldInfo& field = struct_info.fields[i];
        if (!field.type) continue;

        if (field.type->kind == TypeKind::Uniq || field.type->noncopyable()) {
            emit_single_field_destroy(self_ptr, field.name,
                field.slot_offset, field.slot_count, field.type);
        }
    }

    // Process variant fields in when clauses (discriminant-aware cleanup)
    for (const auto& clause : struct_info.when_clauses) {
        // Check if any variant in this clause has noncopyable fields
        bool has_noncopyable_variant = false;
        for (const auto& variant : clause.variants) {
            for (const auto& variant_field : variant.fields) {
                if (variant_field.type && (variant_field.type->kind == TypeKind::Uniq ||
                                           variant_field.type->noncopyable())) {
                    has_noncopyable_variant = true;
                    break;
                }
            }
            if (has_noncopyable_variant) break;
        }
        if (!has_noncopyable_variant) continue;

        // Load the discriminant value
        const FieldInfo* disc_field = nullptr;
        for (const auto& field : struct_info.fields) {
            if (field.name == clause.discriminant_name) {
                disc_field = &field;
                break;
            }
        }
        if (!disc_field) continue;

        ValueId disc_val = emit_get_field(self_ptr, disc_field->name,
            disc_field->slot_offset, disc_field->slot_count, disc_field->type);

        // Create a merge block for after all variant cleanup
        IRBlock* merge_block = create_block("variant_cleanup_done");

        // For each variant, emit: if disc == variant_value, clean up that variant's fields
        for (u32 vi = 0; vi < clause.variants.size(); vi++) {
            const auto& variant = clause.variants[vi];

            // Check if this variant has any noncopyable fields
            bool variant_has_cleanup = false;
            for (const auto& variant_field : variant.fields) {
                if (variant_field.type && (variant_field.type->kind == TypeKind::Uniq ||
                                           variant_field.type->noncopyable())) {
                    variant_has_cleanup = true;
                    break;
                }
            }
            if (!variant_has_cleanup) continue;

            // Compare discriminant to this variant's value
            ValueId variant_val = emit_const_int(
                static_cast<i32>(variant.discriminant_value), disc_field->type);
            ValueId is_match = emit_binary(IROp::EqI, disc_val, variant_val, m_types.bool_type());

            IRBlock* cleanup_block = create_block("variant_cleanup");
            IRBlock* next_block = create_block("variant_next");

            finish_block_branch(is_match, cleanup_block->id, next_block->id);

            // Emit cleanup for this variant's noncopyable fields
            set_current_block(cleanup_block);
            for (i32 fi = static_cast<i32>(variant.fields.size()) - 1; fi >= 0; fi--) {
                const auto& variant_field = variant.fields[fi];
                if (!variant_field.type) continue;
                if (variant_field.type->kind == TypeKind::Uniq ||
                    variant_field.type->noncopyable()) {
                    u32 actual_offset = clause.union_slot_offset + variant_field.slot_offset;
                    emit_single_field_destroy(self_ptr, variant_field.name,
                        actual_offset, variant_field.slot_count, variant_field.type);
                }
            }
            finish_block_goto(merge_block->id);

            set_current_block(next_block);
        }

        // Fall through to merge block from last next_block
        finish_block_goto(merge_block->id);
        set_current_block(merge_block);
    }
}

// emit_element_destroy, emit_list_cleanup, and emit_map_cleanup have been
// removed — all container/element cleanup is now handled by the typed
// IROp::Delete instruction which lowers to the DELETE bytecode opcode.

void IRBuilder::emit_scope_cleanup(u32 min_scope_depth) {
    if (!m_current_block) return;  // Block already terminated

    // LIFO order (reverse declaration order, like C++ destructors)
    for (i32 i = static_cast<i32>(m_owned_locals.size()) - 1; i >= 0; i--) {
        auto& info = m_owned_locals[i];
        if (info.scope_depth >= min_scope_depth && !info.is_moved) {
            emit_implicit_destroy(info);
        }
    }
}

void IRBuilder::collect_assigned_vars(Stmt* stmt, Vector<StringView>& out) {
    if (!stmt) return;

    switch (stmt->kind) {
        case AstKind::StmtExpr:
            collect_assigned_vars_expr(stmt->expr_stmt.expr, out);
            break;
        case AstKind::StmtBlock: {
            BlockStmt& block = stmt->block;
            for (auto* d : block.declarations) {
                if (!d) continue;
                // Recurse into statements (not var decls - those are new vars)
                if (d->kind >= AstKind::StmtExpr && d->kind <= AstKind::StmtYield) {
                    collect_assigned_vars(&d->stmt, out);
                }
            }
            break;
        }
        case AstKind::StmtIf:
            collect_assigned_vars(stmt->if_stmt.then_branch, out);
            collect_assigned_vars(stmt->if_stmt.else_branch, out);
            break;
        case AstKind::StmtWhile:
            collect_assigned_vars(stmt->while_stmt.body, out);
            break;
        case AstKind::StmtFor:
            collect_assigned_vars(stmt->for_stmt.body, out);
            collect_assigned_vars_expr(stmt->for_stmt.increment, out);
            break;
        case AstKind::StmtWhen: {
            WhenStmt& ws = stmt->when_stmt;
            for (auto& when_case : ws.cases) {
                for (auto* d : when_case.body) {
                    if (d && d->kind >= AstKind::StmtExpr && d->kind <= AstKind::StmtYield) {
                        collect_assigned_vars(&d->stmt, out);
                    }
                }
            }
            for (auto* d : ws.else_body) {
                if (d && d->kind >= AstKind::StmtExpr && d->kind <= AstKind::StmtYield) {
                    collect_assigned_vars(&d->stmt, out);
                }
            }
            break;
        }
        case AstKind::StmtThrow:
            collect_assigned_vars_expr(stmt->throw_stmt.expr, out);
            break;
        case AstKind::StmtYield:
            collect_assigned_vars_expr(stmt->yield_stmt.value, out);
            break;
        case AstKind::StmtTry: {
            TryStmt& ts = stmt->try_stmt;
            collect_assigned_vars(ts.try_body, out);
            for (u32 i = 0; i < ts.catches.size(); i++) {
                collect_assigned_vars(ts.catches[i].body, out);
            }
            if (ts.finally_body) {
                collect_assigned_vars(ts.finally_body, out);
            }
            break;
        }
        default:
            break;
    }
}

void IRBuilder::collect_assigned_vars_expr(Expr* expr, Vector<StringView>& out) {
    if (!expr) return;

    switch (expr->kind) {
        case AstKind::ExprAssign: {
            // Check if the target is an identifier
            if (expr->assign.target->kind == AstKind::ExprIdentifier) {
                StringView name = expr->assign.target->identifier.name;
                // Add if not already present
                bool found = false;
                for (const auto& existing : out) {
                    if (existing == name) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    out.push_back(name);
                }
            }
            // Recurse into value expression (it might have nested assignments)
            collect_assigned_vars_expr(expr->assign.value, out);
            break;
        }
        case AstKind::ExprBinary:
            collect_assigned_vars_expr(expr->binary.left, out);
            collect_assigned_vars_expr(expr->binary.right, out);
            break;
        case AstKind::ExprUnary:
            collect_assigned_vars_expr(expr->unary.operand, out);
            break;
        case AstKind::ExprTernary:
            collect_assigned_vars_expr(expr->ternary.condition, out);
            collect_assigned_vars_expr(expr->ternary.then_expr, out);
            collect_assigned_vars_expr(expr->ternary.else_expr, out);
            break;
        case AstKind::ExprCall:
            collect_assigned_vars_expr(expr->call.callee, out);
            for (auto& arg : expr->call.arguments) {
                collect_assigned_vars_expr(arg.expr, out);
                // An `inout`/`out` argument stores through the caller's slot
                // via the post-call reload (see gen_call_expr's inout_args).
                // Treat it as a write to the identifier so loop variable
                // collection phis the local at the loop header — otherwise
                // the reload redefines `xs` inside the body but the SSA value
                // never makes it back to the header, and post-loop uses read
                // a stale register / trip register allocation.
                if ((arg.modifier == ParamModifier::Inout || arg.modifier == ParamModifier::Out)
                    && arg.expr && arg.expr->kind == AstKind::ExprIdentifier) {
                    StringView name = arg.expr->identifier.name;
                    bool found = false;
                    for (const auto& existing : out) {
                        if (existing == name) { found = true; break; }
                    }
                    if (!found) out.push_back(name);
                }
            }
            break;
        case AstKind::ExprIndex:
            collect_assigned_vars_expr(expr->index.object, out);
            collect_assigned_vars_expr(expr->index.index, out);
            break;
        case AstKind::ExprGet:
            collect_assigned_vars_expr(expr->get.object, out);
            break;
        case AstKind::ExprGrouping:
            collect_assigned_vars_expr(expr->grouping.expr, out);
            break;
        case AstKind::ExprStringInterp:
            for (auto* sub_expr : expr->string_interp.expressions) {
                collect_assigned_vars_expr(sub_expr, out);
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

IROp IRBuilder::get_binary_op(BinaryOp op, Type* type) {
    bool is_f32 = type && type->kind == TypeKind::F32;
    bool is_f64 = type && type->kind == TypeKind::F64;

    switch (op) {
        case BinaryOp::Add:
            return is_f32 ? IROp::AddF : (is_f64 ? IROp::AddD : IROp::AddI);
        case BinaryOp::Sub:
            return is_f32 ? IROp::SubF : (is_f64 ? IROp::SubD : IROp::SubI);
        case BinaryOp::Mul:
            return is_f32 ? IROp::MulF : (is_f64 ? IROp::MulD : IROp::MulI);
        case BinaryOp::Div:
            return is_f32 ? IROp::DivF : (is_f64 ? IROp::DivD : IROp::DivI);
        case BinaryOp::Mod:
            return IROp::ModI;
        case BinaryOp::BitAnd:
            return IROp::BitAnd;
        case BinaryOp::BitOr:
            return IROp::BitOr;
        case BinaryOp::BitXor:
            return IROp::BitXor;
        case BinaryOp::Shl:
            return IROp::Shl;
        case BinaryOp::Shr:
            return IROp::Shr;
        default:
            return IROp::Copy;
    }
}

IROp IRBuilder::get_comparison_op(BinaryOp op, Type* type) {
    bool is_f32 = type && type->kind == TypeKind::F32;
    bool is_f64 = type && type->kind == TypeKind::F64;

    switch (op) {
        case BinaryOp::Equal:
            return is_f32 ? IROp::EqF : (is_f64 ? IROp::EqD : IROp::EqI);
        case BinaryOp::NotEqual:
            return is_f32 ? IROp::NeF : (is_f64 ? IROp::NeD : IROp::NeI);
        case BinaryOp::Less:
            return is_f32 ? IROp::LtF : (is_f64 ? IROp::LtD : IROp::LtI);
        case BinaryOp::LessEq:
            return is_f32 ? IROp::LeF : (is_f64 ? IROp::LeD : IROp::LeI);
        case BinaryOp::Greater:
            return is_f32 ? IROp::GtF : (is_f64 ? IROp::GtD : IROp::GtI);
        case BinaryOp::GreaterEq:
            return is_f32 ? IROp::GeF : (is_f64 ? IROp::GeD : IROp::GeI);
        default:
            return IROp::EqI;
    }
}

IROp IRBuilder::get_unary_op(UnaryOp op, Type* type) {
    bool is_f32 = type && type->kind == TypeKind::F32;
    bool is_f64 = type && type->kind == TypeKind::F64;

    switch (op) {
        case UnaryOp::Negate:
            return is_f32 ? IROp::NegF : (is_f64 ? IROp::NegD : IROp::NegI);
        case UnaryOp::Not:
            return IROp::Not;
        case UnaryOp::BitNot:
            return IROp::BitNot;
        case UnaryOp::Ref:
            return IROp::Copy;  // Handled specially in gen_unary_expr
    }
    return IROp::Copy;
}

// Name mangling helpers

StringView IRBuilder::mangle_method(StringView struct_name, StringView method_name) {
    return intern_format("{}$${}", struct_name, method_name);
}

StringView IRBuilder::mangle_constructor(StringView struct_name, StringView ctor_name) {
    if (ctor_name.empty()) {
        return intern_format("{}$$new", struct_name);
    }
    return intern_format("{}$$new$${}", struct_name, ctor_name);
}

StringView IRBuilder::mangle_module_local(StringView name) {
    if (m_module_name.empty()) return name;
    return intern_format("{}::{}", m_module_name, name);
}

void IRBuilder::emit_zero_slots(ValueId self_ptr, u32 start_slot, u32 slot_count) {
    if (slot_count == 0) return;
    ValueId null_val = emit_const_null();
    Type* void_ty = m_types.void_type();
    StringView tag("__zero", 6);
    u32 offset = start_slot;
    u32 remaining = slot_count;
    // 2-slot writes cover 8 bytes per op by splitting the u64 null register
    // across field[offset] and field[offset+1]. A trailing 1-slot write
    // handles the odd tail.
    while (remaining >= 2) {
        emit_set_field(self_ptr, tag, offset, 2, null_val, void_ty);
        offset += 2;
        remaining -= 2;
    }
    if (remaining == 1) {
        emit_set_field(self_ptr, tag, offset, 1, null_val, void_ty);
    }
}

Type* IRBuilder::apply_ref_kind(Type* base_type, RefKind ref_kind) {
    switch (ref_kind) {
        case RefKind::Uniq: return m_types.uniq_type(base_type);
        case RefKind::Ref:  return m_types.ref_type(base_type);
        case RefKind::Weak: return m_types.weak_type(base_type);
        case RefKind::None: return base_type;
    }
    return base_type;
}

void IRBuilder::begin_function_body(bool skip_hidden_return) {
    // Create entry block
    IRBlock* entry = create_block("entry");
    set_current_block(entry);

    // Initialize local variable scopes and ownership tracking
    m_local_scopes.clear_keep_capacity();
    m_owned_locals.clear_keep_capacity();
    m_next_temp_id = 0;
    push_scope();

    // Add function parameters to local scope
    // Skip the hidden return pointer parameter if requested
    u32 param_count = skip_hidden_return && m_current_func->returns_large_struct()
        ? m_current_func->params.size() - 1
        : m_current_func->params.size();
    for (u32 i = 0; i < param_count; i++) {
        BlockParam& bp = m_current_func->params[i];
        define_local(bp.name, bp.value, bp.type);

        // Track owned parameters — callee now owns them (uniq refs and value
        // structs with destructors). inout/out params are borrows through a
        // pointer: the caller still owns the slot and the callee must not
        // destroy it at scope exit — that would double-free the caller's
        // value. `m_param_is_ptr` is exactly the set of inout/out params, so
        // skip tracking when it contains `bp.name`.
        if (bp.type && bp.type->noncopyable() && !m_param_is_ptr.count(bp.name)) {
            u32 scope_depth = static_cast<u32>(m_local_scopes.size());
            BlockId current_block_id = m_current_block ? m_current_block->id : BlockId::invalid();
            m_owned_locals.push_back({bp.name, bp.type, scope_depth, false, false, current_block_id, bp.value});
        }
    }

    // Emit RefInc for ref-typed parameters at function entry
    // This tracks borrows in the constraint reference model
    for (const RefParamInfo& ref_param : m_ref_params) {
        emit_ref_inc(ref_param.value);
    }

    // Capture the entry block: ref params are live from here, so their
    // exception-path RefDec cleanup records (built in end_function_body) start
    // at this block.
    m_ref_param_entry_block = m_current_block ? m_current_block->id : BlockId::invalid();
}

void IRBuilder::end_function_body() {
    // If current block doesn't have a terminator, add implicit return
    if (m_current_block && m_current_block->terminator.kind == TerminatorKind::None) {
        // Emit cleanup for all scopes before implicit return
        emit_scope_cleanup(1);

        if (m_current_func->return_type->is_void()) {
            finish_block_return(ValueId::invalid());
        } else {
            // This shouldn't happen if semantic analysis passed
            // Return a default value
            ValueId default_val = emit_const_null();
            finish_block_return(default_val);
        }
    }

    // pop_scope without cleanup (already emitted above, or block is terminated)
    // Record cleanup info for exception-path cleanup before removing owned locals.
    // This is needed for functions that don't have try/catch but may have exceptions
    // propagate through them (cross-frame unwinding).
    if (!m_local_scopes.empty()) {
        u32 depth = static_cast<u32>(m_local_scopes.size());
        BlockId end_block = m_current_block ? m_current_block->id : BlockId::invalid();
        if (!m_current_block && !m_current_func->blocks.empty()) {
            end_block = m_current_func->blocks.back()->id;
        }
        // Record in declaration order; VM iterates in reverse for LIFO cleanup
        u32 first_in_scope = 0;
        for (u32 i = 0; i < m_owned_locals.size(); i++) {
            if (m_owned_locals[i].scope_depth >= depth) {
                first_in_scope = i;
                break;
            }
            if (i == m_owned_locals.size() - 1) {
                first_in_scope = static_cast<u32>(m_owned_locals.size());
            }
        }
        for (u32 i = first_in_scope; i < m_owned_locals.size(); i++) {
            auto& info = m_owned_locals[i];
            if (info.scope_depth < depth) continue;
            if (info.start_block.is_valid() && end_block.is_valid() && info.initial_value.is_valid()) {
                IRCleanupKind kind = info.kind == OwnedKind::RefBorrow
                    ? IRCleanupKind::RefDec : IRCleanupKind::Delete;
                m_current_func->cleanup_info.push_back(
                    {info.initial_value, info.type, info.start_block, end_block, kind});
            }
        }

        // Ref parameters are counted borrows live for the whole function. Their
        // normal-path RefDec is the explicit decrement at each return
        // (emit_ref_param_decrements); add a RefDec cleanup record spanning the
        // function so an exception unwinding OUT of this frame also decrements
        // them. Without this the borrow count leaks on every unwind path (e.g.
        // a callee throwing through a `ref`-param frame), and a later delete of
        // the borrowed owner spuriously traps. The "handler in scope" skip in
        // execute_cleanup leaves in-function catches to the normal-path RefDec,
        // so the two paths are mutually exclusive.
        if (m_ref_param_entry_block.is_valid() && end_block.is_valid()) {
            for (const RefParamInfo& rp : m_ref_params) {
                m_current_func->cleanup_info.push_back(
                    {rp.value, rp.type, m_ref_param_entry_block, end_block,
                     IRCleanupKind::RefDec, /*whole_function_scope=*/true});
            }
        }

        while (!m_owned_locals.empty() && m_owned_locals.back().scope_depth >= depth) {
            m_owned_locals.pop_back();
        }
        m_local_scopes.pop_back();
    }

    m_current_func->reorder_blocks_rpo();
}

void IRBuilder::setup_parameters(Span<Param> params, Type* self_type) {
    // Clear parameter tracking
    m_param_is_ptr.clear();  // robin_map::clear already keeps capacity
    m_ref_params.clear_keep_capacity();

    // Add 'self' parameter if this is a method/constructor/destructor
    if (self_type) {
        BlockParam self_param;
        self_param.value = m_current_func->new_value();
        self_param.type = m_types.ref_type(self_type);
        self_param.name = "self";
        m_current_func->params.push_back(self_param);
        m_current_func->param_is_ptr.push_back(true);
        m_param_is_ptr["self"] = true;
    }

    // Set up other parameters
    for (auto& param : params) {
        Type* param_type = nullptr;
        if (param.resolved_type) {
            // Use type resolved by semantic analysis (handles generics like List<T>)
            param_type = param.resolved_type;
        } else if (param.type) {
            param_type = m_type_env.type_by_name(param.type->name);
            if (!param_type) {
                param_type = m_types.error_type();
            }
            param_type = apply_ref_kind(param_type, param.type->ref_kind);
        }

        bool is_ptr = (param.modifier != ParamModifier::None);
        if (is_ptr) {
            m_param_is_ptr[param.name] = true;
        }

        BlockParam bp;
        bp.value = m_current_func->new_value();
        bp.type = param_type;
        bp.name = param.name;
        m_current_func->params.push_back(bp);
        m_current_func->param_is_ptr.push_back(is_ptr);

        // Track ref-typed parameters for RefInc/RefDec at boundaries
        if (param_type && param_type->kind == TypeKind::Ref) {
            m_ref_params.push_back({bp.value, param_type});
        }
    }
}

StringView IRBuilder::mangle_destructor(StringView struct_name, StringView dtor_name) {
    if (dtor_name.empty()) {
        return intern_format("{}$$delete", struct_name);
    }
    return intern_format("{}$$delete$${}", struct_name, dtor_name);
}

}
