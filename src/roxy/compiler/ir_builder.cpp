// IRBuilder — module build phases, globals, and per-function scaffolding
// (the build_* entry points, parameter setup, function begin/end, mangling).
// Statement lowering lives in ir_builder_stmt.cpp, expression lowering and
// instruction emission in ir_builder_expr.cpp, ownership/cleanup bookkeeping
// in ir_builder_lifetime.cpp, and pure constant folding in ir_fold.cpp.
// File-internal helpers shared across the TUs live in ir_builder_internal.hpp.

#include "roxy/compiler/ir_builder.hpp"

#include "ir_builder_internal.hpp"

namespace rx {

using namespace ir_builder_detail;

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

    // Assign module-global slot offsets first, so any function body can resolve
    // a global reference to its slot while being built.
    m_global_indices.clear();
    collect_globals(program);

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

    // Synthesize the module init/teardown functions for globals. Built after
    // user decls so the constructors/destructors they invoke already exist.
    if (IRFunction* init_fn = build_module_init(program)) {
        m_module->functions.push_back(init_fn);
    }
    if (m_has_error) return nullptr;
    if (IRFunction* shutdown_fn = build_module_shutdown()) {
        m_module->functions.push_back(shutdown_fn);
    }
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
        if (instance->is_abstract) continue;  // Phase-B artifact, never codegen'd
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
    for_each_concrete_struct_instance([&](auto* instance) {
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
    });
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
    for_each_concrete_struct_instance([&](auto* instance) {
        // Check if there's a user-defined default constructor for this instance
        if (has_default_ctor.find(instance->mangled_name) == has_default_ctor.end()) {
            IRFunction* func = build_synthesized_default_constructor(instance->concrete_type);
            m_module->functions.push_back(func);
        }
    });
}

void IRBuilder::build_generic_struct_methods() {
    // Generate external methods for generic struct instances
    for_each_concrete_struct_instance([&](auto* instance) {
        for (Decl* method_decl : instance->instantiated_methods) {
            MethodDecl& method = method_decl->method_decl;
            if (method.body) {
                IRFunction* func = build_method(&method, instance->concrete_type);
                m_module->functions.push_back(func);
            }
        }
    });
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
    for_each_concrete_struct_instance([&](auto* instance) {
        Type* concrete_type = instance->concrete_type;
        if (!concrete_type->is_struct()) return;

        // Check for synthetic default destructor (decl == nullptr)
        for (const auto& dtor : concrete_type->struct_info.destructors) {
            if (dtor.name.empty() && dtor.decl == nullptr) {
                IRFunction* func = build_synthesized_default_destructor(concrete_type);
                m_module->functions.push_back(func);
                break;
            }
        }
    });

    // Synthesized closure-env structs aren't in program->declarations, so build
    // their destructors here. An env gets one only when it has cleanup-needing
    // captures (a noncopyable/ref capture made backfill_lambda_env attach a
    // synthetic default destructor). The closure delete dispatches it by type_id.
    for (Type* env_type : m_env_struct_types) {
        if (!env_type || !env_type->is_struct()) continue;
        // Make the synthesized env struct visible to the C backend (typedef +
        // dependency sort + TYPEID). collect_backend_types ran before lambdas
        // were processed, so these aren't in struct_types yet; the VM ignores
        // struct_types, so this is safe for both pipelines.
        m_module->struct_types.push_back(env_type);
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

void IRBuilder::collect_globals(Program* program) {
    u32 offset = 0;
    for (auto* decl : program->declarations) {
        if (!decl || decl->kind != AstKind::DeclVar) continue;
        VarDecl& var_decl = decl->var_decl;
        Type* type = var_decl.resolved_type;
        if (!type) continue;
        u32 slot_count = slot_count_or_1(type);
        IRGlobal g;
        g.name = var_decl.name;
        g.type = type;
        g.slot_offset = offset;
        g.slot_count = slot_count;
        g.initializer = var_decl.initializer;
        m_global_indices[var_decl.name] = static_cast<u32>(m_module->globals.size());
        m_module->globals.push_back(g);
        offset += slot_count;
    }
    m_module->global_slot_count = offset;
}

ValueId IRBuilder::emit_global_addr(u32 slot_offset, Type* type) {
    IRInst* inst = emit_inst(IROp::GlobalAddr, type);
    if (!inst) return ValueId::invalid();
    inst->global_data.slot_offset = slot_offset;
    return inst->result;
}

ValueId IRBuilder::gen_global_read(u32 global_index, Type* /*result_type*/) {
    const IRGlobal& g = m_module->globals[global_index];
    Type* type = g.type;
    u32 slot_count = g.slot_count;
    ValueId addr = emit_global_addr(g.slot_offset, type);
    // For struct globals the address IS the value (field ops want a pointer);
    // for everything else, load the stored value out of the slot.
    if (type && type->is_struct()) return addr;
    return emit_load_ptr(addr, slot_count, type);
}

// Synthesize `__module_init`: run each global's initializer (incl. constructors)
// and store the result into its slot. Returns null if there is nothing to init.
IRFunction* IRBuilder::build_module_init(Program* /*program*/) {
    bool any_init = false;
    for (const IRGlobal& g : m_module->globals) {
        if (g.initializer) { any_init = true; break; }
    }
    if (!any_init) return nullptr;

    begin_ir_function(StringView("__module_init", 13), /*is_pub=*/false, 0);
    m_current_func->return_type = m_types.void_type();
    setup_parameters(Span<Param>(), nullptr);
    begin_function_body(false);

    for (u32 i = 0; i < m_module->globals.size(); i++) {
        // Copy fields out before gen_expr (which may push functions/temps).
        Type* type = m_module->globals[i].type;
        u32 slot_offset = m_module->globals[i].slot_offset;
        u32 slot_count = m_module->globals[i].slot_count;
        Expr* initializer = m_module->globals[i].initializer;
        if (!initializer) continue;

        ValueId addr = emit_global_addr(slot_offset, type);
        ValueId val = gen_expr(initializer);
        val = maybe_wrap_weak(val, initializer->resolved_type, type, initializer);
        if (type && type->is_struct()) {
            emit_struct_copy(addr, val, slot_count);
        } else {
            emit_store_ptr(addr, val, slot_count, type);
        }
        // A `ref` global is a counted borrow held for the whole VM lifetime: the
        // create-inc goes here in __module_init and the matching dec in
        // __module_shutdown, so `delete owner` traps while the global still
        // borrows it (lifetimes.md "Constraint references"; finding 8a). A call
        // initializer already hands off a count (adopt, no inc), mirroring a ref
        // local (gen_var_decl); a `weak` global is generational and needs none;
        // `self` promotion can't occur at module scope.
        if (type && type->kind == TypeKind::Ref && !is_ref_handoff_source(initializer)) {
            emit_ref_borrow_inc(val, initializer);
        }
        // The initializer temporary's ownership transfers into the global slot.
        if (type && type->noncopyable()) {
            consume_temp_noncopyable(val);
        }
    }

    end_function_body();
    return finish_ir_function();
}

// Synthesize `__module_shutdown`: destroy noncopyable globals (uniq/List/Map,
// or value structs with destructors) in reverse declaration order. Returns null
// if no global needs teardown.
IRFunction* IRBuilder::build_module_shutdown() {
    bool any = false;
    for (const IRGlobal& g : m_module->globals) {
        // Noncopyable globals need destruction; a `ref` global needs its
        // create-inc (build_module_init, finding 8a) released here even though
        // `ref` is copyable.
        if (g.type && (g.type->noncopyable() || g.type->kind == TypeKind::Ref)) {
            any = true;
            break;
        }
    }
    if (!any) return nullptr;

    begin_ir_function(StringView("__module_shutdown", 17), /*is_pub=*/false, 0);
    m_current_func->return_type = m_types.void_type();
    setup_parameters(Span<Param>(), nullptr);
    begin_function_body(false);

    for (i32 i = static_cast<i32>(m_module->globals.size()) - 1; i >= 0; i--) {
        Type* type = m_module->globals[i].type;
        u32 slot_offset = m_module->globals[i].slot_offset;
        u32 slot_count = m_module->globals[i].slot_count;
        if (!type) continue;

        // A `ref` global: release the borrow acquired in __module_init (finding
        // 8a). Reverse-declaration order runs this before the borrowed owner's
        // Delete (a ref global is declared after the global it borrows), so the
        // dec drops the count to 0 before the owner is freed. If user code had
        // tried to `delete` the owner earlier it would have trapped, so the
        // pointee is still live here.
        if (type->kind == TypeKind::Ref) {
            ValueId addr = emit_global_addr(slot_offset, type);
            ValueId val = emit_load_ptr(addr, slot_count, type);
            emit_ref_dec(val);
            continue;
        }
        if (type->is_copy()) continue;

        ValueId addr = emit_global_addr(slot_offset, type);
        if (type->is_struct()) {
            // Value struct with a destructor: destroy in place via its address.
            emit_delete(addr, type);
        } else {
            // uniq / List / Map: the slot holds the owning pointer.
            ValueId val = emit_load_ptr(addr, slot_count, type);
            emit_delete(val, type);
        }
    }

    end_function_body();
    return finish_ir_function();
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
    for_each_concrete_struct_instance([&](auto* instance) {
        m_module->struct_types.push_back(instance->concrete_type);
    });
}

void IRBuilder::begin_ir_function(StringView name, bool is_pub, u32 source_line) {
    m_current_func = m_allocator.emplace<IRFunction>();
    m_current_func->name = name;
    m_current_func->is_pub = is_pub;
    m_current_func->source_line = source_line;
    // Instructions emitted before the first statement (parameter RefIncs,
    // zero-init preambles, synthesized bodies) carry "unknown" rather than a
    // stale line from the previously built function.
    m_current_source_line = 0;
}

IRFunction* IRBuilder::finish_ir_function() {
    IRFunction* result = m_current_func;
    m_current_func = nullptr;
    m_current_block = nullptr;
    return result;
}

void IRBuilder::gen_body(Stmt* body) {
    if (!body || body->kind != AstKind::StmtBlock) return;
    for (auto* decl : body->block.declarations) {
        gen_decl(decl);
    }
}

void IRBuilder::add_hidden_return_param() {
    if (!m_current_func->returns_large_struct()) return;
    BlockParam hidden_param;
    hidden_param.value = m_current_func->new_value();
    hidden_param.type = m_current_func->return_type;  // Pointer to struct
    hidden_param.name = "__ret_ptr";
    m_current_func->params.push_back(hidden_param);
    m_current_func->param_is_ptr.push_back(true);
}

Type* IRBuilder::resolve_return_type(TypeExpr* return_type_expr, StringView symbol_name) {
    if (!return_type_expr) return m_types.void_type();
    // Prefer the symbol table's resolved function type (semantic analysis
    // already resolved it) for declarations that are function symbols.
    if (!symbol_name.empty()) {
        Symbol* func_sym = m_symbols.lookup(symbol_name);
        if (func_sym && func_sym->type && func_sym->type->is_function()) {
            return func_sym->type->func_info.return_type;
        }
    }
    Type* type = m_type_env.type_by_name(return_type_expr->name);
    if (!type) type = m_types.void_type();
    return apply_ref_kind(type, return_type_expr->ref_kind);
}

IRFunction* IRBuilder::build_function(FunDecl* decl) {
    // Non-pub functions are scoped to their module so they don't collide at link time.
    // "main" is the program entry point convention — leave it un-mangled so the host
    // can still invoke it via vm_call(&vm, "main", {}).
    StringView name = (!decl->is_pub && decl->name != StringView("main", 4))
        ? mangle_module_local(decl->name)
        : decl->name;
    // Source line for AOT `#line` directives. Use the body's first line —
    // typically the same as the function header or the next line after.
    begin_ir_function(name, decl->is_pub, decl->body ? decl->body->loc.line : 0);

    // Set up parameters
    setup_parameters(decl->params);

    m_current_func->return_type = resolve_return_type(decl->return_type, decl->name);

    // Detect coroutine function (returns Coro<T>)
    if (m_current_func->return_type && m_current_func->return_type->is_coroutine()) {
        m_current_func->is_coroutine = true;
        m_current_func->coro_type = m_current_func->return_type;
        m_current_func->coro_yield_type = m_current_func->return_type->coro_info.yield_type;
        m_current_func->coro_struct_type = m_current_func->return_type->coro_info.generated_struct_type;
        // A coroutine's `ref` params are NOT counted via the normal per-frame
        // resume-flow inc/dec: the coroutine split scatters the entry-inc and
        // exit-dec across resume states, so the dec is missed when the coro is
        // destroyed before completion (the owner's delete then spuriously traps —
        // a real break). Instead the borrow is counted for the *state struct's*
        // lifetime — ref_inc when the param is stored into the state at creation,
        // ref_dec in `$$delete` (coroutine_lowering.cpp). So drop the per-frame
        // ref-param tracking here. (lifetimes.md "Value lifecycle".)
        m_ref_params.clear_keep_capacity();
    }

    // Check for large struct return - add hidden output pointer as last parameter
    add_hidden_return_param();

    begin_function_body(true);  // skip hidden return pointer
    gen_body(decl->body);
    end_function_body();
    return finish_ir_function();
}

IRFunction* IRBuilder::build_constructor(ConstructorDecl* decl, Type* struct_type) {
    begin_ir_function(mangle_constructor(decl->struct_name, decl->name), decl->is_pub,
                      decl->body ? decl->body->loc.line : 0);

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
        // 'self' is the first parameter
        emit_call(parent_ctor_name, alloc_span({m_current_func->params[0].value}),
                  m_types.void_type());
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

    gen_body(decl->body);
    end_function_body();
    return finish_ir_function();
}

IRFunction* IRBuilder::build_destructor(DestructorDecl* decl, Type* struct_type) {
    begin_ir_function(mangle_destructor(decl->struct_name, decl->name), decl->is_pub,
                      decl->body ? decl->body->loc.line : 0);

    // Set up parameters with 'self' as first parameter
    setup_parameters(decl->params, struct_type);

    // Destructors return void
    m_current_func->return_type = m_types.void_type();

    begin_function_body(false);
    gen_body(decl->body);

    // After child destructor body, call parent's default destructor if present
    // Only chain default destructors (named destructors are called explicitly)
    Type* parent_type = struct_type->struct_info.parent;
    if (parent_type && decl->name.empty()) {
        StringView parent_dtor_name = mangle_destructor(parent_type->struct_info.name);
        // 'self' is the first parameter
        emit_call(parent_dtor_name, alloc_span({m_current_func->params[0].value}),
                  m_types.void_type());
    }

    // For default destructors, clean up uniq fields after user body and parent chain
    if (decl->name.empty()) {
        emit_field_cleanup(m_current_func->params[0].value, struct_type);
    }

    end_function_body();
    return finish_ir_function();
}

IRFunction* IRBuilder::build_method(MethodDecl* decl, Type* struct_type) {
    begin_ir_function(mangle_method(decl->struct_name, decl->name), decl->is_pub,
                      decl->body ? decl->body->loc.line : 0);

    // Set up parameters with 'self' as first parameter
    setup_parameters(decl->params, struct_type);

    // Resolve return type (methods aren't function symbols — pass no name)
    m_current_func->return_type = resolve_return_type(decl->return_type, StringView());

    // Check for large struct return - add hidden output pointer as last parameter
    add_hidden_return_param();

    begin_function_body(true);  // skip hidden return pointer
    gen_body(decl->body);
    end_function_body();
    return finish_ir_function();
}

IRFunction* IRBuilder::build_synthesized_default_constructor(Type* struct_type) {
    StructTypeInfo& struct_type_info = struct_type->struct_info;
    begin_ir_function(mangle_constructor(struct_type_info.name),
                      struct_type_info.decl && struct_type_info.decl->struct_decl.is_pub,
                      0);

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
        emit_call(parent_ctor_name, alloc_span({self_param.value}), m_types.void_type());
    }

    // Initialize own fields (declared defaults / zero-init / recursive nested
    // structs), own discriminants, and own union regions. Inherited fields were
    // populated by the parent constructor call above.
    emit_own_field_default_init(self_param.value, struct_type);

    // End function body (will add implicit return)
    end_function_body();
    return finish_ir_function();
}

ValueId IRBuilder::emit_zero_value(Type* type) {
    if (type->is_bool()) return emit_const_bool(false);
    if (type->is_integer() || type->is_enum()) return emit_const_int(0, type);
    if (type->is_float()) return emit_const_float(0.0, type);
    if (type->kind == TypeKind::String) return emit_const_string("");
    return emit_const_null();
}

void IRBuilder::emit_own_field_default_init(ValueId self_ptr, Type* struct_type) {
    StructTypeInfo& struct_type_info = struct_type->struct_info;
    // No declaration (synthesized struct types) — nothing declares a default.
    if (!struct_type_info.decl) return;
    StructDecl& struct_decl = struct_type_info.decl->struct_decl;

    // Initialize regular fields from struct_decl.fields
    for (const auto& field : struct_decl.fields) {
        const FieldInfo* field_info = struct_type_info.find_field(field.name);
        if (!field_info) continue;

        if (field.default_value) {
            ValueId value = gen_expr(field.default_value);
            // Struct rvalues are pointers — copy slot-by-slot into the field.
            if (field_info->type && field_info->type->is_struct()) {
                ValueId field_addr = emit_get_field_addr(self_ptr, field_info->name,
                                                         field_info->slot_offset, field_info->type);
                emit_struct_copy(field_addr, value, field_info->slot_count);
            } else {
                emit_set_field(self_ptr, field_info->name, field_info->slot_offset,
                               field_info->slot_count, value, field_info->type);
            }
        } else if (field_info->type && field_info->type->is_struct()) {
            // Nested value struct: default-init it in place, at any depth.
            ValueId field_addr = emit_get_field_addr(self_ptr, field_info->name,
                                                     field_info->slot_offset, field_info->type);
            emit_struct_default_init(field_addr, field_info->type);
        } else {
            ValueId value = emit_zero_value(field_info->type);
            emit_set_field(self_ptr, field_info->name, field_info->slot_offset,
                           field_info->slot_count, value, field_info->type);
        }
    }

    // Discriminants are zero-initialized (first enum variant)
    for (const auto& wfd : struct_decl.when_clauses) {
        const FieldInfo* field_info = struct_type_info.find_field(wfd.discriminant_name);
        if (!field_info) continue;
        ValueId value = emit_zero_value(field_info->type);
        emit_set_field(self_ptr, field_info->name, field_info->slot_offset,
                       field_info->slot_count, value, field_info->type);
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
}

void IRBuilder::emit_struct_default_init(ValueId ptr, Type* struct_type) {
    // Walk the inheritance chain: unlike the synthesized constructor's own
    // body, an in-place nested-field init has no parent-constructor call to
    // populate inherited fields, so every level initializes here.
    for (Type* current = struct_type; current && current->is_struct();
         current = current->struct_info.parent) {
        emit_own_field_default_init(ptr, current);
    }
}

IRFunction* IRBuilder::build_synthesized_default_destructor(Type* struct_type) {
    StructTypeInfo& struct_type_info = struct_type->struct_info;
    begin_ir_function(mangle_destructor(struct_type_info.name),
                      struct_type_info.decl && struct_type_info.decl->struct_decl.is_pub,
                      0);

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
                emit_call(parent_dtor_name, alloc_span({self_ptr}), m_types.void_type());
                break;
            }
        }
    }

    // Clean up uniq fields
    emit_field_cleanup(self_ptr, struct_type);

    end_function_body();
    return finish_ir_function();
}

IRFunction* IRBuilder::build_cleanup_wrapper(Type* noncopyable_type, u32 wrapper_index) {
    begin_ir_function(intern_format("__cleanup_wrapper_{}", wrapper_index),
                      /*is_pub=*/false, 0);

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
    emit_delete(param_val, noncopyable_type);

    // Set Return terminator directly (avoid finish_block_return which calls emit_ref_param_decrements).
    if (m_current_block && m_current_block->terminator.kind == TerminatorKind::None) {
        m_current_block->terminator.kind = TerminatorKind::Return;
        m_current_block->terminator.return_value = ValueId::invalid();
    }

    return finish_ir_function();
}

// Block management

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
        record_scope_cleanup_records(depth);
        BlockId end_block = current_or_last_block_id();

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

    // Append deferred call-site receiver-borrow records last, so they sort after
    // every owned-local Delete record and thus run FIRST on the reverse-ordered
    // unwind — releasing the borrow before the owner is destroyed (else the
    // owner's Delete would see ref_count != 0 and spuriously trap).
    for (const IRCleanupInfo& ci : m_call_borrow_cleanups) {
        m_current_func->cleanup_info.push_back(ci);
    }

    m_current_func->reorder_blocks_rpo();
}

void IRBuilder::setup_parameters(Span<Param> params, Type* self_type) {
    // Clear parameter tracking
    m_param_is_ptr.clear();  // robin_map::clear already keeps capacity
    m_ref_params.clear_keep_capacity();
    m_call_borrow_cleanups.clear_keep_capacity();

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
