#include "roxy/compiler/semantic.hpp"
#include "roxy/compiler/operator_traits.hpp"
#include "roxy/compiler/module_registry.hpp"
#include "roxy/vm/natives.hpp"
#include "roxy/vm/binding/registry.hpp"

#include <cstring>
#include <utility>

namespace rx {

namespace {
// RAII guard for the analyzer's implicit "current context" members. Saves a
// slot's value on construction and restores it on destruction, so a method can
// freely mutate the member and have the previous value reinstated at scope exit
// — even on an early return. Replaces the manual `auto prev = m_x; m_x = ...;
// ...; m_x = prev;` idiom, which silently leaks state if a return slips between
// the save and the restore.
template <typename T>
class ScopedValue {
public:
    explicit ScopedValue(T& slot) : m_slot(slot), m_saved(slot) {}
    ~ScopedValue() { m_slot = std::move(m_saved); }
    ScopedValue(const ScopedValue&) = delete;
    ScopedValue& operator=(const ScopedValue&) = delete;
private:
    T& m_slot;
    T m_saved;
};
}

// Build a MethodInfo with all fields set (native_name defaults to empty).
static MethodInfo make_method(StringView name, Span<Type*> param_types,
                              Type* return_type, StringView native_name = StringView()) {
    MethodInfo method;
    method.name = name;
    method.param_types = param_types;
    method.return_type = return_type;
    method.decl = nullptr;
    method.native_name = native_name;
    return method;
}

static const ConstructorInfo* find_constructor(Span<ConstructorInfo> constructors, StringView name) {
    for (const auto& constructor : constructors) {
        if (constructor.name == name) {
            return &constructor;
        }
    }
    return nullptr;
}

// get_type_slot_count is declared in types.hpp and defined in types.cpp.

// ===== Initialization & Passes =====

SemanticAnalyzer::SemanticAnalyzer(BumpAllocator& allocator, TypeEnv& type_env, ModuleRegistry& modules,
                                   NativeRegistry* registry)
    : m_allocator(allocator)
    , m_type_env(type_env)
    , m_types(type_env.types())
    , m_modules(modules)
    , m_registry(registry)
    , m_owned_symbols(new SymbolTable(allocator))
    , m_symbols(*m_owned_symbols)
    , m_reporter(allocator)
    , m_checker(m_reporter)
    , m_program(nullptr)
{
}

SemanticAnalyzer::SemanticAnalyzer(BumpAllocator& allocator, TypeEnv& type_env, ModuleRegistry& modules,
                                   SymbolTable& external_symbols, NativeRegistry* registry)
    : m_allocator(allocator)
    , m_type_env(type_env)
    , m_types(type_env.types())
    , m_modules(modules)
    , m_registry(registry)
    , m_owned_symbols(nullptr)
    , m_symbols(external_symbols)
    , m_reporter(allocator)
    , m_checker(m_reporter)
    , m_program(nullptr)
{
}

void SemanticAnalyzer::set_lsp_mode(bool enable) { m_reporter.set_lsp_mode(enable); }
bool SemanticAnalyzer::lsp_mode() const { return m_reporter.lsp_mode(); }

Type* SemanticAnalyzer::register_builtin_trait(
        StringView name, StringView method_name,
        Span<Type*> method_param_types, Type* return_type,
        Span<TypeKind> primitive_kinds, bool register_trait_on_primitives) {
    Type* trait_type = m_types.trait_type(name, nullptr);
    m_type_env.register_trait_type(name, trait_type);

    TraitMethodInfo tmi;
    tmi.name = method_name;
    tmi.param_types = method_param_types;
    tmi.return_type = return_type;
    tmi.decl = nullptr;
    tmi.has_default = false;
    trait_type->trait_info.methods =
        Span<TraitMethodInfo>(m_allocator.emplace<TraitMethodInfo>(tmi), 1);

    for (TypeKind tk : primitive_kinds) {
        MethodInfo mi;
        mi.name = method_name;
        mi.param_types = method_param_types;
        mi.return_type = return_type;
        mi.decl = nullptr;
        m_types.register_primitive_method(tk, mi);
        if (register_trait_on_primitives) {
            m_types.register_primitive_trait(tk, trait_type);
        }
    }
    return trait_type;
}

void SemanticAnalyzer::run_declaration_passes(Program* program) {
    m_program = program;

    // Pass 0a: Auto-import builtin module as prelude
    import_builtin_prelude();

    // Pass 0b: Process user imports
    for (auto* decl : program->declarations) {
        if (decl && decl->kind == AstKind::DeclImport) {
            analyze_import_decl(decl);
        }
    }

    // Pass 0c: Apply native function symbols from registry (non-method entries)
    if (m_registry) {
        m_registry->apply_to_symbols(m_symbols, m_types, m_allocator);
    }

    // Pass 1: Collect type declarations (struct/enum names)
    collect_type_declarations(program);

    // Pass 1.5: Create native struct types from registry
    if (m_registry) {
        m_registry->apply_structs_to_types(m_type_env, m_allocator, m_symbols);
    }

    // Pass 1.6: Apply native methods to struct types
    if (m_registry) {
        m_registry->apply_methods_to_types(m_type_env, m_allocator);
    }

    // Pass 1.7: Register builtin traits (Printable, Hash, Exception)
    // and primitive operator methods — guarded against re-initialization
    if (!m_type_env.printable_type()) {
        TypeKind prim_kinds[] = {
            TypeKind::Bool, TypeKind::I32, TypeKind::I64,
            TypeKind::F32, TypeKind::F64, TypeKind::String
        };
        Type* printable_type = register_builtin_trait(
            StringView("Printable", 9), StringView("to_string", 9),
            Span<Type*>(nullptr, 0), m_types.string_type(),
            Span<TypeKind>(prim_kinds, 6), /*register_trait_on_primitives=*/true);
        m_type_env.set_printable_type(printable_type);
    }

    if (!m_type_env.hash_type()) {
        TypeKind hashable_kinds[] = {
            TypeKind::Bool,
            TypeKind::I8, TypeKind::I16, TypeKind::I32, TypeKind::I64,
            TypeKind::U8, TypeKind::U16, TypeKind::U32, TypeKind::U64,
            TypeKind::F32, TypeKind::F64,
            TypeKind::String
        };
        Type* hash_trait_type = register_builtin_trait(
            StringView("Hash", 4), StringView("hash", 4),
            Span<Type*>(nullptr, 0), m_types.u64_type(),
            Span<TypeKind>(hashable_kinds, 12), /*register_trait_on_primitives=*/true);
        m_type_env.set_hash_type(hash_trait_type);
    }

    // Builtin `Eq` trait used by Map's struct-key dispatch — `Map<K, V>` uses
    // custom hash/eq runtime dispatch only when K explicitly implements both
    // `Hash` and `Eq` (just defining `hash()` / `eq()` methods isn't enough).
    // We declare `eq(other: Self): bool` as the required trait method so user
    // `for Eq` impls match against it, but we don't register `eq` as a method
    // on primitive types — that would shadow the native operator-dispatch
    // path for `==` on primitives.
    //
    // Tests / user code may also write `trait Eq;` to declare the trait for
    // operator overloading — the trait-decl handler below merges that with
    // this builtin instead of erroring on duplicate.
    if (!m_type_env.eq_type()) {
        // Required method: fun Eq.eq(other: Self): bool. Eq is intentionally not
        // registered on primitives (see comment above), so primitive_kinds is
        // empty and register_trait_on_primitives is irrelevant.
        Span<Type*> eq_params(m_allocator.emplace<Type*>(m_types.self_type()), 1);
        Type* eq_trait_type = register_builtin_trait(
            StringView("Eq", 2), StringView("eq", 2),
            eq_params, m_types.bool_type(),
            Span<TypeKind>(), /*register_trait_on_primitives=*/false);
        m_type_env.set_eq_type(eq_trait_type);
    }

    if (!m_type_env.exception_type()) {
        // `message` is installed on ExceptionRef as a primitive method, but the
        // Exception trait itself is not registered on it (catch-all handles only
        // message()), so register_trait_on_primitives is false.
        TypeKind exc_kinds[] = { TypeKind::ExceptionRef };
        Type* exception_trait_type = register_builtin_trait(
            StringView("Exception", 9), StringView("message", 7),
            Span<Type*>(nullptr, 0), m_types.string_type(),
            Span<TypeKind>(exc_kinds, 1), /*register_trait_on_primitives=*/false);
        m_type_env.set_exception_type(exception_trait_type);
    }

    // Pass 1.8: Register built-in operator trait methods for primitive types
    register_primitive_operator_methods();

    // Pass 1.9: Resolve trait bounds on generic type parameters
    resolve_generic_bounds();

    // Pass 2: Resolve type members (field types, parent types, method signatures)
    resolve_type_members(program);
}

void SemanticAnalyzer::run_body_analysis(Program* program) {
    m_program = program;
    analyze_function_bodies(program);
}

void SemanticAnalyzer::set_program(Program* program) {
    m_program = program;
}

u32 SemanticAnalyzer::analyze_owned_pending_fun_instances() {
    // Drain pending generic-fun instances whose template was registered by
    // this analyzer's module. Cross-module-owned instances are sidelined
    // for the next compiler-level pass; new instances triggered by analysis
    // (which land back in m_pending_funs) get drained on the next iteration
    // of the compiler's outer loop.
    if (!m_type_env.generics().has_pending_funs()) return 0;
    StringView this_module = m_program ? m_program->module_name : StringView{};
    auto pending = m_type_env.generics().take_pending_funs();
    u32 drained = 0;
    for (auto* inst : pending) {
        bool owns = inst->template_module.empty()
                 || this_module.empty()
                 || inst->template_module == this_module;
        if (owns) {
            analyze_fun_decl(inst->instantiated_decl);
            inst->is_analyzed = true;
            drained++;
        } else {
            m_type_env.generics().sideline_cross_module_fun(inst);
        }
    }
    return drained;
}

void SemanticAnalyzer::analyze_single_function(Decl* decl) {
    if (!decl) return;

    // Import builtin prelude so symbols are available
    import_builtin_prelude();

    // Analyze the declaration based on its kind
    switch (decl->kind) {
        case AstKind::DeclFun:
            analyze_fun_decl(decl);
            break;
        case AstKind::DeclMethod:
            analyze_method_decl(decl);
            break;
        case AstKind::DeclConstructor:
            analyze_constructor_decl(decl);
            break;
        case AstKind::DeclDestructor:
            analyze_destructor_decl(decl);
            break;
        default:
            break;
    }
}

void SemanticAnalyzer::import_builtin_prelude() {
    // Auto-import all exports from the "builtin" module if available
    ModuleInfo* builtin_module = m_modules.find_module(BUILTIN_MODULE_NAME);
    if (!builtin_module) return;

    // Import all exports from the builtin module into global scope
    for (const ModuleExport& exp : builtin_module->exports) {
        // Skip if already defined (shouldn't happen, but be safe)
        if (m_symbols.lookup_local(exp.name)) continue;

        // Register the imported symbol based on its kind
        if (exp.kind == ExportKind::Function) {
            m_symbols.define_imported_function(
                exp.name, exp.type, SourceLocation{0, 0, 0, 0},
                BUILTIN_MODULE_NAME,
                exp.name, exp.index, exp.is_native);
        } else {
            // For structs/enums, define as regular types
            m_symbols.define(static_cast<SymbolKind>(
                exp.kind == ExportKind::Struct ? SymbolKind::Struct : SymbolKind::Enum),
                exp.name, exp.type, SourceLocation{0, 0, 0, 0}, exp.decl);
        }
    }
}

bool SemanticAnalyzer::analyze(Program* program) {
    // Run declaration passes (0-2)
    run_declaration_passes(program);
    if (too_many_errors()) return false;

    // Pass 3: Analyze function bodies (full type checking)
    run_body_analysis(program);

    return !has_errors();
}

// Pass 1: Collect type declarations

void SemanticAnalyzer::collect_type_declarations(Program* program) {
    for (auto* decl : program->declarations) {
        if (!decl) continue;

        // Register generic functions as templates (not concrete functions)
        if (decl->kind == AstKind::DeclFun && decl->fun_decl.type_params.size() > 0) {
            m_type_env.generics().register_generic_fun(
                decl->fun_decl.name, decl, m_program ? m_program->module_name : StringView{});
            continue;
        }

        if (decl->kind == AstKind::DeclStruct) {
            StringView name = decl->struct_decl.name;

            // Register generic structs as templates (not concrete types)
            if (decl->struct_decl.type_params.size() > 0) {
                m_type_env.generics().register_generic_struct(name, decl);
                continue;
            }

            // Check for duplicate type names
            if (m_type_env.named_type_by_name(name) != nullptr) {
                error_fmt(decl->loc, "duplicate type declaration '{}'", name);
                continue;
            }

            // Create the struct type
            Type* type = m_types.struct_type(name, decl, m_program->module_name);
            m_type_env.register_named_type(name, type);

            // Define in global scope
            m_symbols.define(SymbolKind::Struct, name, type, decl->loc, decl);
        }
        else if (decl->kind == AstKind::DeclEnum) {
            StringView name = decl->enum_decl.name;

            // Check for duplicate type names
            if (m_type_env.named_type_by_name(name) != nullptr) {
                error_fmt(decl->loc, "duplicate type declaration '{}'", name);
                continue;
            }

            // Create the enum type
            Type* type = m_types.enum_type(name, decl);
            populate_enum_methods(type);
            m_type_env.register_named_type(name, type);

            // Define in global scope
            m_symbols.define(SymbolKind::Enum, name, type, decl->loc, decl);
        }
        else if (decl->kind == AstKind::DeclTrait) {
            StringView name = decl->trait_decl.name;

            // Allow user redeclaration of builtin traits (Hash, Eq, etc.):
            // a builtin trait was registered with `decl == nullptr` at this
            // pass's start; if the user writes `trait Eq;`, attach their decl
            // to the existing trait type rather than erroring on duplicate.
            // The user's trait method declarations (later in Pass 2) populate
            // the trait's methods on the existing type.
            Type* existing_trait = m_type_env.trait_type_by_name(name);
            if (existing_trait != nullptr && existing_trait->trait_info.decl == nullptr) {
                existing_trait->trait_info.decl = decl;
                existing_trait->trait_info.type_params = decl->trait_decl.type_params;
                m_symbols.define(SymbolKind::Trait, name, existing_trait, decl->loc, decl);
                continue;
            }

            // Check for duplicate type/trait names
            if (m_type_env.named_type_by_name(name) != nullptr ||
                existing_trait != nullptr) {
                error_fmt(decl->loc, "duplicate type declaration '{}'", name);
                continue;
            }

            // Create the trait type
            Type* type = m_types.trait_type(name, decl);
            type->trait_info.type_params = decl->trait_decl.type_params;
            m_type_env.register_trait_type(name, type);

            // Define in global scope
            m_symbols.define(SymbolKind::Trait, name, type, decl->loc, decl);
        }
    }
}

// Pass 2: Resolve type members

void SemanticAnalyzer::resolve_type_members(Program* program) {
    for (auto* decl : program->declarations) {
        if (!decl) continue;
        switch (decl->kind) {
            case AstKind::DeclStruct:      resolve_struct_members(decl); break;
            case AstKind::DeclEnum:        resolve_enum_members(decl); break;
            case AstKind::DeclFun:         resolve_fun_signature(decl); break;
            case AstKind::DeclVar:         resolve_global_var(decl); break;
            case AstKind::DeclConstructor: resolve_constructor_member(decl); break;
            case AstKind::DeclDestructor:  resolve_destructor_member(decl); break;
            case AstKind::DeclMethod:      resolve_method_member(decl); break;
            case AstKind::DeclTrait:       resolve_trait_parent(decl); break;
            default: break;
        }
    }

    detect_mutual_struct_recursion(program);
    generate_synthetic_destructors(program);

    // Now validate all trait implementations
    validate_trait_implementations();
}

void SemanticAnalyzer::resolve_struct_members(Decl* decl) {
    StructDecl& struct_decl = decl->struct_decl;

    // Skip generic struct templates - they have unresolved type params
    if (struct_decl.type_params.size() > 0) return;

    Type* type = m_type_env.named_type_by_name(struct_decl.name);

    // Resolve parent type
    if (!struct_decl.parent_name.empty()) {
        Type* parent = m_type_env.named_type_by_name(struct_decl.parent_name);
        if (!parent) {
            error_fmt(decl->loc, "unknown parent type '{}'", struct_decl.parent_name);
        } else if (parent->kind != TypeKind::Struct) {
            error_fmt(decl->loc, "parent type '{}' is not a struct", struct_decl.parent_name);
        } else {
            type->struct_info.parent = parent;
        }
    }

    // Resolve field types
    Vector<FieldInfo> fields;

    // First, inherit parent fields
    if (type->struct_info.parent) {
        Type* parent = type->struct_info.parent;
        for (const auto& field : parent->struct_info.fields) {
            fields.push_back(field);
        }
    }

    // Then add own fields
    for (auto& field : struct_decl.fields) {
        Type* field_type = resolve_type_expr(field.type);
        if (!field_type) {
            field_type = m_types.error_type();
        }

        // Check: ref types cannot be used in struct fields (prevents cycles)
        if (field_type->kind == TypeKind::Ref) {
            error_fmt(field.loc, "'ref' types cannot be used in struct fields");
        }

        FieldInfo info;
        info.name = field.name;
        info.type = field_type;
        info.is_pub = field.is_pub;
        info.index = static_cast<u32>(fields.size());
        info.slot_offset = 0;  // Will be computed below
        info.slot_count = 0;   // Will be computed below
        fields.push_back(info);
    }

    // Check for direct self-referencing cycles (infinite size)
    bool has_cycle = false;
    for (u32 fi = 0; fi < struct_decl.fields.size(); fi++) {
        auto& field_info = fields[fields.size() - struct_decl.fields.size() + fi];
        if (field_info.type->kind == TypeKind::Struct &&
            field_info.type == type) {
            error_fmt(struct_decl.fields[fi].loc,
                "recursive struct type '{}' has infinite size; "
                "use 'uniq {}' for indirection",
                field_info.type->struct_info.name,
                field_info.type->struct_info.name);
            has_cycle = true;
        }
    }

    if (has_cycle) return;

    // Compute field slot offsets for regular fields
    u32 current_slot = 0;
    for (auto& field_info : fields) {
        field_info.slot_count = get_type_slot_count(field_info.type);
        field_info.slot_offset = current_slot;
        current_slot += field_info.slot_count;
    }

    // Process when clauses (tagged unions)
    Vector<WhenClauseInfo> when_clauses;
    resolve_when_clauses(struct_decl.when_clauses, fields, when_clauses, current_slot);

    type->struct_info.slot_count = current_slot;

    type->struct_info.fields = m_allocator.alloc_span(fields);
    type->struct_info.when_clauses = m_allocator.alloc_span(when_clauses);

    // Initialize empty constructor/destructor/method lists
    type->struct_info.constructors = Span<ConstructorInfo>(nullptr, 0);
    type->struct_info.destructors = Span<DestructorInfo>(nullptr, 0);
    type->struct_info.methods = Span<MethodInfo>(nullptr, 0);
}

void SemanticAnalyzer::resolve_enum_members(Decl* decl) {
    EnumDecl& ed = decl->enum_decl;
    Type* type = m_type_env.named_type_by_name(ed.name);

    // Define enum variants in global scope (accessible as EnumName::Variant)
    i64 next_value = 0;
    for (auto& v : ed.variants) {

        i64 value = next_value;
        if (v.value) {
            // Analyze the value expression
            Type* vtype = analyze_expr(v.value);
            if (vtype && !vtype->is_error() && !vtype->is_integer() && !vtype->is_int_literal()) {
                error_fmt(v.loc, "enum variant value must be an integer");
            }
            // For simplicity, we require compile-time integer literals
            if (v.value->kind == AstKind::ExprLiteral) {
                LiteralKind lk = v.value->literal.literal_kind;
                if (lk == LiteralKind::I32 || lk == LiteralKind::I64 ||
                    lk == LiteralKind::U32 || lk == LiteralKind::U64) {
                    value = v.value->literal.int_value;
                }
            }
        }

        // Create a qualified name for the variant (e.g., "EnumName::VariantName")
        // But for lookup purposes, we store it separately
        m_symbols.define_enum_variant(v.name, type, v.loc, value);

        next_value = value + 1;
    }
}

void SemanticAnalyzer::resolve_fun_signature(Decl* decl) {
    // Skip generic function templates - they have unresolved type params
    FunDecl& fun_decl = decl->fun_decl;
    if (fun_decl.type_params.size() > 0) return;

    // Register function in global scope (for forward references)
    // Resolve parameter types
    Vector<Type*> param_types;
    for (const auto& param : fun_decl.params) {
        Type* ptype = resolve_type_expr(param.type);
        if (!ptype) ptype = m_types.error_type();
        param_types.push_back(ptype);
    }

    // Resolve return type
    Type* return_type = fun_decl.return_type ? resolve_type_expr(fun_decl.return_type) : m_types.void_type();
    if (!return_type) return_type = m_types.error_type();

    // For coroutine functions, create a function-specific coroutine type
    // so that method calls (.resume(), .done()) can be resolved to the
    // correct mangled function names.
    if (return_type && return_type->is_coroutine()) {
        return_type = m_types.coroutine_type_for_func(
            return_type->coro_info.yield_type, fun_decl.name);
        populate_coro_methods(return_type);
    }

    // Create function type
    Type* func_type = m_types.function_type(
        m_allocator.alloc_span(param_types), return_type);

    // Define function in global scope
    Symbol* sym = m_symbols.define(SymbolKind::Function, fun_decl.name, func_type, decl->loc, decl);
    sym->is_pub = fun_decl.is_pub;
}

void SemanticAnalyzer::resolve_global_var(Decl* decl) {
    VarDecl& var_decl = decl->var_decl;

    Type* var_type = nullptr;
    if (var_decl.type) {
        var_type = resolve_type_expr(var_decl.type);
    }

    if (var_decl.initializer) {
        Type* init_type = analyze_expr(var_decl.initializer);
        if (var_type && coerce_generic_template_ref(var_decl.initializer, var_type)) {
            init_type = var_decl.initializer->resolved_type;
        }
        if (!var_type) {
            // Type inference
            var_type = init_type;
            if (var_type->is_int_literal()) {
                var_type = m_types.i32_type();
                m_checker.coerce_int_literal(var_decl.initializer, var_type);
            }
        } else if (!m_checker.check_assignable(var_type, init_type, decl->loc)) {
            // Error already reported by check_assignable
        } else {
            m_checker.coerce_int_literal(var_decl.initializer, var_type);
        }
    } else if (!var_type) {
        error(decl->loc, "variable declaration requires type annotation or initializer");
        var_type = m_types.error_type();
    }

    var_decl.resolved_type = var_type;
    Symbol* sym = m_symbols.define(SymbolKind::Variable, var_decl.name, var_type, decl->loc, decl);
    sym->is_pub = var_decl.is_pub;
}

void SemanticAnalyzer::resolve_constructor_member(Decl* decl) {
    ConstructorDecl& constructor_decl = decl->constructor_decl;
    if (generics().is_generic_struct(constructor_decl.struct_name)) {
        generics().register_generic_struct_constructor(constructor_decl.struct_name, decl);
        return;
    }
    analyze_constructor_decl(decl);
}

void SemanticAnalyzer::resolve_destructor_member(Decl* decl) {
    DestructorDecl& destructor_decl = decl->destructor_decl;
    if (generics().is_generic_struct(destructor_decl.struct_name)) {
        generics().register_generic_struct_destructor(destructor_decl.struct_name, decl);
        return;
    }
    analyze_destructor_decl(decl);
}

void SemanticAnalyzer::resolve_method_member(Decl* decl) {
    MethodDecl& method_decl = decl->method_decl;

    // Check if struct_name is actually a trait name
    Type* trait_lookup = m_type_env.trait_type_by_name(method_decl.struct_name);
    if (trait_lookup && method_decl.trait_name.empty()) {
        // This is a trait method declaration (fun TraitName.method(...))
        analyze_trait_method_decl(decl, trait_lookup);
    }
    else if (!method_decl.trait_name.empty()) {
        // This is a trait implementation (fun Type.method(...) for Trait<Args>)
        Type* struct_type_lookup = m_type_env.named_type_by_name(method_decl.struct_name);
        if (!struct_type_lookup) {
            error_fmt(decl->loc, "method for unknown type '{}'", method_decl.struct_name);
        } else if (struct_type_lookup->kind != TypeKind::Struct) {
            error_fmt(decl->loc, "'{}' is not a struct type", method_decl.struct_name);
        } else {
            Type* impl_trait = m_type_env.trait_type_by_name(method_decl.trait_name);
            if (!impl_trait) {
                error_fmt(decl->loc, "unknown trait '{}'", method_decl.trait_name);
            } else {
                Type* trait_type = impl_trait;
                TraitTypeInfo& trait_type_info = trait_type->trait_info;

                // Resolve the `for Trait<Args>` type args of this impl.
                Span<Type*> resolved_trait_type_args;
                if (!resolve_trait_impl_type_args(method_decl, trait_type_info,
                                                  struct_type_lookup, decl->loc,
                                                  resolved_trait_type_args)) {
                    return;
                }

                m_pending_trait_impls.push_back({decl, struct_type_lookup, trait_type, resolved_trait_type_args});
            }
        }
    }
    else {
        // Regular method (no trait involvement)
        // Check if struct_name is a generic struct template
        if (generics().is_generic_struct(method_decl.struct_name)) {
            generics().register_generic_struct_method(method_decl.struct_name, decl);
            return;  // Skip normal method analysis; handled in worklist
        }
        analyze_method_decl(decl);
    }
}

void SemanticAnalyzer::resolve_trait_parent(Decl* decl) {
    TraitDecl& trait_decl = decl->trait_decl;
    Type* trait_type = m_type_env.trait_type_by_name(trait_decl.name);

    // Resolve parent trait
    if (!trait_decl.parent_name.empty()) {
        Type* parent_trait = m_type_env.trait_type_by_name(trait_decl.parent_name);
        if (!parent_trait) {
            error_fmt(decl->loc, "unknown parent trait '{}'", trait_decl.parent_name);
        } else if (parent_trait == trait_type) {
            error_fmt(decl->loc, "trait '{}' cannot inherit from itself", trait_decl.name);
        } else {
            trait_type->trait_info.parent = parent_trait;
        }
    }
}

void SemanticAnalyzer::detect_mutual_struct_recursion(Program* program) {
    // After all structs are resolved, recompute each struct's expected
    // slot_count. If it differs from the stored value, a field referenced a
    // not-yet-resolved struct (mutual recursion or invalid forward reference
    // of a value type).
    for (auto* decl : program->declarations) {
        if (!decl || decl->kind != AstKind::DeclStruct) continue;
        if (decl->struct_decl.type_params.size() > 0) continue;

        Type* type = m_type_env.named_type_by_name(decl->struct_decl.name);
        if (!type || !type->is_struct()) continue;

        u32 recomputed_slots = 0;
        for (const auto& field : type->struct_info.fields) {
            recomputed_slots += get_type_slot_count(field.type);
        }

        if (recomputed_slots != type->struct_info.slot_count) {
            // Find the offending field and report error
            for (u32 fi = 0; fi < decl->struct_decl.fields.size(); fi++) {
                Type* field_type = type->struct_info.fields[fi].type;
                if (field_type && field_type->kind == TypeKind::Struct) {
                    // Check if this field's slot count changed
                    u32 current_field_slots = get_type_slot_count(field_type);
                    if (current_field_slots != type->struct_info.fields[fi].slot_count) {
                        error_fmt(decl->struct_decl.fields[fi].loc,
                            "recursive struct type '{}' has infinite size; "
                            "use 'uniq {}' for indirection",
                            field_type->struct_info.name,
                            field_type->struct_info.name);
                    }
                }
            }
        }
    }
}

void SemanticAnalyzer::generate_synthetic_destructors(Program* program) {
    // Generate synthetic default destructors for structs that have fields
    // needing cleanup. A field needs cleanup if:
    //   - It is a uniq reference (needs destructor call + memory free)
    //   - It is a value-type struct whose type has a default destructor
    // Use a fixpoint loop because adding a synthetic destructor to Inner
    // may cause Outer (which embeds Inner) to also need one.
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto* decl : program->declarations) {
            if (!decl || decl->kind != AstKind::DeclStruct) continue;
            if (decl->struct_decl.type_params.size() > 0) continue;

            Type* struct_type = m_type_env.named_type_by_name(decl->struct_decl.name);
            if (!struct_type || !struct_type->is_struct()) continue;

            StructTypeInfo& struct_info = struct_type->struct_info;

            // Check if struct already has a default destructor
            bool has_default_dtor = false;
            for (const auto& dtor : struct_info.destructors) {
                if (dtor.name.empty()) {
                    has_default_dtor = true;
                    break;
                }
            }
            if (has_default_dtor) continue;

            // Check if any field needs cleanup (regular fields or variant fields)
            bool needs_cleanup = false;
            for (const auto& field : struct_info.fields) {
                if (!field.type) continue;
                if (field.type->noncopyable()) {
                    needs_cleanup = true;
                    break;
                }
            }
            if (!needs_cleanup) {
                // Also check variant fields in when clauses
                for (const auto& clause : struct_info.when_clauses) {
                    for (const auto& variant : clause.variants) {
                        for (const auto& variant_field : variant.fields) {
                            if (variant_field.type && variant_field.type->noncopyable()) {
                                needs_cleanup = true;
                                break;
                            }
                        }
                        if (needs_cleanup) break;
                    }
                    if (needs_cleanup) break;
                }
            }
            if (!needs_cleanup) continue;

            // Add synthetic default destructor (decl = nullptr marks it as synthetic)
            DestructorInfo synthetic_dtor;
            synthetic_dtor.name = StringView();
            synthetic_dtor.param_types = Span<Type*>();
            synthetic_dtor.decl = nullptr;
            append_destructor(struct_info, synthetic_dtor);
            changed = true;
        }
    }
}

void SemanticAnalyzer::resolve_when_clauses(Span<WhenFieldDecl> when_decls,
                                            Vector<FieldInfo>& fields,
                                            Vector<WhenClauseInfo>& when_clauses,
                                            u32& current_slot) {
    for (auto& wfd : when_decls) {
        // Resolve discriminant type - must be enum
        Type* disc_type = resolve_type_expr(wfd.discriminant_type);
        if (!disc_type || disc_type->is_error()) {
            disc_type = m_types.error_type();
        } else if (!disc_type->is_enum()) {
            error_fmt(wfd.loc, "when discriminant must be an enum type");
            disc_type = m_types.error_type();
        }

        // Add discriminant as a regular field
        FieldInfo disc_field;
        disc_field.name = wfd.discriminant_name;
        disc_field.type = disc_type;
        disc_field.is_pub = true;  // Discriminant is accessible
        disc_field.index = static_cast<u32>(fields.size());
        disc_field.slot_offset = current_slot;
        disc_field.slot_count = get_type_slot_count(disc_type);
        fields.push_back(disc_field);

        u32 disc_slot_offset = current_slot;
        current_slot += disc_field.slot_count;
        u32 union_slot_offset = current_slot;

        // Process each case
        u32 max_variant_slots = 0;
        Vector<VariantInfo> variants;

        for (auto& case_decl : wfd.cases) {
            // Validate case names are enum variants
            i64 discriminant_value = 0;
            for (const auto& case_name : case_decl.case_names) {
                Symbol* sym = m_symbols.lookup(case_name);
                if (!sym || sym->kind != SymbolKind::EnumVariant) {
                    error_fmt(case_decl.loc, "'{}' is not a known enum variant", case_name);
                } else {
                    discriminant_value = sym->enum_variant.value;
                }
            }

            // Process variant fields
            Vector<VariantFieldInfo> var_fields;
            u32 var_slot = 0;
            for (auto& field : case_decl.fields) {
                Type* field_type = resolve_type_expr(field.type);
                if (!field_type) field_type = m_types.error_type();

                u32 slot_count = get_type_slot_count(field_type);
                VariantFieldInfo variant_field_info;
                variant_field_info.name = field.name;
                variant_field_info.type = field_type;
                variant_field_info.is_pub = field.is_pub;
                variant_field_info.slot_offset = var_slot;
                variant_field_info.slot_count = slot_count;
                var_fields.push_back(variant_field_info);
                var_slot += slot_count;
            }

            // Create VariantInfo for each case name
            for (const auto& case_name : case_decl.case_names) {
                Symbol* sym = m_symbols.lookup(case_name);
                i64 value = sym ? sym->enum_variant.value : 0;

                VariantInfo vi;
                vi.case_name = case_name;
                vi.discriminant_value = value;
                vi.fields = m_allocator.alloc_span(var_fields);
                vi.variant_slot_count = var_slot;
                variants.push_back(vi);
            }

            max_variant_slots = var_slot > max_variant_slots ? var_slot : max_variant_slots;
        }

        // Create WhenClauseInfo
        WhenClauseInfo clause;
        clause.discriminant_name = wfd.discriminant_name;
        clause.discriminant_type = disc_type;
        clause.discriminant_slot_offset = disc_slot_offset;
        clause.union_slot_offset = union_slot_offset;
        clause.union_slot_count = max_variant_slots;
        clause.variants = m_allocator.alloc_span(variants);
        when_clauses.push_back(clause);

        current_slot += max_variant_slots;
    }
}

// Pass 3: Analyze function bodies

void SemanticAnalyzer::analyze_function_bodies(Program* program) {
    for (auto* decl : program->declarations) {
        if (!decl) continue;

        if (decl->kind == AstKind::DeclFun) {
            if (decl->fun_decl.type_params.size() > 0) {
                // Phase B: Check bounded template bodies against declared trait bounds
                analyze_generic_template_body(decl);
                continue;
            }
            analyze_fun_decl(decl);
        }
        else if (decl->kind == AstKind::DeclStruct) {
            // Skip generic struct templates
            if (decl->struct_decl.type_params.size() > 0) continue;

            // Analyze struct methods
            StructDecl& struct_decl = decl->struct_decl;
            Type* struct_type = m_type_env.named_type_by_name(struct_decl.name);

            m_symbols.push_struct_scope(struct_type);

            // Define fields in struct scope
            for (auto& field_info : struct_type->struct_info.fields) {
                m_symbols.define_field(field_info.name, field_info.type, decl->loc, field_info.index, field_info.is_pub);
            }

            // Analyze methods
            for (auto* method : struct_decl.methods) {
                if (!method) continue;

                // Create a temporary Decl wrapper for the method
                Decl* method_decl = m_allocator.emplace<Decl>();
                method_decl->kind = AstKind::DeclFun;
                method_decl->loc = method->body ? method->body->loc : decl->loc;
                method_decl->fun_decl = *method;

                analyze_fun_decl(method_decl);
            }

            m_symbols.pop_scope();
        }
        else if (decl->kind == AstKind::DeclConstructor) {
            ConstructorDecl& constructor_decl = decl->constructor_decl;
            // Skip generic struct constructor templates (handled in worklist)
            if (generics().is_generic_struct(constructor_decl.struct_name)) continue;
            Type* ctor_struct = m_type_env.named_type_by_name(constructor_decl.struct_name);
            if (ctor_struct) {
                analyze_constructor_body(decl, ctor_struct);
            }
        }
        else if (decl->kind == AstKind::DeclDestructor) {
            DestructorDecl& destructor_decl = decl->destructor_decl;
            // Skip generic struct destructor templates (handled in worklist)
            if (generics().is_generic_struct(destructor_decl.struct_name)) continue;
            Type* dtor_struct = m_type_env.named_type_by_name(destructor_decl.struct_name);
            if (dtor_struct) {
                analyze_destructor_body(decl, dtor_struct);
            }
        }
        else if (decl->kind == AstKind::DeclMethod) {
            MethodDecl& method_decl = decl->method_decl;
            // Skip trait method declarations (struct_name is a trait)
            if (m_type_env.trait_type_by_name(method_decl.struct_name) && method_decl.trait_name.empty()) {
                continue;
            }
            // Skip generic struct method templates (handled in worklist)
            if (generics().is_generic_struct(method_decl.struct_name)) {
                continue;
            }
            Type* method_struct = m_type_env.named_type_by_name(method_decl.struct_name);
            if (method_struct) {
                analyze_method_body(decl, method_struct);
            }
        }
    }

    // Also analyze synthetic (injected default method) bodies
    for (Decl* decl : m_synthetic_decls) {
        if (decl && decl->kind == AstKind::DeclMethod) {
            MethodDecl& method_decl = decl->method_decl;
            Type* synth_struct = m_type_env.named_type_by_name(method_decl.struct_name);
            if (synth_struct) {
                analyze_method_body(decl, synth_struct);
            }
        }
    }

    // Process pending generic instances (worklist loop)
    while (m_type_env.generics().has_pending_structs() || m_type_env.generics().has_pending_funs()) {
        if (too_many_errors()) return;

        // Process pending struct instances first (they create types that functions may use)
        if (m_type_env.generics().has_pending_structs()) {
            auto pending_structs = m_type_env.generics().take_pending_structs();
            for (auto* inst : pending_structs) {
                // Fields and MethodInfo may already be resolved eagerly
                // (via resolve_generic_struct_fields). Only resolve if not yet done.
                if (!inst->is_analyzed) {
                    resolve_generic_struct_fields(inst);
                }
            }

            // Analyze bodies (after ALL struct types and infos are registered)
            for (auto* inst : pending_structs) {
                // Analyze external method bodies
                for (Decl* method_decl : inst->instantiated_methods) {
                    analyze_method_body(method_decl, inst->concrete_type);
                }

                // Analyze constructor bodies
                for (Decl* ctor_decl : inst->instantiated_constructors) {
                    analyze_constructor_body(ctor_decl, inst->concrete_type);
                }

                // Analyze destructor bodies
                for (Decl* dtor_decl : inst->instantiated_destructors) {
                    analyze_destructor_body(dtor_decl, inst->concrete_type);
                }

                // Generate synthetic default destructor if struct has fields needing cleanup
                StructTypeInfo& concrete_info = inst->concrete_type->struct_info;
                bool needs_cleanup = false;
                for (const auto& field : concrete_info.fields) {
                    if (!field.type) continue;
                    if (field.type->noncopyable()) {
                        needs_cleanup = true;
                        break;
                    }
                }
                if (!needs_cleanup) {
                    for (const auto& clause : concrete_info.when_clauses) {
                        for (const auto& variant : clause.variants) {
                            for (const auto& variant_field : variant.fields) {
                                if (variant_field.type && variant_field.type->noncopyable()) {
                                    needs_cleanup = true;
                                    break;
                                }
                            }
                            if (needs_cleanup) break;
                        }
                        if (needs_cleanup) break;
                    }
                }
                if (needs_cleanup) {
                    DestructorInfo synthetic_dtor;
                    synthetic_dtor.name = StringView();
                    synthetic_dtor.param_types = Span<Type*>();
                    synthetic_dtor.decl = nullptr;
                    append_destructor(concrete_info, synthetic_dtor);
                }
            }
        }

        // Process pending function instances. Only handle instances whose
        // template was defined in THIS module — cross-module instances are
        // sidelined for the compiler's post-pass to drain in the right
        // module's context (re-queueing them here would infinite-loop the
        // outer worklist).
        if (m_type_env.generics().has_pending_funs()) {
            StringView this_module = m_program ? m_program->module_name : StringView{};
            auto pending_funs = m_type_env.generics().take_pending_funs();
            for (auto* inst : pending_funs) {
                bool owns_template =
                    inst->template_module.empty() ||
                    this_module.empty() ||
                    inst->template_module == this_module;
                if (owns_template) {
                    analyze_fun_decl(inst->instantiated_decl);
                    inst->is_analyzed = true;
                } else {
                    m_type_env.generics().sideline_cross_module_fun(inst);
                }
            }
        }
    }
}

// ===== Type Expression Resolution =====

void SemanticAnalyzer::resolve_generic_struct_fields(GenericStructInstance* inst) {
    if (inst->is_analyzed) return;

    m_type_env.register_named_type(inst->mangled_name, inst->concrete_type);

    StructDecl& struct_decl = inst->instantiated_decl->struct_decl;
    StructTypeInfo& struct_type_info = inst->concrete_type->struct_info;

    // Track for cycle detection
    m_resolving_structs.insert(inst->concrete_type);

    Vector<FieldInfo> fields;
    for (u32 fi = 0; fi < struct_decl.fields.size(); fi++) {
        Type* field_type = resolve_type_expr(struct_decl.fields[fi].type);
        if (!field_type) field_type = m_types.error_type();

        FieldInfo info;
        info.name = struct_decl.fields[fi].name;
        info.type = field_type;
        info.is_pub = struct_decl.fields[fi].is_pub;
        info.index = fi;
        info.slot_offset = 0;
        info.slot_count = 0;
        fields.push_back(info);
    }

    // Check for direct value-type cycles
    bool has_cycle = false;
    for (u32 fi = 0; fi < fields.size(); fi++) {
        if (fields[fi].type->kind == TypeKind::Struct &&
            m_resolving_structs.count(fields[fi].type)) {
            error_fmt(struct_decl.fields[fi].loc,
                "recursive struct type '{}' has infinite size; "
                "use 'uniq {}' for indirection",
                fields[fi].type->struct_info.name,
                fields[fi].type->struct_info.name);
            has_cycle = true;
        }
    }

    m_resolving_structs.erase(inst->concrete_type);

    if (has_cycle) {
        inst->is_analyzed = true;
        return;
    }

    // Compute slot offsets
    u32 slot_offset = 0;
    for (auto& field_info : fields) {
        field_info.slot_count = get_type_slot_count(field_info.type);
        field_info.slot_offset = slot_offset;
        slot_offset += field_info.slot_count;
    }

    struct_type_info.fields = m_allocator.alloc_span(fields);
    struct_type_info.slot_count = slot_offset;

    // Initialize empty constructor/destructor/method lists
    struct_type_info.constructors = Span<ConstructorInfo>(nullptr, 0);
    struct_type_info.destructors = Span<DestructorInfo>(nullptr, 0);
    struct_type_info.methods = Span<MethodInfo>(nullptr, 0);

    // Register ConstructorInfo for external constructors
    for (Decl* ctor_decl : inst->instantiated_constructors) {
        ConstructorDecl& ctor = ctor_decl->constructor_decl;
        Vector<Type*> param_types;
        for (const auto& param : ctor.params) {
            Type* ptype = resolve_type_expr(param.type);
            if (!ptype) ptype = m_types.error_type();
            param_types.push_back(ptype);
        }

        ConstructorInfo ctor_info;
        ctor_info.name = ctor.name;
        ctor_info.param_types = m_allocator.alloc_span(param_types);
        ctor_info.decl = ctor_decl;
        append_constructor(struct_type_info, ctor_info);
    }

    // Register DestructorInfo for external destructors
    for (Decl* dtor_decl : inst->instantiated_destructors) {
        DestructorDecl& dtor = dtor_decl->destructor_decl;
        Vector<Type*> param_types;
        for (const auto& param : dtor.params) {
            Type* ptype = resolve_type_expr(param.type);
            if (!ptype) ptype = m_types.error_type();
            param_types.push_back(ptype);
        }

        DestructorInfo dtor_info;
        dtor_info.name = dtor.name;
        dtor_info.param_types = m_allocator.alloc_span(param_types);
        dtor_info.decl = dtor_decl;
        append_destructor(struct_type_info, dtor_info);
    }

    // Register MethodInfo for external methods so call sites can resolve them
    for (Decl* method_decl : inst->instantiated_methods) {
        MethodDecl& method = method_decl->method_decl;

        Vector<Type*> param_types;
        for (const auto& param : method.params) {
            Type* ptype = resolve_type_expr(param.type);
            if (!ptype) ptype = m_types.error_type();
            param_types.push_back(ptype);
        }

        Type* return_type = method.return_type ? resolve_type_expr(method.return_type) : m_types.void_type();
        if (!return_type) return_type = m_types.error_type();

        MethodInfo method_info;
        method_info.name = method.name;
        method_info.param_types = m_allocator.alloc_span(param_types);
        method_info.return_type = return_type;
        method_info.decl = method_decl;
        append_method(struct_type_info, method_info);
    }

    // Generate synthetic default destructor if struct has fields needing cleanup
    bool needs_cleanup = false;
    for (const auto& field : struct_type_info.fields) {
        if (!field.type) continue;
        if (field.type->noncopyable()) {
            needs_cleanup = true;
            break;
        }
    }
    if (!needs_cleanup) {
        for (const auto& clause : struct_type_info.when_clauses) {
            for (const auto& variant : clause.variants) {
                for (const auto& variant_field : variant.fields) {
                    if (variant_field.type && variant_field.type->noncopyable()) {
                        needs_cleanup = true;
                        break;
                    }
                }
                if (needs_cleanup) break;
            }
            if (needs_cleanup) break;
        }
    }
    if (needs_cleanup) {
        DestructorInfo synthetic_dtor;
        synthetic_dtor.name = StringView();
        synthetic_dtor.param_types = Span<Type*>();
        synthetic_dtor.decl = nullptr;
        append_destructor(struct_type_info, synthetic_dtor);
    }

    inst->is_analyzed = true;
}

Type* SemanticAnalyzer::resolve_type_expr(TypeExpr* type_expr) {
    if (!type_expr) return nullptr;

    // Function type: fun(T1, T2) -> R
    if (type_expr->kind == TypeExprKind::Function) {
        Vector<Type*> params;
        for (auto* param_expr : type_expr->type_args) {
            Type* pt = resolve_type_expr(param_expr);
            if (!pt || pt->is_error()) return m_types.error_type();
            params.push_back(pt);
        }
        Type* ret = type_expr->return_type
            ? resolve_type_expr(type_expr->return_type)
            : m_types.void_type();
        if (!ret || ret->is_error()) return m_types.error_type();
        Type* base_type = m_types.function_type(m_allocator.alloc_span(params), ret);

        switch (type_expr->ref_kind) {
            case RefKind::Uniq: base_type = m_types.uniq_type(base_type); break;
            case RefKind::Ref:  base_type = m_types.ref_type(base_type); break;
            case RefKind::Weak: base_type = m_types.weak_type(base_type); break;
            case RefKind::None: break;
        }
        return base_type;
    }

    Type* base_type = nullptr;

    {
        // Check for Self type (used in trait method signatures)
        if (type_expr->name == "Self") {
            Type* struct_type = m_symbols.current_struct_type();
            if (struct_type) {
                base_type = struct_type;
            } else {
                error(type_expr->loc, "'Self' can only be used in struct/trait method context");
                return m_types.error_type();
            }
        }

        // Check if this is an active generic type parameter (during template body checking)
        if (!base_type && m_active_type_params.size() > 0) {
            for (u32 i = 0; i < m_active_type_params.size(); i++) {
                if (type_expr->name == m_active_type_params[i].name) {
                    base_type = m_types.type_param(m_active_type_params[i].name, i);
                    break;
                }
            }
        }

        // Check for built-in List<T> type
        if (!base_type && type_expr->name == "List") {
            if (type_expr->type_args.size() != 1) {
                error(type_expr->loc, "List requires exactly 1 type argument");
                return m_types.error_type();
            }
            Type* elem = resolve_type_expr(type_expr->type_args[0]);
            if (!elem || elem->is_error()) return m_types.error_type();
            base_type = m_types.list_type(elem);
            populate_list_methods(base_type);
        }

        // Check for built-in Coro<T> type
        if (!base_type && type_expr->name == "Coro") {
            if (type_expr->type_args.size() != 1) {
                error(type_expr->loc, "Coro requires exactly 1 type argument");
                return m_types.error_type();
            }
            Type* yield_type = resolve_type_expr(type_expr->type_args[0]);
            if (!yield_type || yield_type->is_error()) return m_types.error_type();
            base_type = m_types.coroutine_type(yield_type);
            populate_coro_methods(base_type);
        }

        // Check for built-in Map<K, V> type
        if (!base_type && type_expr->name == "Map") {
            if (type_expr->type_args.size() != 2) {
                error(type_expr->loc, "Map requires exactly 2 type arguments");
                return m_types.error_type();
            }
            Type* key_type = resolve_type_expr(type_expr->type_args[0]);
            Type* val_type = resolve_type_expr(type_expr->type_args[1]);
            if (!key_type || key_type->is_error()) return m_types.error_type();
            if (!val_type || val_type->is_error()) return m_types.error_type();
            if (!is_hashable_key_type(key_type)) {
                error(type_expr->loc, "Map key type must implement Hash");
                return m_types.error_type();
            }
            base_type = m_types.map_type(key_type, val_type);
            populate_map_methods(base_type);
        }

        // Check for generic struct instantiation: Box<i32>
        if (!base_type && type_expr->type_args.size() > 0 &&
            m_type_env.generics().is_generic_struct(type_expr->name)) {
            // Resolve type args to concrete types
            Vector<Type*> type_arg_types;
            for (auto* type_arg : type_expr->type_args) {
                Type* arg_type = resolve_type_expr(type_arg);
                if (!arg_type || arg_type->is_error()) return m_types.error_type();
                type_arg_types.push_back(arg_type);
            }

            // Validate arg count
            Decl* template_decl = m_type_env.generics().get_generic_struct_decl(type_expr->name);
            if (template_decl->struct_decl.type_params.size() != type_arg_types.size()) {
                error_fmt(type_expr->loc, "generic struct '{}' expects {} type arguments but got {}",
                         type_expr->name,
                         template_decl->struct_decl.type_params.size(),
                         type_arg_types.size());
                return m_types.error_type();
            }

            // Instantiate the generic struct
            Span<Type*> type_args = m_allocator.alloc_span(type_arg_types);
            StringView mangled = m_type_env.generics().instantiate_struct(type_expr->name, type_args);
            GenericStructInstance* inst = m_type_env.generics().find_struct_instance(mangled);
            base_type = inst->concrete_type;

            // Update the TypeExpr to use the mangled name so the IR builder can find it
            type_expr->name = mangled;
            type_expr->type_args = Span<TypeExpr*>();

            // Immediately resolve struct fields if not yet analyzed
            resolve_generic_struct_fields(inst);
        }

        if (!base_type) {
            // Look up by name
            base_type = m_types.primitive_by_name(type_expr->name);
        }

        if (!base_type) {
            base_type = m_type_env.named_type_by_name(type_expr->name);
        }

        if (!base_type) {
            error_fmt(type_expr->loc, "unknown type '{}'", type_expr->name);
            return m_types.error_type();
        }
    }

    // Apply reference modifiers
    switch (type_expr->ref_kind) {
        case RefKind::Uniq: base_type = m_types.uniq_type(base_type); break;
        case RefKind::Ref:  base_type = m_types.ref_type(base_type); break;
        case RefKind::Weak: base_type = m_types.weak_type(base_type); break;
        case RefKind::None: break;
    }

    return base_type;
}

// ===== Declaration Analysis =====

void SemanticAnalyzer::analyze_decl(Decl* decl) {
    if (!decl) return;

    switch (decl->kind) {
        case AstKind::DeclVar:
            analyze_var_decl(decl);
            break;
        case AstKind::DeclFun:
            analyze_fun_decl(decl);
            break;
        default:
            // Other declarations already handled in earlier passes
            break;
    }
}

void SemanticAnalyzer::analyze_var_decl(Decl* decl) {
    VarDecl& var_decl = decl->var_decl;

    // Check for duplicate in current scope
    if (m_symbols.lookup_local(var_decl.name)) {
        error_fmt(decl->loc, "redefinition of '{}'", var_decl.name);
        return;
    }

    Type* var_type = nullptr;
    if (var_decl.type) {
        var_type = resolve_type_expr(var_decl.type);
    }

    if (var_decl.initializer) {
        Type* init_type = analyze_expr(var_decl.initializer);
        // Resolve generic-template-ref initializers against the declared type
        // before assignability checking; updates init_type via resolved_type.
        if (var_type && coerce_generic_template_ref(var_decl.initializer, var_type)) {
            init_type = var_decl.initializer->resolved_type;
        }
        if (!var_type) {
            // Type inference
            var_type = init_type;
            if (var_type->is_nil()) {
                error(decl->loc, "cannot infer type from nil literal");
                var_type = m_types.error_type();
            } else if (var_type->is_int_literal()) {
                // Default unsuffixed integer literals to i32
                var_type = m_types.i32_type();
                m_checker.coerce_int_literal(var_decl.initializer, var_type);
            }
        } else if (!m_checker.check_assignable(var_type, init_type, decl->loc)) {
            // Error already reported
        } else {
            // Coerce int literals to the annotated type
            m_checker.coerce_int_literal(var_decl.initializer, var_type);
        }

        // Consume noncopyable source (field-move check + mark source as moved)
        if (var_type && var_type->noncopyable()) {
            consume_noncopyable(var_decl.initializer, decl->loc);
        }
    } else if (!var_type) {
        error(decl->loc, "variable declaration requires type annotation or initializer");
        var_type = m_types.error_type();
    }

    var_decl.resolved_type = var_type;
    Symbol* var_sym = m_symbols.define(SymbolKind::Variable, var_decl.name, var_type, decl->loc, decl);

    // Track owned variables for move semantics (uniq refs and value structs with destructors)
    if (var_type && var_type->noncopyable() && var_sym) {
        m_move_states[var_sym] = MoveState::Live;
    }
}

// ===== Move-State Tracking for Uniq Variables =====

void SemanticAnalyzer::merge_move_states(const MoveStateSnapshot& then_states,
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

void SemanticAnalyzer::merge_two_branches(const MoveStateSnapshot& pre_branch,
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

bool SemanticAnalyzer::merge_branch_snapshots(const Vector<MoveStateSnapshot>& snapshots,
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

bool SemanticAnalyzer::expr_references_name(Expr* expr, StringView name) const {
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

bool SemanticAnalyzer::loop_reassigns_var_first(Stmt* body, StringView var_name) const {
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

void SemanticAnalyzer::check_loop_cross_iteration_moves(
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
            error_fmt(loc,
                "variable '{}' is moved in the loop body and never reassigned; "
                "it would be used after move on the next iteration",
                sym ? sym->name : StringView());
        } else {
            error_fmt(loc,
                "variable '{}' may be moved in the loop body without being "
                "reassigned; it could be used after move on the next iteration",
                sym ? sym->name : StringView());
        }
    }
}

bool SemanticAnalyzer::check_not_moved(StringView name, SourceLocation loc) {
    Symbol* sym = m_symbols.lookup(name);
    if (!sym) return true;
    auto it = m_move_states.find(sym);
    if (it == m_move_states.end()) return true;  // Not tracked (not noncopyable)

    if (it->second == MoveState::Moved) {
        error_fmt(loc, "use of moved value '{}'", name);
        return false;
    }
    if (it->second == MoveState::MaybeValid) {
        error_fmt(loc, "use of possibly moved value '{}'", name);
        return false;
    }
    return true;
}

bool SemanticAnalyzer::check_not_field_move(Expr* expr, SourceLocation loc) {
    if (expr->kind != AstKind::ExprGet) return true;

    Type* field_type = expr->resolved_type;
    if (!field_type || !field_type->noncopyable()) return true;

    // Allow moving a noncopyable field out of a local value-struct variable,
    // including nested chains like `obj.inner.field` provided every link in the
    // chain is a value struct (no references). A reference type anywhere along
    // the chain breaks the rule: we can read through `uniq`/`ref`/`weak` but
    // can't take ownership of storage we don't own. After the move the root
    // variable is marked moved (unusable); the IR builder skips its scope-exit
    // destructor so the remaining noncopyable fields leak rather than
    // double-free.
    Expr* current = expr->get.object;
    while (current->kind == AstKind::ExprGet) {
        Type* parent_type = current->resolved_type;
        if (!parent_type || parent_type->is_reference() || !parent_type->is_struct()) {
            error(loc, "cannot move out of a struct field; consider borrowing with 'ref' instead");
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

    error(loc, "cannot move out of a struct field; consider borrowing with 'ref' instead");
    return false;
}

void SemanticAnalyzer::consume_noncopyable(Expr* expr, SourceLocation loc) {
    if (!expr) return;
    Type* type = expr->resolved_type;
    if (!type || !type->noncopyable()) return;

    // Look through parenthesization: `(p)` denotes the same storage as `p`
    // (is_lvalue() recurses through grouping the same way). A move whose source
    // is wrapped in a grouping must still mark the underlying variable moved;
    // otherwise `consume((p))` launders the move past the identifier check below
    // and leaves `p` Live — a use-after-move false negative.
    while (expr->kind == AstKind::ExprGrouping) {
        expr = expr->grouping.expr;
        if (!expr) return;
    }

    if (!check_not_field_move(expr, loc)) return;

    if (expr->kind == AstKind::ExprIdentifier) {
        mark_moved(expr->identifier.name);
    }
}

void SemanticAnalyzer::mark_moved(StringView name) {
    Symbol* sym = m_symbols.lookup(name);
    if (!sym) return;
    auto it = m_move_states.find(sym);
    if (it != m_move_states.end()) {
        it.value() = MoveState::Moved;
    }
}

void SemanticAnalyzer::mark_live(StringView name) {
    Symbol* sym = m_symbols.lookup(name);
    if (!sym) return;
    auto it = m_move_states.find(sym);
    if (it != m_move_states.end()) {
        it.value() = MoveState::Live;
    }
}

void SemanticAnalyzer::check_scope_exit_uniq_destructors(const Scope* scope, SourceLocation loc) {
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
            error_fmt(loc, "variable '{}' of type 'uniq {}' has only named destructors; "
                          "must be explicitly deleted with 'delete {}.name(args)' before scope exit",
                      sym->name, struct_info.name, sym->name);
        }
    }
}

void SemanticAnalyzer::check_all_scopes_uniq_destructors(SourceLocation loc, ScopeKind stop_kind) {
    Scope* scope = m_symbols.current_scope();
    while (scope) {
        check_scope_exit_uniq_destructors(scope, loc);
        if (scope->kind == stop_kind) break;
        scope = scope->parent;
    }
}

void SemanticAnalyzer::analyze_fun_decl(Decl* decl) {
    FunDecl& fun_decl = decl->fun_decl;

    // Native functions don't have bodies
    if (fun_decl.is_native) return;
    if (!fun_decl.body) return;

    // Resolve return type
    Type* return_type = fun_decl.return_type ? resolve_type_expr(fun_decl.return_type) : m_types.void_type();

    // Detect coroutine function (returns Coro<T>). These guards restore the
    // outer coroutine context and move-state map when the function returns.
    bool is_coroutine = return_type && return_type->is_coroutine();
    ScopedValue coro_guard(m_in_coroutine);
    ScopedValue yield_guard(m_coro_yield_type);
    if (is_coroutine) {
        m_in_coroutine = true;
        m_coro_yield_type = return_type->coro_info.yield_type;
    }

    // Reset move states for this function body (restored on exit by the guard).
    ScopedValue move_states_guard(m_move_states);
    m_move_states.clear();

    // Push function scope
    m_symbols.push_function_scope(return_type);

    // Define parameters
    for (u32 i = 0; i < fun_decl.params.size(); i++) {
        Param& p = fun_decl.params[i];
        Type* ptype = resolve_type_expr(p.type);
        if (!ptype) ptype = m_types.error_type();
        p.resolved_type = ptype;

        // Check for duplicate parameter names
        if (m_symbols.lookup_local(p.name)) {
            error_fmt(p.loc, "duplicate parameter name '{}'", p.name);
        } else {
            m_symbols.define_parameter(p.name, ptype, p.loc, i);
        }

        // Track owned parameters as Live (uniq refs and value structs with destructors)
        if (ptype && ptype->noncopyable()) {
            Symbol* param_sym = m_symbols.lookup(p.name);
            if (param_sym) m_move_states[param_sym] = MoveState::Live;
        }
    }

    // Analyze body
    analyze_stmt(fun_decl.body);

    // Check return paths (simplified - doesn't track all paths)
    // A more complete implementation would track control flow
    if (!is_coroutine && !return_type->is_void() && !return_type->is_error()) {
        // For now, we just warn if there's no return at all
        // A full implementation would check all paths
    }

    check_scope_exit_uniq_destructors(m_symbols.current_scope(), decl->loc);
    m_symbols.pop_scope();
    // coro_guard / yield_guard / move_states_guard restore on return.
}

void SemanticAnalyzer::analyze_struct_decl(Decl* decl) {
    // Struct declarations are handled in earlier passes
}

void SemanticAnalyzer::analyze_enum_decl(Decl* decl) {
    // Enum declarations are handled in earlier passes
}

// Helper to extract the last component of a dotted module path
// e.g., "math.vec2" -> "vec2", "math" -> "math"
static StringView get_last_path_component(StringView path) {
    const char* last_dot = nullptr;
    for (u32 i = 0; i < path.size(); i++) {
        if (path.data()[i] == '.') {
            last_dot = path.data() + i;
        }
    }
    if (last_dot) {
        return StringView(last_dot + 1,
            static_cast<u32>(path.data() + path.size() - last_dot - 1));
    }
    return path;  // No dot - return whole path
}

void SemanticAnalyzer::analyze_import_decl(Decl* decl) {
    ImportDecl& imp = decl->import_decl;

    // Look up the module by the full path
    ModuleInfo* module = m_modules.find_module(imp.module_path);
    if (!module) {
        error_fmt(decl->loc, "unknown module '{}'", imp.module_path);
        return;
    }

    if (imp.is_from_import) {
        // from math.vec2 import sin, cos;
        for (auto& name : imp.names) {

            // Find the export in the module
            const ModuleExport* exp = m_modules.find_export(module, name.name);
            if (!exp) {
                error_fmt(name.loc, "module '{}' has no export '{}'",
                         imp.module_path, name.name);
                continue;
            }

            // Check visibility
            if (!exp->is_pub) {
                error_fmt(name.loc, "'{}' is not public in module '{}'",
                         name.name, imp.module_path);
                continue;
            }

            // Determine local name (use alias if present)
            StringView local_name = name.alias.empty() ? name.name : name.alias;

            // Check for duplicate symbol
            if (m_symbols.lookup_local(local_name)) {
                error_fmt(name.loc, "redefinition of '{}'",
                         local_name);
                continue;
            }

            // Register the imported symbol based on its kind
            if (exp->kind == ExportKind::Function) {
                m_symbols.define_imported_function(
                    local_name, exp->type, name.loc,
                    imp.module_path, exp->name, exp->index, exp->is_native);
            } else if (exp->kind == ExportKind::Enum) {
                // Define the enum type
                m_symbols.define(SymbolKind::Enum, local_name, exp->type, name.loc, exp->decl);

                // Also register enum variants so EnumName::Variant works
                if (exp->type && exp->type->kind == TypeKind::Enum && exp->type->enum_info.decl) {
                    EnumDecl& enum_decl = exp->type->enum_info.decl->enum_decl;
                    i64 next_value = 0;
                    for (auto& variant : enum_decl.variants) {
                        i64 value = next_value;
                        if (variant.value && variant.value->kind == AstKind::ExprLiteral) {
                            LiteralKind lk = variant.value->literal.literal_kind;
                            if (lk == LiteralKind::I32 || lk == LiteralKind::I64 ||
                                lk == LiteralKind::U32 || lk == LiteralKind::U64) {
                                value = variant.value->literal.int_value;
                            }
                        }
                        m_symbols.define_enum_variant(variant.name, exp->type, name.loc, value);
                        next_value = value + 1;
                    }
                }
            } else {
                // Structs
                m_symbols.define(SymbolKind::Struct, local_name, exp->type, name.loc, exp->decl);
            }
        }
    } else {
        // import math.vec2;
        // Register the module under the LAST component of the path for qualified access
        // e.g., "math.vec2" registers as "vec2", so you can use vec2.add()
        StringView local_name = get_last_path_component(imp.module_path);
        m_symbols.define_module(local_name, module, decl->loc);
    }
}

void SemanticAnalyzer::analyze_constructor_decl(Decl* decl) {
    ConstructorDecl& constructor_decl = decl->constructor_decl;

    // Look up the struct type
    Type* struct_type = m_type_env.named_type_by_name(constructor_decl.struct_name);
    if (!struct_type) {
        error_fmt(decl->loc, "constructor for unknown struct '{}'",
                 constructor_decl.struct_name);
        return;
    }

    if (struct_type->kind != TypeKind::Struct) {
        error_fmt(decl->loc, "'{}' is not a struct type",
                 constructor_decl.struct_name);
        return;
    }

    // Check for duplicate constructor names
    for (const auto& ctor : struct_type->struct_info.constructors) {
        if (ctor.name == constructor_decl.name) {
            if (constructor_decl.name.empty()) {
                error_fmt(decl->loc, "duplicate default constructor for struct '{}'",
                         constructor_decl.struct_name);
            } else {
                error_fmt(decl->loc, "duplicate constructor '{}' for struct '{}'",
                         constructor_decl.name, constructor_decl.struct_name);
            }
            return;
        }
    }

    // Resolve parameter types
    Vector<Type*> param_types;
    for (const auto& param : constructor_decl.params) {
        Type* ptype = resolve_type_expr(param.type);
        if (!ptype) ptype = m_types.error_type();
        param_types.push_back(ptype);
    }

    // Create constructor info
    ConstructorInfo ctor_info;
    ctor_info.name = constructor_decl.name;
    ctor_info.param_types = m_allocator.alloc_span(param_types);
    ctor_info.decl = decl;

    // Add to struct's constructor list
    append_constructor(struct_type->struct_info, ctor_info);
}

void SemanticAnalyzer::analyze_destructor_decl(Decl* decl) {
    DestructorDecl& destructor_decl = decl->destructor_decl;

    // Look up the struct type
    Type* struct_type = m_type_env.named_type_by_name(destructor_decl.struct_name);
    if (!struct_type) {
        error_fmt(decl->loc, "destructor for unknown struct '{}'",
                 destructor_decl.struct_name);
        return;
    }

    if (struct_type->kind != TypeKind::Struct) {
        error_fmt(decl->loc, "'{}' is not a struct type",
                 destructor_decl.struct_name);
        return;
    }

    // Check for duplicate destructor names
    for (const auto& dtor : struct_type->struct_info.destructors) {
        if (dtor.name == destructor_decl.name) {
            if (destructor_decl.name.empty()) {
                error_fmt(decl->loc, "duplicate default destructor for struct '{}'",
                         destructor_decl.struct_name);
            } else {
                error_fmt(decl->loc, "duplicate destructor '{}' for struct '{}'",
                         destructor_decl.name, destructor_decl.struct_name);
            }
            return;
        }
    }

    // Resolve parameter types
    Vector<Type*> param_types;
    for (const auto& param : destructor_decl.params) {
        Type* ptype = resolve_type_expr(param.type);
        if (!ptype) ptype = m_types.error_type();
        param_types.push_back(ptype);
    }

    // Create destructor info
    DestructorInfo dtor_info;
    dtor_info.name = destructor_decl.name;
    dtor_info.param_types = m_allocator.alloc_span(param_types);
    dtor_info.decl = decl;

    // Add to struct's destructor list
    append_destructor(struct_type->struct_info, dtor_info);
}

void SemanticAnalyzer::analyze_method_decl(Decl* decl) {
    MethodDecl& method_decl = decl->method_decl;

    // Look up the struct type
    Type* struct_type = m_type_env.named_type_by_name(method_decl.struct_name);
    if (!struct_type) {
        error_fmt(decl->loc, "method for unknown struct '{}'",
                 method_decl.struct_name);
        return;
    }

    if (struct_type->kind != TypeKind::Struct) {
        error_fmt(decl->loc, "'{}' is not a struct type",
                 method_decl.struct_name);
        return;
    }

    // Check for duplicate method names
    for (const auto& method : struct_type->struct_info.methods) {
        if (method.name == method_decl.name) {
            error_fmt(decl->loc, "duplicate method '{}' for struct '{}'",
                     method_decl.name, method_decl.struct_name);
            return;
        }
    }

    // Resolve parameter types
    Vector<Type*> param_types;
    for (const auto& param : method_decl.params) {
        Type* ptype = resolve_type_expr(param.type);
        if (!ptype) ptype = m_types.error_type();
        param_types.push_back(ptype);
    }

    // Resolve return type
    Type* return_type = method_decl.return_type ? resolve_type_expr(method_decl.return_type) : m_types.void_type();
    if (!return_type) return_type = m_types.error_type();

    // Create method info
    MethodInfo method_info;
    method_info.name = method_decl.name;
    method_info.param_types = m_allocator.alloc_span(param_types);
    method_info.return_type = return_type;
    method_info.decl = decl;

    // Add to struct's method list
    append_method(struct_type->struct_info, method_info);
}

void SemanticAnalyzer::analyze_member_body(Decl* decl, Type* struct_type,
                                            Span<Param> params, Stmt* body,
                                            Type* return_type) {
    if (!body) return;

    // Reset move states for this function body (restored on exit by the guard).
    ScopedValue move_states_guard(m_move_states);
    m_move_states.clear();

    // Push struct scope so 'self' and fields are accessible
    m_symbols.push_struct_scope(struct_type);

    // Define fields in scope
    for (auto& field_info : struct_type->struct_info.fields) {
        m_symbols.define_field(field_info.name, field_info.type, decl->loc, field_info.index, field_info.is_pub);
    }

    // Push function scope with return type
    m_symbols.push_function_scope(return_type);

    // Define parameters
    for (u32 i = 0; i < params.size(); i++) {
        Param& p = params[i];
        Type* ptype = resolve_type_expr(p.type);
        if (!ptype) ptype = m_types.error_type();

        if (m_symbols.lookup_local(p.name)) {
            error_fmt(p.loc, "duplicate parameter name '{}'", p.name);
        } else {
            m_symbols.define_parameter(p.name, ptype, p.loc, i);
        }

        // Track owned parameters as Live (uniq refs and value structs with destructors)
        if (ptype && ptype->noncopyable()) {
            Symbol* param_sym = m_symbols.lookup(p.name);
            if (param_sym) m_move_states[param_sym] = MoveState::Live;
        }
    }

    // Analyze body
    analyze_stmt(body);

    check_scope_exit_uniq_destructors(m_symbols.current_scope(), decl->loc);
    m_symbols.pop_scope();  // function scope
    m_symbols.pop_scope();  // struct scope
    // move_states_guard restores the outer function's move states on return.
}

void SemanticAnalyzer::analyze_constructor_body(Decl* decl, Type* struct_type) {
    auto& cd = decl->constructor_decl;
    analyze_member_body(decl, struct_type, cd.params, cd.body, m_types.void_type());
}

void SemanticAnalyzer::analyze_destructor_body(Decl* decl, Type* struct_type) {
    auto& dd = decl->destructor_decl;
    // Track delete destructor context to forbid throw inside it.
    // Named destructors (dd.name is non-empty) are explicitly called and can throw.
    ScopedValue in_delete_guard(m_in_delete_destructor);
    if (dd.name.empty()) {
        m_in_delete_destructor = true;
    }
    analyze_member_body(decl, struct_type, dd.params, dd.body, m_types.void_type());
}

void SemanticAnalyzer::analyze_method_body(Decl* decl, Type* struct_type) {
    MethodDecl& method_decl = decl->method_decl;
    Type* return_type = method_decl.return_type ? resolve_type_expr(method_decl.return_type) : m_types.void_type();
    if (!return_type) return_type = m_types.error_type();
    analyze_member_body(decl, struct_type, method_decl.params, method_decl.body, return_type);
}

// ===== Span Append Helpers =====

void SemanticAnalyzer::append_method(StructTypeInfo& info, MethodInfo method) {
    Vector<MethodInfo> methods;
    for (const auto& m : info.methods) {
        methods.push_back(m);
    }
    methods.push_back(method);
    info.methods = m_allocator.alloc_span(methods);
}

void SemanticAnalyzer::append_constructor(StructTypeInfo& info, ConstructorInfo ctor) {
    Vector<ConstructorInfo> ctors;
    for (const auto& c : info.constructors) {
        ctors.push_back(c);
    }
    ctors.push_back(ctor);
    info.constructors = m_allocator.alloc_span(ctors);
}

void SemanticAnalyzer::append_destructor(StructTypeInfo& info, DestructorInfo dtor) {
    Vector<DestructorInfo> dtors;
    for (const auto& d : info.destructors) {
        dtors.push_back(d);
    }
    dtors.push_back(dtor);
    info.destructors = m_allocator.alloc_span(dtors);
}

// ===== Trait Validation =====

Type* SemanticAnalyzer::resolve_trait_method_type_expr(TypeExpr* type_expr,
                                                 const TraitTypeInfo& trait_info) {
    if (!type_expr) {
        return m_types.error_type();
    }
    if (type_expr->name == "Self") {
        return m_types.self_type();
    }
    // Check if this is a trait type param
    for (u32 i = 0; i < trait_info.type_params.size(); i++) {
        if (type_expr->name == trait_info.type_params[i].name) {
            return m_types.type_param(trait_info.type_params[i].name, i);
        }
    }
    Type* resolved = resolve_type_expr(type_expr);
    if (!resolved) resolved = m_types.error_type();
    return resolved;
}

void SemanticAnalyzer::analyze_trait_method_decl(Decl* decl, Type* trait_type) {
    MethodDecl& method_decl = decl->method_decl;

    // Check for duplicate method names in this trait. Builtin traits (Hash,
    // Eq, etc.) pre-register their required methods with `decl == nullptr`;
    // if the user redeclares the same method (matching name + arity), allow
    // it as an idempotent re-declaration so user `trait Eq; fun Eq.eq(other:
    // Self): bool;` doesn't conflict with the builtin Eq registration.
    TraitTypeInfo& trait_type_info = trait_type->trait_info;
    for (const auto& trait_method : trait_type_info.methods) {
        if (trait_method.name == method_decl.name) {
            bool is_builtin_redecl = trait_method.decl == nullptr &&
                trait_method.param_types.size() == method_decl.params.size();
            if (is_builtin_redecl) {
                // The builtin shape stays; ignore the redeclaration silently.
                return;
            }
            error_fmt(decl->loc, "duplicate trait method '{}' in trait '{}'",
                     method_decl.name, trait_type_info.name);
            return;
        }
    }

    // Resolve parameter types - use TypeKind::Self for Self, TypeParam for trait type params
    Vector<Type*> param_types;
    for (const auto& param : method_decl.params) {
        param_types.push_back(resolve_trait_method_type_expr(param.type, trait_type_info));
    }

    // Resolve return type (TypeKind::Self for Self, TypeParam for trait type params)
    Type* return_type = method_decl.return_type
        ? resolve_trait_method_type_expr(method_decl.return_type, trait_type_info)
        : m_types.void_type();

    // Create trait method info
    TraitMethodInfo trait_method_info;
    trait_method_info.name = method_decl.name;
    trait_method_info.param_types = m_allocator.alloc_span(param_types);
    trait_method_info.return_type = return_type;
    trait_method_info.decl = decl;
    trait_method_info.has_default = (method_decl.body != nullptr);

    // Add to trait's method list
    Vector<TraitMethodInfo> methods;
    for (const auto& method : trait_type_info.methods) {
        methods.push_back(method);
    }
    methods.push_back(trait_method_info);

    trait_type_info.methods = m_allocator.alloc_span(methods);
}

// ===== Generic Trait Bound Resolution =====

Span<TraitBound> SemanticAnalyzer::resolve_type_param_bounds(Span<TypeExpr*> bound_exprs, SourceLocation loc) {
    if (bound_exprs.size() == 0) return {};

    Vector<TraitBound> bounds;
    for (auto* bound_expr : bound_exprs) {
        // Reject reference kinds on bounds
        if (bound_expr->ref_kind != RefKind::None) {
            error(bound_expr->loc, "trait bounds cannot have reference qualifiers");
            continue;
        }

        // Look up the trait by name
        Type* trait_type = m_type_env.trait_type_by_name(bound_expr->name);
        if (!trait_type) {
            error_fmt(bound_expr->loc, "unknown trait '{}' in type parameter bound", bound_expr->name);
            continue;
        }

        // Resolve type args for generic trait bounds (e.g., Add<i32>)
        Vector<Type*> resolved_type_args;
        for (auto* type_arg_expr : bound_expr->type_args) {
            Type* arg_type = resolve_type_expr(type_arg_expr);
            if (!arg_type || arg_type->is_error()) {
                // Already reported
                continue;
            }
            resolved_type_args.push_back(arg_type);
        }

        // Validate type arg count against trait's type params
        u32 expected_count = trait_type->trait_info.type_params.size();
        if (resolved_type_args.size() != expected_count) {
            error_fmt(bound_expr->loc, "trait '{}' expects {} type arguments but got {}",
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

bool SemanticAnalyzer::resolve_template_bounds(Span<TypeParam> type_params, ResolvedTypeParams& out) {
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

void SemanticAnalyzer::resolve_generic_bounds() {
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

bool SemanticAnalyzer::check_type_arg_bounds(StringView template_name, Span<Type*> type_args,
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
                auto concrete_str = m_checker.type_string(concrete_type);
                // Build trait name with type args for error message
                String trait_str;
                auto append_sv = [&](StringView sv) { for (char c : sv) trait_str.push_back(c); };
                auto append_cs = [&](const char* cs) { while (*cs) trait_str.push_back(*cs++); };
                append_sv(bound.trait->trait_info.name);
                if (subst_args.size() > 0) {
                    append_cs("<");
                    for (u32 j = 0; j < subst_args.size(); j++) {
                        if (j > 0) append_cs(", ");
                        type_to_string(subst_args[j], trait_str);
                    }
                    append_cs(">");
                }
                trait_str.push_back('\0');

                error_fmt(loc,
                    "type '{}' does not implement trait '{}' required by type parameter bound on '{}'",
                    concrete_str.data(), trait_str.data(), template_name);
                all_ok = false;
            }
        }
    }
    return all_ok;
}

// ============================================================================
// Phase B: Definition-site checking of generic template bodies
// ============================================================================

Type* SemanticAnalyzer::substitute_trait_types(Type* type, Type* type_param, Type* found_in_trait) {
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

const TraitMethodInfo* SemanticAnalyzer::lookup_type_param_method(
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

Type* SemanticAnalyzer::analyze_type_param_method_call(
    Expr* expr, CallExpr& call_expr, GetExpr& get_expr, Type* obj_type,
    Type* type_param_type, const TraitMethodInfo* trait_method, Type* found_in_trait) {

    auto substitute = [&](Type* t) -> Type* {
        return substitute_trait_types(t, type_param_type, found_in_trait);
    };

    // Check argument count
    if (call_expr.arguments.size() != trait_method->param_types.size()) {
        error_fmt(expr->loc, "method '{}' expects {} arguments but got {}",
                 trait_method->name, trait_method->param_types.size(), call_expr.arguments.size());
        return substitute(trait_method->return_type);
    }

    // Type-check arguments with substituted parameter types
    for (u32 i = 0; i < call_expr.arguments.size(); i++) {
        Type* arg_type = analyze_expr(call_expr.arguments[i].expr);
        Type* param_type = substitute(trait_method->param_types[i]);
        if (!arg_type->is_error() && !param_type->is_error()) {
            m_checker.check_assignable(param_type, arg_type, call_expr.arguments[i].expr->loc);
        }
    }

    get_expr.object->resolved_type = obj_type;
    return substitute(trait_method->return_type);
}

void SemanticAnalyzer::analyze_generic_template_body(Decl* decl) {
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

    // Resolve return type (may reference type params → resolves to TypeParam via resolve_type_expr)
    Type* return_type = fun_decl.return_type ? resolve_type_expr(fun_decl.return_type) : m_types.void_type();

    // Reset move states (same pattern as analyze_fun_decl).
    ScopedValue move_states_guard(m_move_states);
    m_move_states.clear();

    // Push function scope with return type
    m_symbols.push_function_scope(return_type);

    // Define parameters in scope
    for (u32 i = 0; i < fun_decl.params.size(); i++) {
        Param& param = fun_decl.params[i];
        Type* param_type = resolve_type_expr(param.type);
        if (!param_type) param_type = m_types.error_type();
        m_symbols.define_parameter(param.name, param_type, decl->loc, i);
    }

    // Analyze the body
    analyze_stmt(fun_decl.body);

    m_symbols.pop_scope();
    // move_states_guard / params_guard / bounds_guard restore on return.
}

bool SemanticAnalyzer::resolve_trait_impl_type_args(
        MethodDecl& method_decl, const TraitTypeInfo& trait_info,
        Type* struct_type, SourceLocation loc, Span<Type*>& out) {
    out = Span<Type*>();
    if (method_decl.trait_type_args.size() > 0) {
        if (trait_info.type_params.size() == 0) {
            error_fmt(loc, "trait '{}' does not take type arguments", method_decl.trait_name);
            return false;
        }
        if (method_decl.trait_type_args.size() != trait_info.type_params.size()) {
            error_fmt(loc, "trait '{}' expects {} type argument(s), got {}",
                     method_decl.trait_name, trait_info.type_params.size(),
                     method_decl.trait_type_args.size());
            return false;
        }
        Vector<Type*> args;
        for (auto* type_arg : method_decl.trait_type_args) {
            Type* arg_type = resolve_type_expr(type_arg);
            if (!arg_type) arg_type = m_types.error_type();
            args.push_back(arg_type);
        }
        out = m_allocator.alloc_span(args);
    } else if (trait_info.type_params.size() > 0) {
        // Default all type params to Self (the implementing struct type).
        // Enables `for Add` as shorthand for `for Add<Vec2>` on struct Vec2.
        Vector<Type*> args;
        for (u32 i = 0; i < trait_info.type_params.size(); i++) {
            args.push_back(struct_type);
        }
        out = m_allocator.alloc_span(args);
    }
    return true;
}

Vector<SemanticAnalyzer::TraitImplGroup> SemanticAnalyzer::group_pending_trait_impls() {
    // Group pending impls by (struct, trait, type_args). A simple list of
    // groups suffices since we don't expect many.
    Vector<TraitImplGroup> groups;
    for (auto& pending : m_pending_trait_impls) {
        TraitImplGroup* group = nullptr;
        for (auto& g : groups) {
            if (g.struct_type != pending.struct_type || g.trait_type != pending.trait_type) continue;
            // Match on type args element-wise too.
            bool args_match = g.trait_type_args.size() == pending.trait_type_args.size();
            if (args_match) {
                for (u32 i = 0; i < g.trait_type_args.size(); i++) {
                    if (g.trait_type_args[i] != pending.trait_type_args[i]) { args_match = false; break; }
                }
            }
            if (args_match) { group = &g; break; }
        }
        if (!group) {
            groups.push_back({pending.struct_type, pending.trait_type, pending.trait_type_args, {}});
            group = &groups.back();
        }
        group->impl_decls.push_back(pending.decl);
    }
    return groups;
}

bool SemanticAnalyzer::check_parent_trait_satisfied(const TraitImplGroup& group,
                                                    const Vector<TraitImplGroup>& all_groups) {
    TraitTypeInfo& trait_type_info = group.trait_type->trait_info;
    if (!trait_type_info.parent) return true;

    StructTypeInfo& struct_type_info = group.struct_type->struct_info;
    for (const auto& impl : struct_type_info.implemented_traits) {
        if (impl.trait == trait_type_info.parent) return true;
    }
    // Also satisfied if the parent trait is implemented in this same batch.
    for (const auto& other : all_groups) {
        if (other.struct_type == group.struct_type && other.trait_type == trait_type_info.parent) {
            return true;
        }
    }
    error_fmt(group.impl_decls[0]->loc,
             "trait '{}' requires parent trait '{}' to be implemented for '{}'",
             trait_type_info.name, trait_type_info.parent->trait_info.name, struct_type_info.name);
    return false;
}

void SemanticAnalyzer::validate_and_register_impl_method(const TraitImplGroup& group, Decl* decl,
                                                         Vector<bool>& implemented) {
    TraitTypeInfo& trait_type_info = group.trait_type->trait_info;
    StructTypeInfo& struct_type_info = group.struct_type->struct_info;
    MethodDecl& method_decl = decl->method_decl;

    for (u32 i = 0; i < trait_type_info.methods.size(); i++) {
        const TraitMethodInfo& trait_method = trait_type_info.methods[i];
        if (trait_method.name != method_decl.name) continue;
        implemented[i] = true;

        // Validate parameter count matches.
        if (method_decl.params.size() != trait_method.param_types.size()) {
            error_fmt(decl->loc, "method '{}' parameter count mismatch with trait '{}'",
                     method_decl.name, trait_type_info.name);
        }

        // Resolve the impl's param/return types.
        Vector<Type*> param_types;
        for (const auto& param : method_decl.params) {
            Type* ptype = resolve_type_expr(param.type);
            if (!ptype) ptype = m_types.error_type();
            param_types.push_back(ptype);
        }
        Type* return_type = method_decl.return_type ? resolve_type_expr(method_decl.return_type) : m_types.void_type();
        if (!return_type) return_type = m_types.error_type();

        // Validate parameter types match the trait signature (Self / trait
        // type-params concretized against this impl).
        if (method_decl.params.size() == trait_method.param_types.size()) {
            for (u32 p = 0; p < param_types.size(); p++) {
                if (param_types[p]->is_error()) continue;
                Type* expected = concretize_trait_type(trait_method.param_types[p],
                                                       group.struct_type, group.trait_type_args);
                if (param_types[p] != expected) {
                    auto got_str = m_checker.type_string(param_types[p]);
                    auto exp_str = m_checker.type_string(expected);
                    error_fmt(decl->loc,
                             "method '{}' parameter {} has type '{}' but trait '{}' expects '{}'",
                             method_decl.name, p + 1, got_str.data(), trait_type_info.name, exp_str.data());
                }
            }
        }

        // Validate return type matches the trait signature.
        if (!return_type->is_error()) {
            Type* expected_ret = concretize_trait_type(trait_method.return_type,
                                                       group.struct_type, group.trait_type_args);
            if (return_type != expected_ret) {
                auto got_str = m_checker.type_string(return_type);
                auto exp_str = m_checker.type_string(expected_ret);
                error_fmt(decl->loc,
                         "method '{}' return type is '{}' but trait '{}' expects '{}'",
                         method_decl.name, got_str.data(), trait_type_info.name, exp_str.data());
            }
        }

        // Register as a regular method on the struct, unless one already exists.
        bool is_duplicate = false;
        for (const auto& method : struct_type_info.methods) {
            if (method.name == method_decl.name) { is_duplicate = true; break; }
        }
        if (!is_duplicate) {
            MethodInfo method_info;
            method_info.name = method_decl.name;
            method_info.param_types = m_allocator.alloc_span(param_types);
            method_info.return_type = return_type;
            method_info.decl = decl;
            append_method(struct_type_info, method_info);
        }
        return;  // matched a trait method
    }

    error_fmt(decl->loc, "method '{}' is not defined in trait '{}'",
             method_decl.name, trait_type_info.name);
}

void SemanticAnalyzer::finalize_trait_impl(const TraitImplGroup& group, const Vector<bool>& implemented) {
    TraitTypeInfo& trait_type_info = group.trait_type->trait_info;
    StructTypeInfo& struct_type_info = group.struct_type->struct_info;

    // Missing required methods; inject defaults where the trait provides one.
    for (u32 i = 0; i < trait_type_info.methods.size(); i++) {
        if (implemented[i]) continue;
        if (trait_type_info.methods[i].has_default) {
            inject_default_method(group.struct_type, group.trait_type,
                                  trait_type_info.methods[i], group.trait_type_args);
        } else {
            error_fmt(group.impl_decls[0]->loc,
                     "trait '%.*s' requires method '%.*s' which is not implemented for '%.*s'",
                     trait_type_info.name.size(), trait_type_info.name.data(),
                     trait_type_info.methods[i].name.size(), trait_type_info.methods[i].name.data(),
                     struct_type_info.name.size(), struct_type_info.name.data());
        }
    }

    // Append this trait to the struct's implemented_traits list.
    Vector<TraitImplRecord> trait_records;
    for (const auto& impl : struct_type_info.implemented_traits) {
        trait_records.push_back(impl);
    }
    trait_records.push_back(TraitImplRecord{group.trait_type, group.trait_type_args});
    struct_type_info.implemented_traits = m_allocator.alloc_span(trait_records);
}

void SemanticAnalyzer::validate_trait_implementations() {
    Vector<TraitImplGroup> groups = group_pending_trait_impls();
    for (auto& group : groups) {
        if (!check_parent_trait_satisfied(group, groups)) continue;

        Vector<bool> implemented(group.trait_type->trait_info.methods.size(), false);
        for (Decl* decl : group.impl_decls) {
            validate_and_register_impl_method(group, decl, implemented);
        }
        finalize_trait_impl(group, implemented);
    }
}

Type* SemanticAnalyzer::concretize_trait_type(Type* abstract_type, Type* struct_type, Span<Type*> trait_type_args) {
    if (abstract_type->is_self()) return struct_type;
    if (abstract_type->is_type_param() && trait_type_args.size() > 0) {
        u32 idx = abstract_type->type_param_info.index;
        if (idx < trait_type_args.size()) return trait_type_args[idx];
    }
    return abstract_type;
}

void SemanticAnalyzer::inject_default_method(Type* struct_type, Type* trait_type,
                                              TraitMethodInfo& trait_method_info, Span<Type*> trait_type_args) {
    // Create a synthetic DeclMethod that targets the implementing struct
    // with a cloned body from the trait's default method
    Decl* trait_decl = trait_method_info.decl;
    MethodDecl& trait_md = trait_decl->method_decl;
    TraitTypeInfo& trait_type_info = trait_type->trait_info;

    Decl* synth = m_allocator.emplace<Decl>();
    synth->kind = AstKind::DeclMethod;
    synth->loc = trait_decl->loc;
    synth->method_decl.struct_name = struct_type->struct_info.name;
    synth->method_decl.name = trait_method_info.name;
    synth->method_decl.type_params = Span<TypeParam>();
    synth->method_decl.is_pub = trait_md.is_pub;
    synth->method_decl.is_native = false;
    synth->method_decl.trait_name = trait_type->trait_info.name;
    synth->method_decl.trait_type_args = Span<TypeExpr*>();

    // Build type substitution: always include Self -> struct_type,
    // plus trait type params -> concrete type args for generic traits
    {
        Vector<StringView> param_names;
        Vector<Type*> concrete_types;

        // Always substitute Self -> struct_type
        param_names.push_back(StringView("Self", 4));
        concrete_types.push_back(struct_type);

        // Add trait type params for generic traits
        if (trait_type_info.type_params.size() > 0 && trait_type_args.size() > 0) {
            for (u32 i = 0; i < trait_type_info.type_params.size(); i++) {
                param_names.push_back(trait_type_info.type_params[i].name);
                concrete_types.push_back(trait_type_args[i]);
            }
        }

        TypeSubstitution subst;
        subst.param_names = m_allocator.alloc_span(param_names);
        subst.concrete_types = m_allocator.alloc_span(concrete_types);

        // Clone body with type substitution
        synth->method_decl.body = m_type_env.generics().clone_stmt(trait_md.body, subst);

        // Clone params with type-expr substitution
        Vector<Param> cloned_params;
        for (const auto& param : trait_md.params) {
            Param p = param;
            p.type = m_type_env.generics().substitute_type_expr(param.type, subst);
            cloned_params.push_back(p);
        }
        synth->method_decl.params = m_allocator.alloc_span(cloned_params);
        synth->method_decl.return_type = m_type_env.generics().substitute_type_expr(trait_md.return_type, subst);
    }

    // Resolve concrete param types (Self -> struct_type, TypeParam -> concrete type)
    StructTypeInfo& struct_type_info = struct_type->struct_info;
    Vector<Type*> param_types;
    for (auto* param_type : trait_method_info.param_types) {
        param_types.push_back(concretize_trait_type(param_type, struct_type, trait_type_args));
    }
    Type* return_type = concretize_trait_type(trait_method_info.return_type, struct_type, trait_type_args);

    // Add MethodInfo to struct's method list
    MethodInfo method_info;
    method_info.name = trait_method_info.name;
    method_info.param_types = m_allocator.alloc_span(param_types);
    method_info.return_type = return_type;
    method_info.decl = synth;

    append_method(struct_type_info, method_info);

    // Add to synthetic decls list for IR builder
    m_synthetic_decls.push_back(synth);
}

// ===== Primitive Operator Registration =====

void SemanticAnalyzer::register_primitive_operator_methods() {
    // Guard: only register once (TypeEnv persists across modules)
    // Use I32's "add" method as a sentinel — if it's already registered, skip.
    if (m_types.lookup_primitive_method(TypeKind::I32, StringView("add", 3))) return;

    // Helper: allocate a Span<Type*> with one element from the bump allocator
    auto make_param_span = [this](Type* param) -> Span<Type*> {
        Type** data = reinterpret_cast<Type**>(
            m_allocator.alloc_bytes(sizeof(Type*), alignof(Type*)));
        data[0] = param;
        return Span<Type*>(data, 1);
    };

    Span<Type*> no_params(nullptr, 0);

    // Helper: register a batch of operator methods for a primitive type
    auto register_ops = [this](TypeKind kind, const char* const* op_names, u32 count,
                               Span<Type*> param_types, Type* return_type) {
        for (u32 i = 0; i < count; i++) {
            MethodInfo method_info;
            method_info.name = StringView(op_names[i], static_cast<u32>(strlen(op_names[i])));
            method_info.param_types = param_types;
            method_info.return_type = return_type;
            method_info.decl = nullptr;
            m_types.register_primitive_method(kind, method_info);
        }
    };

    // Register for integer types (I32, I64)
    struct { TypeKind kind; Type* type; } int_types[] = {
        { TypeKind::I32, m_types.i32_type() },
        { TypeKind::I64, m_types.i64_type() },
    };
    for (auto& entry : int_types) {
        Span<Type*> self_param = make_param_span(entry.type);

        const char* arith_ops[] = { "add", "sub", "mul", "div", "mod" };
        register_ops(entry.kind, arith_ops, 5, self_param, entry.type);

        const char* bit_ops[] = { "bit_and", "bit_or", "bit_xor", "shl", "shr" };
        register_ops(entry.kind, bit_ops, 5, self_param, entry.type);

        const char* cmp_ops[] = { "eq", "ne", "lt", "le", "gt", "ge" };
        register_ops(entry.kind, cmp_ops, 6, self_param, m_types.bool_type());

        const char* unary_ops[] = { "neg", "bit_not" };
        register_ops(entry.kind, unary_ops, 2, no_params, entry.type);

        const char* compound_ops[] = {
            "add_assign", "sub_assign", "mul_assign", "div_assign", "mod_assign",
            "bit_and_assign", "bit_or_assign", "bit_xor_assign", "shl_assign", "shr_assign"
        };
        register_ops(entry.kind, compound_ops, 10, self_param, m_types.void_type());
    }

    // Register for float types (F32, F64)
    struct { TypeKind kind; Type* type; } float_types[] = {
        { TypeKind::F32, m_types.f32_type() },
        { TypeKind::F64, m_types.f64_type() },
    };
    for (auto& entry : float_types) {
        Span<Type*> self_param = make_param_span(entry.type);

        const char* arith_ops[] = { "add", "sub", "mul", "div" };
        register_ops(entry.kind, arith_ops, 4, self_param, entry.type);

        const char* cmp_ops[] = { "eq", "ne", "lt", "le", "gt", "ge" };
        register_ops(entry.kind, cmp_ops, 6, self_param, m_types.bool_type());

        const char* unary_ops[] = { "neg" };
        register_ops(entry.kind, unary_ops, 1, no_params, entry.type);

        const char* compound_ops[] = { "add_assign", "sub_assign", "mul_assign", "div_assign" };
        register_ops(entry.kind, compound_ops, 4, self_param, m_types.void_type());
    }

    // Register for bool: eq, ne → bool
    {
        Span<Type*> bool_param = make_param_span(m_types.bool_type());
        const char* bool_ops[] = { "eq", "ne" };
        register_ops(TypeKind::Bool, bool_ops, 2, bool_param, m_types.bool_type());
    }

    // Register for string: eq, ne → bool
    {
        Span<Type*> string_param = make_param_span(m_types.string_type());
        const char* string_ops[] = { "eq", "ne" };
        register_ops(TypeKind::String, string_ops, 2, string_param, m_types.bool_type());
    }
}

// ===== Operator Dispatch Helpers =====

Type* SemanticAnalyzer::try_resolve_binary_op(BinaryOp op, Type* left, Type* right) {
    const char* method_name = binary_op_to_trait_method(op);
    if (!method_name) return nullptr;
    StringView name(method_name, static_cast<u32>(strlen(method_name)));
    const MethodInfo* mi = m_types.lookup_method(left, name);
    if (mi && mi->param_types.size() == 1) {
        if (mi->param_types[0] == right) {
            return mi->return_type;
        }
        // IntLiteral is compatible with the method's integer param type
        if (right->is_int_literal() && mi->param_types[0]->is_integer()) {
            return mi->return_type;
        }
    }

    // Phase B: TypeParam path — look up through trait bounds
    if (left->is_type_param() && m_active_type_param_bounds.size() > 0) {
        Type* found_in_trait = nullptr;
        const TraitMethodInfo* trait_method = lookup_type_param_method(left, name, &found_in_trait);
        if (trait_method && trait_method->param_types.size() == 1) {
            Type* return_type = substitute_trait_types(trait_method->return_type, left, found_in_trait);
            Type* param_type = substitute_trait_types(trait_method->param_types[0], left, found_in_trait);
            // Check right operand compatibility
            if (param_type == right || m_checker.is_assignable(param_type, right)) {
                return return_type;
            }
        }
    }
    return nullptr;
}

Type* SemanticAnalyzer::try_resolve_unary_op(UnaryOp op, Type* operand) {
    const char* method_name = unary_op_to_trait_method(op);
    if (!method_name) return nullptr;
    StringView name(method_name, static_cast<u32>(strlen(method_name)));
    const MethodInfo* mi = m_types.lookup_method(operand, name);
    if (mi && mi->param_types.size() == 0 && mi->return_type) {
        return mi->return_type;
    }

    // Phase B: TypeParam path — look up through trait bounds
    if (operand->is_type_param() && m_active_type_param_bounds.size() > 0) {
        Type* found_in_trait = nullptr;
        const TraitMethodInfo* trait_method = lookup_type_param_method(operand, name, &found_in_trait);
        if (trait_method && trait_method->param_types.size() == 0 && trait_method->return_type) {
            return substitute_trait_types(trait_method->return_type, operand, found_in_trait);
        }
    }
    return nullptr;
}

// ===== Statement Analysis =====

void SemanticAnalyzer::analyze_stmt(Stmt* stmt) {
    if (!stmt) return;

    switch (stmt->kind) {
        case AstKind::StmtExpr:
            analyze_expr_stmt(stmt);
            break;
        case AstKind::StmtBlock:
            analyze_block_stmt(stmt);
            break;
        case AstKind::StmtIf:
            analyze_if_stmt(stmt);
            break;
        case AstKind::StmtWhile:
            analyze_while_stmt(stmt);
            break;
        case AstKind::StmtFor:
            analyze_for_stmt(stmt);
            break;
        case AstKind::StmtReturn:
            analyze_return_stmt(stmt);
            break;
        case AstKind::StmtBreak:
            analyze_break_stmt(stmt);
            break;
        case AstKind::StmtContinue:
            analyze_continue_stmt(stmt);
            break;
        case AstKind::StmtDelete:
            analyze_delete_stmt(stmt);
            break;
        case AstKind::StmtWhen:
            analyze_when_stmt(stmt);
            break;
        case AstKind::StmtThrow:
            analyze_throw_stmt(stmt);
            break;
        case AstKind::StmtTry:
            analyze_try_stmt(stmt);
            break;
        case AstKind::StmtYield:
            analyze_yield_stmt(stmt);
            break;
        default:
            break;
    }
}

void SemanticAnalyzer::analyze_expr_stmt(Stmt* stmt) {
    analyze_expr(stmt->expr_stmt.expr);
}

void SemanticAnalyzer::analyze_block_stmt(Stmt* stmt) {
    m_symbols.push_scope(ScopeKind::Block);

    BlockStmt& block = stmt->block;
    for (auto* decl : block.declarations) {
        if (!decl) continue;

        if (decl->kind == AstKind::DeclVar) {
            analyze_var_decl(decl);
        } else if (decl->kind == AstKind::DeclFun) {
            // Local functions (if supported)
            error(decl->loc, "local function declarations are not supported; move this function to module scope");
        } else {
            // Statement wrapped in a Decl
            analyze_stmt(&decl->stmt);
        }
    }

    check_scope_exit_uniq_destructors(m_symbols.current_scope(), stmt->loc);
    m_symbols.pop_scope();
}

void SemanticAnalyzer::analyze_if_stmt(Stmt* stmt) {
    IfStmt& is = stmt->if_stmt;

    Type* cond_type = analyze_expr(is.condition);
    if (cond_type && !cond_type->is_error()) {
        m_checker.check_boolean(cond_type, is.condition->loc);
    }

    // Save move states before branching
    MoveStateSnapshot pre_branch_states = save_move_states();

    // Reset termination flag for the then-branch; capture whether it ended
    // with an unconditional return/throw/break/continue.
    m_branch_terminates = false;
    analyze_stmt(is.then_branch);
    MoveStateSnapshot then_states = save_move_states();
    bool then_terminates = m_branch_terminates;

    if (is.else_branch) {
        // Restore to pre-branch state for else analysis
        restore_move_states(pre_branch_states);
        m_branch_terminates = false;
        analyze_stmt(is.else_branch);
        MoveStateSnapshot else_states = save_move_states();
        bool else_terminates = m_branch_terminates;

        merge_two_branches(pre_branch_states, then_states, else_states,
                           then_terminates, else_terminates);
    } else {
        // No else branch — the implicit else is the pre-branch state (no moves)
        // and can never terminate. merge_two_branches therefore leaves
        // m_branch_terminates false: a no-else if never proves termination
        // because the condition may be false.
        merge_two_branches(pre_branch_states, then_states, pre_branch_states,
                           then_terminates, /*else_terminates=*/false);
    }
}

void SemanticAnalyzer::analyze_while_stmt(Stmt* stmt) {
    WhileStmt& ws = stmt->while_stmt;

    Type* cond_type = analyze_expr(ws.condition);
    if (cond_type && !cond_type->is_error()) {
        m_checker.check_boolean(cond_type, ws.condition->loc);
    }

    // Save move states before loop body
    MoveStateSnapshot pre_loop_states = save_move_states();

    // The loop body may execute zero times, so any termination inside the
    // body (return/throw/break/continue) does not prove the loop itself
    // terminates. Preserve the caller's flag across the body.
    bool pre_loop_terminates = m_branch_terminates;

    m_symbols.push_loop_scope();
    analyze_stmt(ws.body);
    check_scope_exit_uniq_destructors(m_symbols.current_scope(), stmt->loc);
    m_symbols.pop_scope();

    m_branch_terminates = pre_loop_terminates;

    MoveStateSnapshot post_body_states = save_move_states();

    check_loop_cross_iteration_moves(ws.body, pre_loop_states, post_body_states, stmt->loc);

    // After loop: merge pre-loop with post-body (loop may execute 0 times)
    restore_move_states(pre_loop_states);
    merge_move_states(post_body_states, pre_loop_states);
}

void SemanticAnalyzer::analyze_for_stmt(Stmt* stmt) {
    ForStmt& fs = stmt->for_stmt;

    // Create a scope for the entire for statement (includes initializer)
    m_symbols.push_scope(ScopeKind::Block);

    if (fs.initializer) {
        if (fs.initializer->kind == AstKind::DeclVar) {
            analyze_var_decl(fs.initializer);
        } else {
            analyze_stmt(&fs.initializer->stmt);
        }
    }

    if (fs.condition) {
        Type* cond_type = analyze_expr(fs.condition);
        if (cond_type && !cond_type->is_error()) {
            m_checker.check_boolean(cond_type, fs.condition->loc);
        }
    }

    // Save move states after initializer/condition, before loop body. The
    // increment is analyzed AFTER the body to match runtime ordering: in
    // iteration 1 the body runs against the post-init state, and only then
    // does the increment fire. Analyzing the increment up-front would make
    // any moves it performs visible to the body and produce a false positive
    // on iteration 1 (the body actually sees a live value there).
    MoveStateSnapshot pre_loop_states = save_move_states();

    // The loop body may execute zero times — see note in analyze_while_stmt.
    bool pre_loop_terminates = m_branch_terminates;

    m_symbols.push_loop_scope();
    analyze_stmt(fs.body);
    check_scope_exit_uniq_destructors(m_symbols.current_scope(), stmt->loc);
    m_symbols.pop_scope();

    m_branch_terminates = pre_loop_terminates;

    if (fs.increment) {
        analyze_expr(fs.increment);
    }

    MoveStateSnapshot post_body_states = save_move_states();

    check_loop_cross_iteration_moves(fs.body, pre_loop_states, post_body_states, stmt->loc);

    // After loop: merge pre-loop with post-body (loop may execute 0 times)
    restore_move_states(pre_loop_states);
    merge_move_states(post_body_states, pre_loop_states);

    check_scope_exit_uniq_destructors(m_symbols.current_scope(), stmt->loc);
    m_symbols.pop_scope();
}

void SemanticAnalyzer::analyze_return_stmt(Stmt* stmt) {
    ReturnStmt& rs = stmt->return_stmt;

    if (!m_symbols.is_in_function()) {
        error(stmt->loc, "'return' statement outside of function");
        return;
    }

    // In coroutine functions, only bare 'return;' is allowed (no return value)
    if (m_in_coroutine) {
        if (rs.value) {
            error(stmt->loc, "coroutine functions cannot return a value; use 'yield' instead");
        }
        m_symbols.mark_return();
        m_branch_terminates = true;
        return;
    }

    Type* expected = m_symbols.current_return_type();

    if (rs.value) {
        Type* actual = analyze_expr(rs.value);
        if (expected && coerce_generic_template_ref(rs.value, expected)) {
            actual = rs.value->resolved_type;
        }
        if (!m_checker.check_assignable(expected, actual, stmt->loc)) {
            // Error already reported
        } else {
            m_checker.coerce_int_literal(rs.value, expected);
        }

        // Consume noncopyable return value (field-move check + mark source as moved)
        consume_noncopyable(rs.value, stmt->loc);
    } else {
        if (!expected->is_void()) {
            error(stmt->loc, "non-void function must return a value");
        }
    }

    check_all_scopes_uniq_destructors(stmt->loc, ScopeKind::Function);
    m_symbols.mark_return();
    m_branch_terminates = true;
}

void SemanticAnalyzer::analyze_break_stmt(Stmt* stmt) {
    if (!m_symbols.is_in_loop()) {
        error(stmt->loc, "'break' statement outside of loop");
    } else {
        check_all_scopes_uniq_destructors(stmt->loc, ScopeKind::Loop);
    }
    m_branch_terminates = true;
}

void SemanticAnalyzer::analyze_continue_stmt(Stmt* stmt) {
    if (!m_symbols.is_in_loop()) {
        error(stmt->loc, "'continue' statement outside of loop");
    } else {
        check_all_scopes_uniq_destructors(stmt->loc, ScopeKind::Loop);
    }
    m_branch_terminates = true;
}

void SemanticAnalyzer::analyze_delete_stmt(Stmt* stmt) {
    DeleteStmt& ds = stmt->delete_stmt;

    Type* type = analyze_expr(ds.expr);
    if (!type || type->is_error()) return;

    // delete only works on uniq types
    if (type->kind != TypeKind::Uniq) {
        error(stmt->loc, "'delete' can only be used on 'uniq' types");
        return;
    }

    // Cannot delete a struct field directly (would leave dangling pointer in parent)
    if (!check_not_field_move(ds.expr, stmt->loc)) return;

    // Get the inner struct type
    Type* inner_type = type->ref_info.inner_type;
    if (inner_type && inner_type->is_struct()) {
        StructTypeInfo& struct_type_info = inner_type->struct_info;

        // Look up destructor by name
        const DestructorInfo* dtor = nullptr;
        for (const auto& destructor : struct_type_info.destructors) {
            if (destructor.name == ds.destructor_name) {
                dtor = &destructor;
                break;
            }
        }

        // If a destructor name was specified but not found
        if (!ds.destructor_name.empty() && !dtor) {
            error_fmt(stmt->loc, "struct '{}' has no destructor '{}'",
                     struct_type_info.name, ds.destructor_name);
            return;
        }

        // If we have a destructor, type-check the arguments
        if (dtor) {
            // Check argument count
            if (ds.arguments.size() != dtor->param_types.size()) {
                error_fmt(stmt->loc, "destructor expects {} arguments but got {}",
                         dtor->param_types.size(), ds.arguments.size());
                return;
            }

            // Check argument types and modifiers (skip for synthetic destructors with no decl)
            if (dtor->decl) {
                DestructorDecl* dtor_decl = &dtor->decl->destructor_decl;
                check_call_args(ds.arguments, dtor->param_types, dtor_decl->params, stmt->loc);
            }
        } else {
            // No destructor defined with this name
            if (!ds.destructor_name.empty()) {
                error_fmt(stmt->loc, "struct '{}' has no destructor '{}'",
                         struct_type_info.name, ds.destructor_name);
                return;
            }

            // Named destructor arguments without a destructor is an error
            if (ds.arguments.size() > 0) {
                error_fmt(stmt->loc, "struct '{}' has no destructor to call",
                         struct_type_info.name);
                return;
            }
        }
    }

    // Consume the deleted variable (mark as moved)
    consume_noncopyable(ds.expr, stmt->loc);
}

void SemanticAnalyzer::analyze_when_stmt(Stmt* stmt) {
    WhenStmt& ws = stmt->when_stmt;

    // Analyze the discriminant expression
    Type* discrim_type = analyze_expr(ws.discriminant);
    if (!discrim_type || discrim_type->is_error()) return;

    // Check that discriminant is an enum type
    if (discrim_type->kind != TypeKind::Enum) {
        error(ws.discriminant->loc, "'when' discriminant must be an enum type");
        return;
    }

    EnumTypeInfo& eti = discrim_type->enum_info;
    EnumDecl& ed = eti.decl->enum_decl;

    // Track which enum variants have been covered (for duplicate detection)
    tsl::robin_map<StringView, bool> covered_variants;

    // Save move states before branching
    MoveStateSnapshot pre_when_states = save_move_states();
    Vector<MoveStateSnapshot> case_snapshots;
    Vector<bool> case_terminates;

    // Analyze each case
    for (auto& wc : ws.cases) {
        // Validate case names are valid enum variants
        for (const auto& case_name : wc.case_names) {
            // Look up the variant in the enum
            bool found = false;
            for (const auto& variant : ed.variants) {
                if (variant.name == case_name) {
                    found = true;
                    break;
                }
            }

            if (!found) {
                error_fmt(wc.loc, "'{}' is not a variant of enum '{}'",
                         case_name, eti.name);
                continue;
            }

            // Check for duplicate case
            if (covered_variants.find(case_name) != covered_variants.end()) {
                error_fmt(wc.loc, "duplicate case '{}' in when statement",
                         case_name);
                continue;
            }

            covered_variants[case_name] = true;
        }

        // Restore pre-when state so each case starts fresh
        restore_move_states(pre_when_states);
        m_branch_terminates = false;

        // Analyze case body in a new scope
        m_symbols.push_scope(ScopeKind::Block);
        for (auto& decl : wc.body) {
            if (!decl) continue;
            if (decl->kind == AstKind::DeclVar) {
                analyze_var_decl(decl);
            } else {
                analyze_stmt(&decl->stmt);
            }
        }
        m_symbols.pop_scope();

        case_snapshots.push_back(save_move_states());
        case_terminates.push_back(m_branch_terminates);
    }

    // Analyze else body if present
    bool has_else = ws.else_body.size() > 0;
    if (has_else) {
        restore_move_states(pre_when_states);
        m_branch_terminates = false;

        m_symbols.push_scope(ScopeKind::Block);
        for (auto& decl : ws.else_body) {
            if (!decl) continue;
            if (decl->kind == AstKind::DeclVar) {
                analyze_var_decl(decl);
            } else {
                analyze_stmt(&decl->stmt);
            }
        }
        m_symbols.pop_scope();

        case_snapshots.push_back(save_move_states());
        case_terminates.push_back(m_branch_terminates);
    } else {
        // No else — an unmatched enum value falls past the whole statement,
        // so the pre-when state is a non-terminating survivor path.
        case_snapshots.push_back(pre_when_states);
        case_terminates.push_back(false);
    }

    // Merge the surviving (non-terminating) case paths. If every path
    // terminates (only possible when an else exists; otherwise the pre-when
    // fall-through is always non-terminating), the code after the when is
    // unreachable and termination propagates upward.
    m_branch_terminates = merge_branch_snapshots(case_snapshots, case_terminates);
}

void SemanticAnalyzer::analyze_throw_stmt(Stmt* stmt) {
    ThrowStmt& ts = stmt->throw_stmt;

    if (m_in_delete_destructor) {
        error(stmt->loc, "'throw' is not allowed inside a delete destructor");
        return;
    }

    Type* expr_type = analyze_expr(ts.expr);
    if (!expr_type || expr_type->is_error()) {
        m_branch_terminates = true;
        return;
    }

    // The thrown expression must be a struct type that implements Exception trait
    Type* base = expr_type->base_type();
    if (!base->is_struct()) {
        error(stmt->loc, "throw expression must be a struct type that implements Exception");
        m_branch_terminates = true;
        return;
    }

    Type* exception_trait = m_type_env.exception_type();
    if (!m_types.implements_trait(base, exception_trait)) {
        error_fmt(stmt->loc, "thrown type '{}' does not implement the Exception trait",
                  base->struct_info.name);
    }
    m_branch_terminates = true;
}

void SemanticAnalyzer::analyze_try_stmt(Stmt* stmt) {
    TryStmt& ts = stmt->try_stmt;

    // Save move states before try body
    MoveStateSnapshot pre_try = save_move_states();

    // Analyze try body (yield is allowed here)
    m_branch_terminates = false;
    analyze_stmt(ts.try_body);
    bool try_terminates = m_branch_terminates;

    // Save post-try states
    MoveStateSnapshot post_try = save_move_states();

    // Compute catch entry state: merge(pre_try, post_try)
    // An exception can be thrown at any point in the try body, so catch clauses
    // must see the conservative merge of pre-try and post-try states
    restore_move_states(pre_try);
    merge_move_states(pre_try, post_try);
    MoveStateSnapshot catch_entry = save_move_states();

    // Normal try exit is one exit path. If the try body ends in an
    // unconditional return/throw, the normal-exit path is unreachable.
    Vector<MoveStateSnapshot> exit_paths;
    Vector<bool> exit_terminates;
    exit_paths.push_back(post_try);
    exit_terminates.push_back(try_terminates);

    bool has_catch_all = false;

    // Analyze each catch clause
    for (u32 i = 0; i < ts.catches.size(); i++) {
        CatchClause& clause = ts.catches[i];

        if (has_catch_all) {
            error(clause.loc, "catch clause after catch-all is unreachable");
            continue;
        }

        // Each catch starts from the same catch entry state
        restore_move_states(catch_entry);
        m_branch_terminates = false;

        m_symbols.push_scope(ScopeKind::Block);

        if (clause.exception_type) {
            // Typed catch: catch (e: Type)
            Type* catch_type = resolve_type_expr(clause.exception_type);
            if (catch_type && !catch_type->is_error()) {
                Type* base = catch_type->base_type();
                if (!base->is_struct()) {
                    error(clause.loc, "catch type must be a struct type that implements Exception");
                } else {
                    Type* exception_trait = m_type_env.exception_type();
                    if (!m_types.implements_trait(base, exception_trait)) {
                        error_fmt(clause.loc, "catch type '{}' does not implement the Exception trait",
                                  base->struct_info.name);
                    }
                }
                clause.resolved_type = catch_type;
                // Define as ref<Type> in catch scope
                Type* ref_catch_type = m_types.ref_type(catch_type);
                m_symbols.define(SymbolKind::Variable, clause.var_name, ref_catch_type,
                                 clause.loc);
            }
        } else {
            // Catch-all: catch (e)
            has_catch_all = true;
            clause.resolved_type = nullptr;
            // Define as ExceptionRef (opaque handle, only message() callable)
            m_symbols.define(SymbolKind::Variable, clause.var_name,
                             m_types.exception_ref_type(), clause.loc);
        }

        analyze_stmt(clause.body);  // yield is allowed in catch bodies
        m_symbols.pop_scope();

        exit_paths.push_back(save_move_states());
        exit_terminates.push_back(m_branch_terminates);
    }

    // Merge the surviving (non-terminating) exit paths. If every exit path
    // terminates, code after the try is unreachable and all_terminate is true.
    bool all_terminate = merge_branch_snapshots(exit_paths, exit_terminates);

    // Analyze finally body if present (yield is NOT allowed here).
    // Finally runs on every exit path, so its own terminators are the
    // conservative upper bound for the whole try/catch/finally.
    if (ts.finally_body) {
        {
            ScopedValue finally_depth_guard(m_in_finally_depth);
            m_in_finally_depth++;
            m_branch_terminates = false;
            analyze_stmt(ts.finally_body);
        }
        // If finally terminates, so does the whole statement; otherwise
        // use the all_terminate result computed from try+catches.
        if (!m_branch_terminates) {
            m_branch_terminates = all_terminate;
        }
    } else {
        m_branch_terminates = all_terminate;
    }
}

void SemanticAnalyzer::analyze_yield_stmt(Stmt* stmt) {
    YieldStmt& ys = stmt->yield_stmt;

    if (!m_in_coroutine) {
        error(stmt->loc, "'yield' can only appear inside a coroutine function (one returning Coro<T>)");
        return;
    }

    if (m_in_finally_depth > 0) {
        error(stmt->loc, "'yield' inside finally is not supported");
        return;
    }

    Type* actual = analyze_expr(ys.value);
    if (!actual || actual->is_error()) return;

    if (!m_checker.check_assignable(m_coro_yield_type, actual, stmt->loc)) {
        // Error already reported
    } else {
        m_checker.coerce_int_literal(ys.value, m_coro_yield_type);
    }
}

// ===== Expression Analysis =====

Type* SemanticAnalyzer::analyze_expr(Expr* expr) {
    if (!expr) return m_types.error_type();

    Type* result = nullptr;
    switch (expr->kind) {
        case AstKind::ExprLiteral:
            result = analyze_literal_expr(expr);
            break;
        case AstKind::ExprIdentifier:
            result = analyze_identifier_expr(expr);
            break;
        case AstKind::ExprUnary:
            result = analyze_unary_expr(expr);
            break;
        case AstKind::ExprBinary:
            result = analyze_binary_expr(expr);
            break;
        case AstKind::ExprTernary:
            result = analyze_ternary_expr(expr);
            break;
        case AstKind::ExprCall:
            result = analyze_call_expr(expr);
            break;
        case AstKind::ExprIndex:
            result = analyze_index_expr(expr);
            break;
        case AstKind::ExprGet:
            result = analyze_get_expr(expr);
            break;
        case AstKind::ExprStaticGet:
            result = analyze_static_get_expr(expr);
            break;
        case AstKind::ExprAssign:
            result = analyze_assign_expr(expr);
            break;
        case AstKind::ExprGrouping:
            result = analyze_grouping_expr(expr);
            break;
        case AstKind::ExprThis:
            result = analyze_this_expr(expr);
            break;
        case AstKind::ExprSuper:
            result = analyze_super_expr(expr);
            break;
        case AstKind::ExprStructLiteral:
            result = analyze_struct_literal_expr(expr);
            break;
        case AstKind::ExprStringInterp:
            result = analyze_string_interp_expr(expr);
            break;
        case AstKind::ExprLambda:
            result = analyze_lambda_expr(expr);
            break;
        default:
            result = m_types.error_type();
            break;
    }

    // Store the resolved type in the AST node for later use (e.g., IR generation)
    expr->resolved_type = result;
    return result;
}

Type* SemanticAnalyzer::analyze_string_interp_expr(Expr* expr) {
    auto& string_interp = expr->string_interp;
    for (auto& expression : string_interp.expressions) {
        Type* etype = analyze_expr(expression);
        if (!etype || etype->is_error()) continue;
        if (etype->is_void()) {
            error(expression->loc, "cannot interpolate void expression in f-string");
            continue;
        }
        // Coerce IntLiteral to i32 so it has a Printable implementation
        if (etype->is_int_literal()) {
            m_checker.coerce_int_literal(expression, m_types.i32_type());
            etype = expression->resolved_type;
        }
        // Uniform trait check for ALL types (primitives and structs)
        if (!m_types.implements_trait(etype, m_type_env.printable_type())) {
            error_fmt(expression->loc,
                     "type '%s' does not implement Printable (no to_string method)",
                     type_kind_to_string(etype->kind));
        }
    }
    return m_types.string_type();
}

Type* SemanticAnalyzer::analyze_literal_expr(Expr* expr) {
    LiteralExpr& lit = expr->literal;

    switch (lit.literal_kind) {
        case LiteralKind::Nil:
            return m_types.nil_type();
        case LiteralKind::Bool:
            return m_types.bool_type();
        case LiteralKind::I32:
            return m_types.int_literal_type();
        case LiteralKind::I64:
            return m_types.i64_type();
        case LiteralKind::U32:
            return m_types.u32_type();
        case LiteralKind::U64:
            return m_types.u64_type();
        case LiteralKind::F32:
            return m_types.f32_type();
        case LiteralKind::F64:
            return m_types.f64_type();
        case LiteralKind::String:
            return m_types.string_type();
    }

    return m_types.error_type();
}

Type* SemanticAnalyzer::analyze_identifier_expr(Expr* expr) {
    IdentifierExpr& id = expr->identifier;

    // Generic-template refs are resolved via the global GenericInstantiator
    // (shared across all modules), regardless of whether the template is
    // local or imported via `from M import identity`. The check fires before
    // the symbol-table lookup because for imports the SymbolTable carries an
    // ImportedFunction with null type — useless for template instantiation.
    if (m_type_env.generics().is_generic_fun(id.name)) {
        // Explicit-type-args: `identity<i32>`. Instantiate immediately.
        if (id.generic_args.size() > 0) {
            return resolve_explicit_generic_template_ref(expr);
        }
        // Bare template name: defer to the surrounding coerce site
        // (var-init annotation / call-arg / return / struct-field).
        id.is_generic_template_ref = true;
        return m_types.error_type();
    }

    Symbol* sym = m_symbols.lookup(id.name);
    if (!sym) {
        // Helpful error for type names parsed as value-position generic refs.
        if (id.generic_args.size() > 0
            && m_type_env.generics().is_generic_struct(id.name)) {
            error_fmt(expr->loc,
                "'{}' is a struct type, not a value; cannot reference it with "
                "type arguments in expression position", id.name);
            return m_types.error_type();
        }
        error_fmt(expr->loc, "undefined identifier '{}'", id.name);
        return m_types.error_type();
    }

    // Closure-capture path: if this identifier resolves across a lambda
    // boundary, record the capture(s) and rewrite the expr in place.
    Type* captured_type = nullptr;
    if (try_capture_identifier(expr, sym, &captured_type)) {
        return captured_type;
    }

    // Check move state for owned variables (uniq refs and value structs with destructors)
    // Note: we check here but the actual move marking happens at call sites, return, delete
    if (sym->type && sym->type->noncopyable()) {
        check_not_moved(id.name, expr->loc);
    }

    return sym->type;
}

bool SemanticAnalyzer::try_capture_identifier(Expr* expr, Symbol* sym, Type** out) {
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

    // Walk from current scope toward sym->defining_scope, collecting the
    // indices of every lambda context whose boundary scope sits between us
    // and the symbol's defining scope (innermost first). For nested closures
    // a lookup can cross multiple boundaries; each enclosing lambda must
    // also capture the symbol so it can be passed inward through env-fields.
    Vector<u32> crossed_ctx_indices;
    for (Scope* sc = m_symbols.current_scope(); sc; sc = sc->parent) {
        if (sc == sym->defining_scope) break;
        if (sc->kind == ScopeKind::Lambda) {
            for (u32 i = 0; i < m_lambda_contexts.size(); i++) {
                if (m_lambda_contexts[i]->boundary_scope == sc) {
                    crossed_ctx_indices.push_back(i);
                    break;
                }
            }
        }
    }

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
            error_fmt(captured_loc,
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
        if (!check_not_moved(captured_name, captured_loc)) {
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

                Expr* env_id = make_identifier_expr(StringView("__env", 5),
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

    Expr* env_id = make_identifier_expr(StringView("__env", 5),
                                        innermost_env_ref, captured_loc);

    expr->kind = AstKind::ExprGet;
    expr->get.object = env_id;
    expr->get.name = captured_name;
    *out = captured_type;
    return true;
}

// Helper: allocate a NUL-free StringView in the bump allocator.
static StringView alloc_view(BumpAllocator& alloc, const char* str) {
    u32 len = 0;
    while (str[len]) ++len;
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

Expr* SemanticAnalyzer::make_identifier_expr(StringView name, Type* type, SourceLocation loc) {
    Expr* e = m_allocator.emplace<Expr>();
    e->kind = AstKind::ExprIdentifier;
    e->loc = loc;
    e->identifier.name = name;
    e->resolved_type = type;
    return e;
}

Expr* SemanticAnalyzer::make_get_expr(Expr* object, StringView name, Type* type, SourceLocation loc) {
    Expr* e = m_allocator.emplace<Expr>();
    e->kind = AstKind::ExprGet;
    e->loc = loc;
    e->get.object = object;
    e->get.name = name;
    e->resolved_type = type;
    return e;
}

Expr* SemanticAnalyzer::make_this_expr(Type* type, SourceLocation loc) {
    Expr* e = m_allocator.emplace<Expr>();
    e->kind = AstKind::ExprThis;
    e->loc = loc;
    e->resolved_type = type;
    return e;
}

void SemanticAnalyzer::ensure_self_captured_through(u32 target_idx, Type* struct_type,
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
    bool copyable_struct = !struct_type->noncopyable();

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

        Expr* env_id = make_identifier_expr(StringView("__env", 5), outer_env_ref, loc);
        src = make_get_expr(env_id, StringView("__self", 6), ref_self, loc);
    }

    CaptureInfo info{};
    info.name = StringView("__self", 6);
    info.type = ref_self;
    info.mode = CaptureMode::Copy;       // ref pointer copied
    info.source_symbol = nullptr;
    info.loc = loc;
    info.source_expr = src;
    info.needs_heap_check = copyable_struct;

    ctx.self_capture_index = static_cast<u32>(ctx.captures.size());
    ctx.captures.push_back(info);
    ctx.has_self_capture = true;
}

Type* SemanticAnalyzer::analyze_lambda_expr(Expr* expr) {
    LambdaExpr& le = expr->lambda;

    // Resolve user-facing param and return types (the `Function<sig>` type).
    Vector<Type*> sig_param_types;
    for (auto& p : le.params) {
        Type* pt = resolve_type_expr(p.type);
        if (!pt || pt->is_error()) return m_types.error_type();
        sig_param_types.push_back(pt);
    }
    Type* ret_type = le.return_type ? resolve_type_expr(le.return_type) : m_types.void_type();
    if (!ret_type || ret_type->is_error()) return m_types.error_type();

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
            mark_moved(cap.name);
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

bool SemanticAnalyzer::validate_lambda_captures(LambdaExpr& le, LambdaCaptureContext& context) {
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
                error_fmt(entry.loc, "capture list references unknown variable '{}'", entry.name);
                return false;
            }
            if (!outer_sym->type || !outer_sym->type->noncopyable()) {
                error_fmt(entry.loc,
                    "move captures only apply to noncopyable types; '{}' is copyable, capture it implicitly",
                    entry.name);
                return false;
            }
            if (context.by_symbol.find(outer_sym) != context.by_symbol.end()) {
                error_fmt(entry.loc, "duplicate capture entry for '{}'", entry.name);
                return false;
            }
            if (!check_not_moved(entry.name, entry.loc)) return false;

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
                for (Scope* sc = m_symbols.current_scope(); sc; sc = sc->parent) {
                    if (sc == outer_sym->defining_scope) break;
                    if (sc->kind == ScopeKind::Lambda) {
                        for (u32 i = 0; i < m_lambda_contexts.size(); i++) {
                            if (m_lambda_contexts[i]->boundary_scope == sc) {
                                crossed_ctx_indices.push_back(i);
                                break;
                            }
                        }
                    }
                }
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
                Expr* env_id = make_identifier_expr(StringView("__env", 5),
                                                    enclosing_env_ref, entry.loc);
                return make_get_expr(env_id, entry.name, outer_sym->type, entry.loc);
            };

            // Add Move captures to crossed enclosing contexts (outermost
            // first). Each one's source reads from the next outer level.
            for (i32 i = static_cast<i32>(crossed_ctx_indices.size()) - 1; i >= 0; i--) {
                u32 ctx_idx = crossed_ctx_indices[i];
                LambdaCaptureContext& ctx = *m_lambda_contexts[ctx_idx];
                if (ctx.by_symbol.find(outer_sym) != ctx.by_symbol.end()) {
                    error_fmt(entry.loc,
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
        if (entry.name != StringView("self", 4)) {
            error_fmt(entry.loc,
                "[copy ...] / [weak ...] captures are currently restricted to 'self'");
            return false;
        }
        if (context.has_self_capture) {
            error(entry.loc, "duplicate self capture in capture list");
            return false;
        }

        Type* struct_type = self_lambda_method_struct();
        if (!struct_type) {
            error_fmt(entry.loc,
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

            Expr* env_id = make_identifier_expr(StringView("__env", 5), outer_env_ref, loc);
            return make_get_expr(env_id, StringView("__self", 6), ref_self, loc);
        };

        if (entry.mode == CaptureMode::Copy) {
            if (struct_type->noncopyable()) {
                error_fmt(entry.loc,
                    "cannot [copy self] of noncopyable struct '{}'; use [weak self] instead",
                    struct_type->struct_info.name);
                return false;
            }
            if (struct_type->struct_info.when_clauses.size() > 0) {
                error_fmt(entry.loc,
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
            info.name = StringView("__self", 6);
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
            // For nested cases, the source comes through outer's __env (a heap
            // ref already), so the receiver-on-heap requirement is satisfied
            // transitively. For direct method nesting on a copyable struct we
            // still need the runtime check on the bare ExprThis.
            bool copyable_struct = !struct_type->noncopyable();
            bool nested = m_lambda_contexts.size() > 0;

            Expr* src = build_outer_self_ref_source(entry.loc);

            CaptureInfo info{};
            info.name = StringView("__self", 6);
            info.type = weak_self;
            info.mode = CaptureMode::Weak;
            info.source_symbol = nullptr;
            info.loc = entry.loc;
            info.source_expr = src;
            info.needs_heap_check = copyable_struct && !nested;

            context.self_capture_index = static_cast<u32>(context.captures.size());
            context.captures.push_back(info);
            context.has_self_capture = true;
        }
    }
    return true;
}

Decl* SemanticAnalyzer::synthesize_lambda_call_fn(Expr* expr, LambdaExpr& le,
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
        // Analyze the lambda body with a fresh move-state map and outside any
        // enclosing coroutine context; the guards restore both at block end.
        ScopedValue move_states_guard(m_move_states);
        m_move_states.clear();

        ScopedValue coro_guard(m_in_coroutine);
        ScopedValue yield_guard(m_coro_yield_type);
        m_in_coroutine = false;
        m_coro_yield_type = nullptr;

        m_symbols.push_function_scope(ret_type);

        for (u32 i = 0; i < fd.params.size(); i++) {
            Param& p = fd.params[i];
            Type* ptype = resolve_type_expr(p.type);
            if (!ptype) ptype = m_types.error_type();
            p.resolved_type = ptype;
            if (m_symbols.lookup_local(p.name)) {
                error_fmt(p.loc, "duplicate parameter name '{}'", p.name);
            } else {
                m_symbols.define_parameter(p.name, ptype, p.loc, i);
            }
            if (ptype && ptype->noncopyable()) {
                Symbol* psym = m_symbols.lookup(p.name);
                if (psym) m_move_states[psym] = MoveState::Live;
            }
        }

        analyze_stmt(fd.body);

        check_scope_exit_uniq_destructors(m_symbols.current_scope(), expr->loc);
        m_symbols.pop_scope();  // function scope
        // coro_guard / yield_guard / move_states_guard restore at block end.
    }
    m_lambda_contexts.pop_back();
    m_symbols.pop_scope();  // lambda boundary scope
    return synth_decl;
}

void SemanticAnalyzer::backfill_lambda_env(Type* env_type, const LambdaCaptureContext& context) {
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
        if (cap.type && cap.type->noncopyable()) any_noncopyable = true;
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
}

Type* SemanticAnalyzer::analyze_unary_expr(Expr* expr) {
    UnaryExpr& unary_expr = expr->unary;

    Type* operand_type = analyze_expr(unary_expr.operand);
    if (operand_type->is_error()) return m_types.error_type();

    // Handle ref expression: borrow a reference from an lvalue
    if (unary_expr.op == UnaryOp::Ref) {
        if (!is_lvalue(unary_expr.operand)) {
            error(expr->loc, "'ref' requires an lvalue operand");
            return m_types.error_type();
        }

        // ref of uniq<T> → ref<T>
        if (operand_type->kind == TypeKind::Uniq) {
            return m_types.ref_type(operand_type->ref_info.inner_type);
        }
        // ref of ref<T> → ref<T> (identity)
        if (operand_type->kind == TypeKind::Ref) {
            return operand_type;
        }

        error_fmt(expr->loc, "'ref' requires a 'uniq' or 'ref' operand, got '{}'",
                  m_checker.type_string(operand_type));
        return m_types.error_type();
    }

    return get_unary_result_type(unary_expr.op, operand_type, expr->loc);
}

Type* SemanticAnalyzer::analyze_binary_expr(Expr* expr) {
    BinaryExpr& binary_expr = expr->binary;

    Type* left_type = analyze_expr(binary_expr.left);
    Type* right_type = analyze_expr(binary_expr.right);

    if (left_type->is_error() || right_type->is_error()) {
        return m_types.error_type();
    }

    // Coerce int literals: match the concrete side, or default both to i32
    if (left_type->is_int_literal() && right_type->is_integer()) {
        m_checker.coerce_int_literal(binary_expr.left, right_type);
        left_type = right_type;
    } else if (right_type->is_int_literal() && left_type->is_integer()) {
        m_checker.coerce_int_literal(binary_expr.right, left_type);
        right_type = left_type;
    } else if (left_type->is_int_literal() && right_type->is_int_literal()) {
        m_checker.coerce_int_literal(binary_expr.left, m_types.i32_type());
        m_checker.coerce_int_literal(binary_expr.right, m_types.i32_type());
        left_type = m_types.i32_type();
        right_type = m_types.i32_type();
    } else if (right_type->is_int_literal() && !left_type->is_int_literal()) {
        // Right is IntLiteral, left is non-integer (e.g., struct) — coerce to method's param type
        const char* method_name = binary_op_to_trait_method(binary_expr.op);
        if (method_name) {
            StringView name(method_name, static_cast<u32>(strlen(method_name)));
            const MethodInfo* mi = m_types.lookup_method(left_type, name);
            if (mi && mi->param_types.size() == 1 && mi->param_types[0]->is_integer()) {
                m_checker.coerce_int_literal(binary_expr.right, mi->param_types[0]);
                right_type = mi->param_types[0];
            }
        }
    } else if (left_type->is_int_literal() && !right_type->is_int_literal()) {
        // Left is IntLiteral, right is non-integer — coerce to method's param type
        const char* method_name = binary_op_to_trait_method(binary_expr.op);
        if (method_name) {
            StringView name(method_name, static_cast<u32>(strlen(method_name)));
            const MethodInfo* mi = m_types.lookup_method(right_type, name);
            if (mi && mi->param_types.size() == 1 && mi->param_types[0]->is_integer()) {
                m_checker.coerce_int_literal(binary_expr.left, mi->param_types[0]);
                left_type = mi->param_types[0];
            }
        }
    }

    return get_binary_result_type(binary_expr.op, left_type, right_type, expr->loc);
}

Type* SemanticAnalyzer::analyze_ternary_expr(Expr* expr) {
    TernaryExpr& ternary_expr = expr->ternary;

    Type* cond_type = analyze_expr(ternary_expr.condition);
    if (!cond_type->is_error()) {
        m_checker.check_boolean(cond_type, ternary_expr.condition->loc);
    }

    // Save/restore/merge move states across the two branches, mirroring
    // analyze_if_stmt. Only one branch executes at runtime, so an analysis
    // that observed both linearly would produce spurious use-after-move
    // errors and miss conditional moves. Ternary branches are expressions
    // so they cannot contain return/throw/break/continue — no termination
    // flag handling is required.
    MoveStateSnapshot pre_branch_states = save_move_states();

    Type* then_type = analyze_expr(ternary_expr.then_expr);
    MoveStateSnapshot then_states = save_move_states();

    restore_move_states(pre_branch_states);
    Type* else_type = analyze_expr(ternary_expr.else_expr);
    MoveStateSnapshot else_states = save_move_states();

    restore_move_states(pre_branch_states);
    merge_move_states(then_states, else_states);

    if (then_type->is_error()) return else_type;
    if (else_type->is_error()) return then_type;

    // Coerce int literals in ternary branches
    if (then_type->is_int_literal() && else_type->is_integer()) {
        m_checker.coerce_int_literal(ternary_expr.then_expr, else_type);
        then_type = else_type;
    } else if (else_type->is_int_literal() && then_type->is_integer()) {
        m_checker.coerce_int_literal(ternary_expr.else_expr, then_type);
        else_type = then_type;
    } else if (then_type->is_int_literal() && else_type->is_int_literal()) {
        m_checker.coerce_int_literal(ternary_expr.then_expr, m_types.i32_type());
        m_checker.coerce_int_literal(ternary_expr.else_expr, m_types.i32_type());
        then_type = m_types.i32_type();
        else_type = m_types.i32_type();
    }

    // Types must be compatible
    if (then_type == else_type) {
        return then_type;
    }

    // Check if one can be converted to the other (probe without errors)
    if (m_checker.is_assignable(then_type, else_type)) {
        return then_type;
    }
    if (m_checker.is_assignable(else_type, then_type)) {
        return else_type;
    }

    error(expr->loc, "incompatible types in ternary expression");
    return m_types.error_type();
}

void SemanticAnalyzer::check_call_args(Span<CallArg> args, Span<Type*> param_types,
                                       Span<Param> params, SourceLocation loc) {
    for (u32 i = 0; i < args.size(); i++) {
        CallArg& arg = args[i];

        // Get expected modifier from params (if available)
        ParamModifier expected_mod = ParamModifier::None;
        if (params.data() && i < params.size()) {
            expected_mod = params[i].modifier;
        }

        // Check modifier matches
        if (expected_mod != arg.modifier) {
            if (expected_mod == ParamModifier::Out) {
                error(arg.expr->loc, "argument requires 'out' modifier");
            } else if (expected_mod == ParamModifier::Inout) {
                error(arg.expr->loc, "argument requires 'inout' modifier");
            } else if (arg.modifier != ParamModifier::None) {
                error(arg.modifier_loc, "unexpected modifier for this parameter");
            }
        }

        // Check lvalue requirement for out/inout
        if (arg.modifier == ParamModifier::Out || arg.modifier == ParamModifier::Inout) {
            if (!is_lvalue(arg.expr)) {
                error(arg.expr->loc, "'out'/'inout' argument must be a variable");
            }
        }

        // Analyze argument expression
        Type* arg_type = analyze_expr(arg.expr);

        // Resolve generic-template-ref arg against param type before
        // assignability checking. Updates arg_type via resolved_type.
        if (param_types[i] && coerce_generic_template_ref(arg.expr, param_types[i])) {
            arg_type = arg.expr->resolved_type;
        }

        // Type check (skip for 'out' since it's write-only)
        if (arg.modifier != ParamModifier::Out) {
            m_checker.check_assignable(param_types[i], arg_type, arg.expr->loc);
            m_checker.coerce_int_literal(arg.expr, param_types[i]);
        }

        // Move semantics: passing owned arg to owned param transfers ownership —
        // but only for by-value arguments. `inout` and `out` borrow through a
        // pointer; the callee sees the same slot the caller still owns, so
        // treating them as a move falsely rejects loops like
        // `while (...) { fn(inout xs); }` with "moved in loop body without
        // reassignment", even though `xs` stays live across iterations.
        if (param_types[i] && param_types[i]->noncopyable()
            && arg.modifier == ParamModifier::None) {
            consume_noncopyable(arg.expr, arg.expr->loc);
        }
    }
}

Type* SemanticAnalyzer::build_method_function_type(Type* self_type, const MethodInfo* method_info) {
    u32 total_params = 1 + method_info->param_types.size();
    Type** ptypes = reinterpret_cast<Type**>(
        m_allocator.alloc_bytes(sizeof(Type*) * total_params, alignof(Type*)));
    ptypes[0] = m_types.ref_type(self_type);
    for (u32 j = 0; j < method_info->param_types.size(); j++) {
        ptypes[j + 1] = method_info->param_types[j];
    }
    return m_types.function_type(Span<Type*>(ptypes, total_params), method_info->return_type);
}

// ============================================================================
// Generic type argument inference
// ============================================================================

bool SemanticAnalyzer::unify_type_expr(TypeExpr* pattern, Type* concrete,
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

InferredTypeArgs SemanticAnalyzer::infer_type_args_from_call(
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
        Type* arg_type = analyze_expr(args[i].expr);
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

InferredTypeArgs SemanticAnalyzer::infer_type_args_from_fields(
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

        Type* value_type = analyze_expr(literal_fields[i].value);
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

Type* SemanticAnalyzer::analyze_generic_fun_call(Expr* expr, CallExpr& ce, StringView func_name) {
    Decl* template_decl = m_type_env.generics().get_generic_fun_decl(func_name);
    FunDecl& template_fun_decl = template_decl->fun_decl;

    // Validate type arg count
    if (ce.type_args.size() != template_fun_decl.type_params.size()) {
        error_fmt(expr->loc, "generic function '{}' expects {} type arguments but got {}",
                 func_name, template_fun_decl.type_params.size(), ce.type_args.size());
        return m_types.error_type();
    }

    // Resolve type args to concrete types
    Vector<Type*> type_arg_types;
    for (auto& type_arg : ce.type_args) {
        Type* arg_type = resolve_type_expr(type_arg);
        if (!arg_type || arg_type->is_error()) return m_types.error_type();
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

Type* SemanticAnalyzer::analyze_generic_fun_call_inferred(Expr* expr, CallExpr& ce, StringView func_name) {
    Decl* template_decl = m_type_env.generics().get_generic_fun_decl(func_name);
    FunDecl& template_fun_decl = template_decl->fun_decl;

    // Infer type args from the call arguments (this analyzes each argument).
    InferredTypeArgs inferred = infer_type_args_from_call(
        template_fun_decl.type_params, template_fun_decl.params,
        ce.arguments, expr->loc);
    if (!inferred.success) {
        error_fmt(expr->loc,
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

Type* SemanticAnalyzer::check_instantiated_generic_call(
        Expr* expr, CallExpr& ce, StringView func_name,
        GenericFunInstance* inst, bool args_pre_analyzed) {
    FunDecl& inst_fun_decl = inst->instantiated_decl->fun_decl;

    if (ce.arguments.size() != inst_fun_decl.params.size()) {
        error_fmt(expr->loc, "function '{}' expects {} arguments but got {}",
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
                                           : analyze_expr(arg.expr);

        Type* param_type = nullptr;
        if (inst_fun_decl.params[i].type) {
            param_type = resolve_type_expr(inst_fun_decl.params[i].type);
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
            consume_noncopyable(arg.expr, arg.expr->loc);
        }
    }

    // Resolve return type from the instantiated function.
    Type* return_type = m_types.void_type();
    if (inst_fun_decl.return_type) {
        return_type = resolve_type_expr(inst_fun_decl.return_type);
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

void SemanticAnalyzer::populate_enum_methods(Type* type) {
    assert(type && type->is_enum());

    // eq(other: Self): bool, ne(other: Self): bool
    Span<Type*> self_param(m_allocator.emplace<Type*>(type), 1);
    Vector<MethodInfo> methods;
    methods.push_back(make_method(StringView("eq", 2), self_param, m_types.bool_type()));
    methods.push_back(make_method(StringView("ne", 2), self_param, m_types.bool_type()));
    type->enum_info.methods = m_allocator.alloc_span(methods);
}

NativeRegistry* SemanticAnalyzer::get_builtin_registry() {
    ModuleInfo* builtin = m_modules.find_module(BUILTIN_MODULE_NAME);
    NativeRegistry* registry = builtin ? builtin->natives : nullptr;
    if (!registry) registry = m_registry;
    return registry;
}

void SemanticAnalyzer::populate_coro_methods(Type* type) {
    assert(type && type->is_coroutine());
    if (type->coro_info.methods.size() > 0) return;

    Type* yield_type = type->coro_info.yield_type;
    StringView func_name = type->coro_info.func_name;

    // Build mangled names: __coro_<func_name>$$resume, __coro_<func_name>$$done
    // These match the names generated by the coroutine lowering pass.
    StringView resume_native = format_to_arena(m_allocator, "__coro_{}$$resume", func_name);
    StringView done_native = format_to_arena(m_allocator, "__coro_{}$$done", func_name);

    // resume() -> yield_type, done() -> bool (no params; self is implicit)
    Vector<MethodInfo> methods;
    methods.push_back(make_method(StringView("resume", 6), Span<Type*>(), yield_type, resume_native));
    methods.push_back(make_method(StringView("done", 4), Span<Type*>(), m_types.bool_type(), done_native));
    type->coro_info.methods = m_allocator.alloc_span(methods);
}

void SemanticAnalyzer::populate_container_methods(
        const char* registry_name, Span<Type*> type_args,
        Span<MethodInfo>& out_methods,
        StringView& out_alloc_name, StringView& out_copy_name) {
    if (out_methods.size() > 0) return;

    NativeRegistry* registry = get_builtin_registry();
    if (!registry) return;

    out_methods = registry->instantiate_generic_methods(registry_name, type_args, m_allocator, m_types);
    out_alloc_name = registry->get_generic_alloc_name(registry_name);
    out_copy_name = registry->get_generic_copy_name(registry_name);
}

void SemanticAnalyzer::populate_list_methods(Type* type) {
    assert(type && type->is_list());
    Type* type_args[] = { type->list_info.element_type };
    populate_container_methods("List", Span<Type*>(type_args, 1),
                               type->list_info.methods,
                               type->list_info.alloc_native_name,
                               type->list_info.copy_native_name);
}

Type* SemanticAnalyzer::analyze_list_constructor_call(Expr* expr, CallExpr& ce) {
    if (ce.type_args.size() != 1) {
        error(expr->loc, "List requires exactly 1 type argument");
        return m_types.error_type();
    }
    Type* elem_type = resolve_type_expr(ce.type_args[0]);
    if (!elem_type || elem_type->is_error()) return m_types.error_type();

    Type* list_type = m_types.list_type(elem_type);
    populate_list_methods(list_type);

    NativeRegistry* registry = get_builtin_registry();
    if (!registry) {
        error(expr->loc, "no native registry available for List constructor");
        return m_types.error_type();
    }

    Type* type_args[] = { elem_type };
    ResolvedConstructor ctor = registry->instantiate_generic_constructor(
        "List", Span<Type*>(type_args, 1), m_allocator, m_types);

    if (ctor.native_name.empty()) {
        error(expr->loc, "List has no registered constructor");
        return m_types.error_type();
    }

    if (ce.arguments.size() < ctor.min_args ||
        ce.arguments.size() > ctor.param_types.size()) {
        error_fmt(expr->loc, "List constructor takes {} to {} arguments but got {}",
                  ctor.min_args, ctor.param_types.size(), ce.arguments.size());
        return m_types.error_type();
    }

    for (u32 i = 0; i < ce.arguments.size(); i++) {
        Type* arg_type = analyze_expr(ce.arguments[i].expr);
        if (arg_type && !arg_type->is_error()) {
            m_checker.check_assignable(ctor.param_types[i], arg_type, ce.arguments[i].expr->loc);
        }
    }

    ce.mangled_name = ctor.native_name;
    ce.callee->resolved_type = list_type;
    return list_type;
}

bool SemanticAnalyzer::is_hashable_key_type(Type* type) {
    if (!type) return false;
    // Primitives with Hash trait
    if (type->is_primitive()) return true;
    // Enums use i32 underlying, always hashable
    if (type->is_enum()) return true;
    // Struct keys: allowed via bytewise hash + memcmp at the runtime level
    // (MapKeyKind::Struct). Custom Hash/Eq trait impls on structs are not yet
    // dispatched by the map runtime — bytewise is the only behavior. This is
    // sufficient for POD struct keys (Roxy structs are slot-aligned with no
    // compiler padding); structs containing pointers (e.g. embedded String)
    // hash by pointer identity, not content, so users wanting content-based
    // keys should normalise into a primitive key (e.g. interned String).
    if (type->is_struct()) return true;
    return false;
}

void SemanticAnalyzer::populate_map_methods(Type* type) {
    assert(type && type->is_map());
    Type* type_args[] = { type->map_info.key_type, type->map_info.value_type };
    populate_container_methods("Map", Span<Type*>(type_args, 2),
                               type->map_info.methods,
                               type->map_info.alloc_native_name,
                               type->map_info.copy_native_name);
}

Type* SemanticAnalyzer::analyze_map_constructor_call(Expr* expr, CallExpr& ce) {
    if (ce.type_args.size() != 2) {
        error(expr->loc, "Map requires exactly 2 type arguments");
        return m_types.error_type();
    }
    Type* key_type = resolve_type_expr(ce.type_args[0]);
    Type* val_type = resolve_type_expr(ce.type_args[1]);
    if (!key_type || key_type->is_error()) return m_types.error_type();
    if (!val_type || val_type->is_error()) return m_types.error_type();

    if (!is_hashable_key_type(key_type)) {
        error(expr->loc, "Map key type must implement Hash");
        return m_types.error_type();
    }

    Type* map_type = m_types.map_type(key_type, val_type);
    populate_map_methods(map_type);

    NativeRegistry* registry = get_builtin_registry();
    if (!registry) {
        error(expr->loc, "no native registry available for Map constructor");
        return m_types.error_type();
    }

    Type* type_arg_array[] = { key_type, val_type };
    ResolvedConstructor ctor = registry->instantiate_generic_constructor(
        "Map", Span<Type*>(type_arg_array, 2), m_allocator, m_types);

    if (ctor.native_name.empty()) {
        error(expr->loc, "Map has no registered constructor");
        return m_types.error_type();
    }

    // Map constructor accepts: Map<K,V>() or Map<K,V>(capacity)
    // The semantic layer passes user arguments directly (0 or 1 capacity arg).
    // The hidden key_kind argument is injected at IR generation time.
    if (ce.arguments.size() > 1) {
        error_fmt(expr->loc, "Map constructor takes 0 to 1 arguments but got {}",
                  ce.arguments.size());
        return m_types.error_type();
    }

    for (u32 i = 0; i < ce.arguments.size(); i++) {
        Type* arg_type = analyze_expr(ce.arguments[i].expr);
        if (arg_type && !arg_type->is_error()) {
            m_checker.check_assignable(m_types.i32_type(), arg_type, ce.arguments[i].expr->loc);
        }
    }

    ce.mangled_name = ctor.native_name;
    ce.callee->resolved_type = map_type;
    return map_type;
}

Type* SemanticAnalyzer::analyze_generic_struct_constructor_call(Expr* expr, CallExpr& ce, StringView func_name) {
    // Resolve type args
    Vector<Type*> type_arg_types;
    for (auto& type_arg : ce.type_args) {
        Type* arg_type = resolve_type_expr(type_arg);
        if (!arg_type || arg_type->is_error()) return m_types.error_type();
        type_arg_types.push_back(arg_type);
    }

    // Check trait bounds on type args
    Span<Type*> type_args = m_allocator.alloc_span(type_arg_types);
    const ResolvedTypeParams* bounds = m_type_env.generics().get_struct_bounds(func_name);
    if (!check_type_arg_bounds(func_name, type_args, bounds, expr->loc)) {
        return m_types.error_type();
    }

    // Instantiate the struct
    StringView mangled = m_type_env.generics().instantiate_struct(func_name, type_args);
    GenericStructInstance* inst = m_type_env.generics().find_struct_instance(mangled);
    Type* struct_type = inst->concrete_type;

    ce.mangled_name = mangled;
    ce.callee->resolved_type = struct_type;
    return analyze_constructor_call(expr, struct_type, ce.constructor_name, ce.is_heap);
}

Type* SemanticAnalyzer::analyze_super_call(Expr* expr, CallExpr& ce) {
    SuperExpr& super_expr = ce.callee->super_expr;

    if (!m_symbols.is_in_struct()) {
        error(expr->loc, "'super' used outside of struct context");
        return m_types.error_type();
    }

    Type* struct_type = m_symbols.current_struct_type();
    Type* parent_type = struct_type->struct_info.parent;

    if (!parent_type) {
        error(expr->loc, "'super' used in struct with no parent");
        return m_types.error_type();
    }

    StructTypeInfo& parent_struct_type_info = parent_type->struct_info;

    // If method_name is empty, this is super() or super(args) - constructor call
    if (super_expr.method_name.empty()) {
        const ConstructorInfo* ctor = find_constructor(parent_struct_type_info.constructors, {});

        // If no default constructor but there are args, error
        if (!ctor && ce.arguments.size() > 0) {
            error_fmt(expr->loc, "parent struct '{}' has no constructor that takes arguments",
                     parent_struct_type_info.name);
            return m_types.void_type();
        }

        // Type-check arguments if constructor found
        if (ctor) {
            if (ce.arguments.size() != ctor->param_types.size()) {
                error_fmt(expr->loc, "parent constructor expects {} arguments but got {}",
                         ctor->param_types.size(), ce.arguments.size());
                return m_types.void_type();
            }

            for (u32 i = 0; i < ce.arguments.size(); i++) {
                CallArg& arg = ce.arguments[i];
                Type* arg_type = analyze_expr(arg.expr);
                if (!m_checker.check_assignable(ctor->param_types[i], arg_type, arg.expr->loc)) {
                    // Error already reported
                } else {
                    m_checker.coerce_int_literal(arg.expr, ctor->param_types[i]);
                }
            }
        }

        // Store parent type in callee for IR builder
        ce.callee->resolved_type = m_types.ref_type(parent_type);
        return m_types.void_type();
    }

    // method_name is not empty - this is super.name()
    // First try to find it as a constructor, then as a method
    const ConstructorInfo* ctor = find_constructor(parent_struct_type_info.constructors, super_expr.method_name);

    if (ctor) {
        // It's a named constructor call
        if (ce.arguments.size() != ctor->param_types.size()) {
            error_fmt(expr->loc, "parent constructor expects {} arguments but got {}",
                     ctor->param_types.size(), ce.arguments.size());
            return m_types.void_type();
        }

        for (u32 i = 0; i < ce.arguments.size(); i++) {
            CallArg& arg = ce.arguments[i];
            Type* arg_type = analyze_expr(arg.expr);
            if (!m_checker.check_assignable(ctor->param_types[i], arg_type, arg.expr->loc)) {
                // Error already reported
            } else {
                m_checker.coerce_int_literal(arg.expr, ctor->param_types[i]);
            }
        }

        ce.callee->resolved_type = m_types.ref_type(parent_type);
        return m_types.void_type();
    }

    // Not a constructor - try method
    Type* found_in_type = nullptr;
    const MethodInfo* mi = lookup_method_in_hierarchy(parent_type, super_expr.method_name, &found_in_type);

    if (mi) {
        // It's a super method call
        // Type-check arguments
        if (ce.arguments.size() != mi->param_types.size()) {
            error_fmt(expr->loc, "method '{}' expects {} arguments but got {}",
                     super_expr.method_name, mi->param_types.size(), ce.arguments.size());
            return mi->return_type;
        }

        for (u32 i = 0; i < ce.arguments.size(); i++) {
            CallArg& arg = ce.arguments[i];
            Type* arg_type = analyze_expr(arg.expr);
            if (!m_checker.check_assignable(mi->param_types[i], arg_type, arg.expr->loc)) {
                // Error already reported
            } else {
                m_checker.coerce_int_literal(arg.expr, mi->param_types[i]);
            }
        }

        // Store found_in_type for IR builder to mangle correctly
        ce.callee->resolved_type = m_types.ref_type(found_in_type);
        return mi->return_type;
    }

    // Neither constructor nor method
    error_fmt(expr->loc, "parent struct '{}' has no constructor or method '{}'",
             parent_struct_type_info.name, super_expr.method_name);
    return m_types.error_type();
}

Type* SemanticAnalyzer::analyze_builtin_method_call(Expr* expr, CallExpr& ce, GetExpr& ge,
                                                     Type* obj_type, const MethodInfo* mi) {
    if (ce.arguments.size() != mi->param_types.size()) {
        error_fmt(expr->loc, "{}() takes {} argument{} but got {}",
                 mi->name, mi->param_types.size(),
                 mi->param_types.size() == 1 ? "" : "s",
                 ce.arguments.size());
        return mi->return_type;
    }
    check_call_args(ce.arguments, mi->param_types, Span<Param>(), expr->loc);
    ce.mangled_name = mi->native_name;
    ge.object->resolved_type = obj_type;

    // Set callee's resolved_type to a function type for IR builder move tracking
    Type* base_type = obj_type->base_type();
    ce.callee->resolved_type = build_method_function_type(base_type, mi);

    return mi->return_type;
}

Type* SemanticAnalyzer::analyze_struct_method_call(Expr* expr, CallExpr& ce, GetExpr& ge,
                                                    Type* obj_type, Type* base_type) {
    // Look for a method with this name (walks inheritance hierarchy)
    Type* found_in_type = nullptr;
    const MethodInfo* mi = lookup_method_in_hierarchy(base_type, ge.name, &found_in_type);
    if (!mi) return nullptr;  // Not a method - caller will fall through

    // Check argument count (NOT including implicit self)
    if (ce.arguments.size() != mi->param_types.size()) {
        error_fmt(expr->loc, "method expects {} arguments but got {}",
                 mi->param_types.size(), ce.arguments.size());
        return mi->return_type;
    }

    // Get params for modifier info
    MethodDecl* method_decl = mi->decl ? &mi->decl->method_decl : nullptr;
    Span<Param> params = method_decl ? method_decl->params : Span<Param>();
    check_call_args(ce.arguments, mi->param_types, params, expr->loc);

    // Set callee's resolved_type to a function type for IR builder
    ce.callee->resolved_type = build_method_function_type(found_in_type, mi);

    // Store the struct type where method was found in the GetExpr
    // IR builder will use this for correct method name mangling
    ge.object->resolved_type = obj_type;  // Keep original object type

    return mi->return_type;
}

Type* SemanticAnalyzer::analyze_regular_fun_call(Expr* expr, CallExpr& ce) {
    Type* callee_type = analyze_expr(ce.callee);
    if (callee_type->is_error()) return m_types.error_type();

    if (!callee_type->is_function()) {
        error(ce.callee->loc, "expression is not callable");
        return m_types.error_type();
    }

    FunctionTypeInfo& fti = callee_type->func_info;

    // Check argument count
    if (ce.arguments.size() != fti.param_types.size()) {
        error_fmt(expr->loc, "expected {} arguments but got {}",
                 fti.param_types.size(), ce.arguments.size());
        return fti.return_type;
    }

    // Try to get the FunDecl to access parameter modifiers
    Span<Param> params;
    if (ce.callee->kind == AstKind::ExprIdentifier) {
        Symbol* sym = m_symbols.lookup(ce.callee->identifier.name);
        if (sym && sym->kind == SymbolKind::Function && sym->decl) {
            params = sym->decl->fun_decl.params;
        }
    }

    check_call_args(ce.arguments, fti.param_types, params, expr->loc);
    return fti.return_type;
}

Type* SemanticAnalyzer::analyze_call_expr(Expr* expr) {
    CallExpr& call_expr = expr->call;

    if (!call_expr.callee) return m_types.error_type();

    // Primitive type cast: i32(x), f64(x), bool(x)
    if (call_expr.callee->kind == AstKind::ExprIdentifier) {
        StringView type_name = call_expr.callee->identifier.name;
        Type* target_type = m_types.primitive_by_name(type_name);
        if (target_type != nullptr && !target_type->is_void() && !target_type->is_error()) {
            return analyze_primitive_cast(expr, target_type);
        }
    }

    // Generic call with type args: name<T>(args)
    if (call_expr.type_args.size() > 0 && call_expr.callee->kind == AstKind::ExprIdentifier) {
        StringView func_name = call_expr.callee->identifier.name;

        if (m_type_env.generics().is_generic_fun(func_name)) {
            return analyze_generic_fun_call(expr, call_expr, func_name);
        }
        if (func_name == "List") {
            return analyze_list_constructor_call(expr, call_expr);
        }
        if (func_name == "Map") {
            return analyze_map_constructor_call(expr, call_expr);
        }
        if (m_type_env.generics().is_generic_struct(func_name)) {
            return analyze_generic_struct_constructor_call(expr, call_expr, func_name);
        }
    }

    // Generic function call WITHOUT explicit type args — infer them.
    if (call_expr.type_args.size() == 0 && call_expr.callee->kind == AstKind::ExprIdentifier) {
        StringView func_name = call_expr.callee->identifier.name;
        if (m_type_env.generics().is_generic_fun(func_name)) {
            return analyze_generic_fun_call_inferred(expr, call_expr, func_name);
        }
    }

    // Constructor call: Type(args)
    if (call_expr.callee->kind == AstKind::ExprIdentifier) {
        StringView type_name = call_expr.callee->identifier.name;
        Type* ctor_type = m_type_env.named_type_by_name(type_name);
        if (ctor_type && ctor_type->is_struct()) {
            call_expr.callee->resolved_type = ctor_type;
            return analyze_constructor_call(expr, ctor_type, call_expr.constructor_name, call_expr.is_heap);
        }
    }

    // Named constructor call: Type.ctor_name(args)
    if (call_expr.callee->kind == AstKind::ExprGet) {
        GetExpr& get_expr = call_expr.callee->get;
        if (get_expr.object->kind == AstKind::ExprIdentifier) {
            StringView type_name = get_expr.object->identifier.name;
            Type* named_ctor_type = m_type_env.named_type_by_name(type_name);
            if (named_ctor_type && named_ctor_type->is_struct()) {
                get_expr.object->resolved_type = named_ctor_type;
                call_expr.constructor_name = get_expr.name;
                return analyze_constructor_call(expr, named_ctor_type, get_expr.name, call_expr.is_heap);
            }
        }
    }

    // Super call: super() / super.name()
    if (call_expr.callee->kind == AstKind::ExprSuper) {
        return analyze_super_call(expr, call_expr);
    }

    // Method call: obj.method(args)
    if (call_expr.callee->kind == AstKind::ExprGet) {
        GetExpr& get_expr = call_expr.callee->get;
        Type* obj_type = analyze_expr(get_expr.object);
        if (obj_type && !obj_type->is_error()) {
            Type* base_type = obj_type->base_type();

            if (base_type && base_type->is_list()) {
                const MethodInfo* mi = lookup_list_method(base_type->list_info, get_expr.name);
                if (mi) return analyze_builtin_method_call(expr, call_expr, get_expr, obj_type, mi);
                error_fmt(expr->loc, "List has no method '{}'", get_expr.name);
                return m_types.error_type();
            }
            if (base_type && base_type->is_map()) {
                const MethodInfo* mi = lookup_map_method(base_type->map_info, get_expr.name);
                if (mi) return analyze_builtin_method_call(expr, call_expr, get_expr, obj_type, mi);
                error_fmt(expr->loc, "Map has no method '{}'", get_expr.name);
                return m_types.error_type();
            }
            if (base_type && base_type->is_coroutine()) {
                const MethodInfo* mi = lookup_coro_method(base_type->coro_info, get_expr.name);
                if (mi) return analyze_builtin_method_call(expr, call_expr, get_expr, obj_type, mi);
                error_fmt(expr->loc, "Coro has no method '{}'", get_expr.name);
                return m_types.error_type();
            }
            if (base_type && base_type->is_type_param() && m_active_type_param_bounds.size() > 0) {
                Type* found_in_trait = nullptr;
                const TraitMethodInfo* trait_method = lookup_type_param_method(base_type, get_expr.name, &found_in_trait);
                if (trait_method) {
                    return analyze_type_param_method_call(expr, call_expr, get_expr, obj_type, base_type,
                                                          trait_method, found_in_trait);
                }
                error_fmt(expr->loc, "no method '{}' found in trait bounds for type parameter '{}'",
                         get_expr.name, base_type->type_param_info.name);
                return m_types.error_type();
            }
            if (base_type && base_type->is_struct()) {
                Type* result = analyze_struct_method_call(expr, call_expr, get_expr, obj_type, base_type);
                if (result) return result;
            }
        }
    }

    // Regular function call (fallback)
    return analyze_regular_fun_call(expr, call_expr);
}

Type* SemanticAnalyzer::analyze_primitive_cast(Expr* expr, Type* target_type) {
    CallExpr& call_expr = expr->call;

    // Must have exactly one argument
    if (call_expr.arguments.size() != 1) {
        error_fmt(expr->loc, "type cast requires exactly 1 argument, got {}", call_expr.arguments.size());
        return m_types.error_type();
    }

    // Check for modifiers (out/inout not allowed)
    if (call_expr.arguments[0].modifier != ParamModifier::None) {
        error(expr->loc, "type cast argument cannot have 'out' or 'inout' modifier");
        return m_types.error_type();
    }

    // Analyze the source expression
    Type* source_type = analyze_expr(call_expr.arguments[0].expr);
    if (!source_type || source_type->is_error()) {
        return m_types.error_type();
    }

    // Check if the cast is valid
    if (!m_checker.can_cast(source_type, target_type)) {
        auto source_str = m_checker.type_string(source_type);
        auto target_str = m_checker.type_string(target_type);
        error_fmt(expr->loc, "cannot cast '{}' to '{}'", source_str.data(), target_str.data());
        return m_types.error_type();
    }

    // Store target type in callee for IR builder to detect this is a cast
    call_expr.callee->resolved_type = target_type;

    return target_type;
}

Type* SemanticAnalyzer::analyze_constructor_call(Expr* expr, Type* struct_type, StringView ctor_name, bool is_heap) {
    CallExpr& call_expr = expr->call;
    StructTypeInfo& struct_type_info = struct_type->struct_info;

    // Look up constructor by name
    const ConstructorInfo* ctor = find_constructor(struct_type_info.constructors, ctor_name);

    // If a constructor name was specified but not found
    if (!ctor_name.empty() && !ctor) {
        error_fmt(expr->loc, "struct '{}' has no constructor '{}'", struct_type_info.name, ctor_name);
        return m_types.error_type();
    }

    // Determine result type based on is_heap flag
    // uniq Type() -> uniq<Type>
    // Type() -> Type (value type, stack-allocated)
    auto result_type = [&]() -> Type* {
        return is_heap ? m_types.uniq_type(struct_type) : struct_type;
    };

    // If we have a constructor, type-check the arguments
    if (ctor) {
        // Check argument count
        if (call_expr.arguments.size() != ctor->param_types.size()) {
            error_fmt(expr->loc, "constructor expects {} arguments but got {}",
                     ctor->param_types.size(), call_expr.arguments.size());
            return result_type();
        }

        // Check argument types and modifiers
        ConstructorDecl* ctor_decl = &ctor->decl->constructor_decl;
        check_call_args(call_expr.arguments, ctor->param_types, ctor_decl->params, expr->loc);
    } else {
        // No constructor defined - either using default construction or error
        if (!ctor_name.empty()) {
            // Named constructor was requested but struct has no constructors
            error_fmt(expr->loc, "struct '{}' has no constructor '{}'", struct_type_info.name, ctor_name);
            return m_types.error_type();
        }

        // Default construction (no constructor, no arguments) - allowed
        // Arguments without a constructor is an error
        if (call_expr.arguments.size() > 0) {
            error_fmt(expr->loc, "struct '{}' has no constructor to call", struct_type_info.name);
            return m_types.error_type();
        }
    }

    return result_type();
}

Type* SemanticAnalyzer::analyze_index_expr(Expr* expr) {
    IndexExpr& index_expr = expr->index;

    Type* obj_type = analyze_expr(index_expr.object);
    Type* idx_type = analyze_expr(index_expr.index);

    if (obj_type->is_error()) return m_types.error_type();

    // Unwrap reference types
    Type* base_type = obj_type->base_type();

    // Unified dispatch: look up "index" method on any type (list, struct, etc.)
    StringView method_name("index", 5);
    const MethodInfo* method_info = m_types.lookup_method(base_type, method_name);
    if (method_info && method_info->param_types.size() == 1) {
        if (!idx_type->is_error()) {
            m_checker.check_assignable(method_info->param_types[0], idx_type, index_expr.index->loc);
            m_checker.coerce_int_literal(index_expr.index, method_info->param_types[0]);
        }
        return method_info->return_type;
    }

    error(index_expr.object->loc, "type has no 'index' method for subscript operator");
    return m_types.error_type();
}

Type* SemanticAnalyzer::analyze_get_expr(Expr* expr) {
    GetExpr& get_expr = expr->get;

    if (!get_expr.object) return m_types.error_type();

    // Check for module-qualified access: module.member
    if (get_expr.object->kind == AstKind::ExprIdentifier) {
        StringView name = get_expr.object->identifier.name;
        Symbol* sym = m_symbols.lookup(name);

        if (sym && sym->kind == SymbolKind::Module) {
            // This is module-qualified access
            ModuleInfo* module = static_cast<ModuleInfo*>(sym->module.module_info);
            const ModuleExport* exp = module->find_export(get_expr.name);

            if (!exp) {
                error_fmt(expr->loc, "module '{}' has no export '{}'", name, get_expr.name);
                return m_types.error_type();
            }

            // Check visibility
            if (!exp->is_pub) {
                error_fmt(expr->loc, "'{}' is not public in module '{}'", get_expr.name, name);
                return m_types.error_type();
            }

            // Mark the expression with the module info for later use by IR builder
            // We set resolved_type on the object to indicate it's a module reference
            get_expr.object->resolved_type = nullptr;  // Modules don't have a type

            return exp->type;
        }
    }

    Type* obj_type = analyze_expr(get_expr.object);
    if (obj_type->is_error()) return m_types.error_type();

    // Unwrap reference types
    Type* base_type = obj_type->base_type();

    if (base_type->is_list()) {
        error(get_expr.object->loc, "cannot access fields of List type; use methods like .len(), .push(), .pop()");
        return m_types.error_type();
    }

    if (base_type->is_map()) {
        error(get_expr.object->loc, "cannot access fields of Map type; use methods like .get(), .insert(), .len()");
        return m_types.error_type();
    }

    if (base_type->is_type_param() && m_active_type_param_bounds.size() > 0) {
        // Type parameters have no fields, but may have methods via bounds
        Type* found_in_trait = nullptr;
        const TraitMethodInfo* trait_method = lookup_type_param_method(base_type, get_expr.name, &found_in_trait);
        if (trait_method) {
            // Methods on type params must be called, not accessed as values
            return m_types.error_type();
        }
        error_fmt(expr->loc, "type parameter '{}' has no field or method '{}'",
                 base_type->type_param_info.name, get_expr.name);
        return m_types.error_type();
    }

    if (!base_type->is_struct()) {
        error(get_expr.object->loc, "cannot access member of non-struct type");
        return m_types.error_type();
    }

    // Look up field
    StructTypeInfo& struct_type_info = base_type->struct_info;
    for (const auto& field : struct_type_info.fields) {
        if (field.name == get_expr.name) {
            // Check visibility: non-public fields can only be accessed from the same module
            // If either module name is empty, we're in single-file mode where all access is allowed
            bool same_module = struct_type_info.module_name.empty() || m_program->module_name.empty() ||
                               struct_type_info.module_name == m_program->module_name;
            if (!field.is_pub && !same_module) {
                error_fmt(expr->loc, "field '{}' is private in struct '{}'", get_expr.name, struct_type_info.name);
                return m_types.error_type();
            }
            return field.type;
        }
    }

    // Look up variant field in when clauses (tagged union)
    const WhenClauseInfo* found_clause = nullptr;
    const VariantInfo* found_variant = nullptr;
    const VariantFieldInfo* variant_field_info = struct_type_info.find_variant_field(get_expr.name, &found_clause, &found_variant);
    if (variant_field_info) {
        // Variant field access - semantic analysis allows it, runtime will check discriminant
        bool same_module = struct_type_info.module_name.empty() || m_program->module_name.empty() ||
                           struct_type_info.module_name == m_program->module_name;
        if (!variant_field_info->is_pub && !same_module) {
            error_fmt(expr->loc, "variant field '{}' is private in struct '{}'",
                      get_expr.name, struct_type_info.name);
            return m_types.error_type();
        }
        return variant_field_info->type;
    }

    // Look up method (walks inheritance hierarchy)
    Type* found_in_type = nullptr;
    const MethodInfo* mi = lookup_method_in_hierarchy(base_type, get_expr.name, &found_in_type);
    if (mi) {
        return build_method_function_type(found_in_type, mi);
    }

    error_fmt(expr->loc, "struct '{}' has no member '{}'", struct_type_info.name, get_expr.name);
    return m_types.error_type();
}

Type* SemanticAnalyzer::analyze_static_get_expr(Expr* expr) {
    StaticGetExpr& sge = expr->static_get;

    // Look up the type
    Type* type = m_type_env.named_type_by_name(sge.type_name);
    if (!type) {
        error_fmt(expr->loc, "unknown type '{}'", sge.type_name);
        return m_types.error_type();
    }

    if (type->is_enum()) {
        // Look up enum variant
        Symbol* sym = m_symbols.lookup(sge.member_name);
        if (sym && sym->kind == SymbolKind::EnumVariant && sym->type == type) {
            return type;
        }
        error_fmt(expr->loc, "enum '{}' has no variant '{}'", sge.type_name, sge.member_name);
        return m_types.error_type();
    }

    error(expr->loc, "static member access is only supported for enums");
    return m_types.error_type();
}

Type* SemanticAnalyzer::analyze_assign_expr(Expr* expr) {
    AssignExpr& assign_expr = expr->assign;

    // Check lvalue
    if (!is_lvalue(assign_expr.target)) {
        error(assign_expr.target->loc, "cannot assign to non-lvalue expression");
        return m_types.error_type();
    }

    // For plain assignment to a uniq identifier, temporarily mark it Live
    // so the move check in analyze_expr doesn't fire (we're reassigning, not using)
    bool restored_for_assign = false;
    MoveState saved_state = MoveState::Live;
    if (assign_expr.op == AssignOp::Assign &&
        assign_expr.target->kind == AstKind::ExprIdentifier) {
        Symbol* target_sym = m_symbols.lookup(assign_expr.target->identifier.name);
        if (target_sym) {
            auto it = m_move_states.find(target_sym);
            if (it != m_move_states.end() && it->second != MoveState::Live) {
                saved_state = it->second;
                it.value() = MoveState::Live;
                restored_for_assign = true;
            }
        }
    }

    Type* target_type = analyze_expr(assign_expr.target);

    // Restore the state so auto-delete logic in IR builder knows the old value was moved
    if (restored_for_assign) {
        Symbol* target_sym = m_symbols.lookup(assign_expr.target->identifier.name);
        if (target_sym) {
            auto it = m_move_states.find(target_sym);
            if (it != m_move_states.end()) {
                it.value() = saved_state;
            }
        }
    }

    Type* value_type = analyze_expr(assign_expr.value);

    if (target_type->is_error() || value_type->is_error()) {
        return m_types.error_type();
    }

    // For compound assignment operators, check operand compatibility
    if (assign_expr.op != AssignOp::Assign) {
        // Coerce int literals to the target type before trait lookup
        if (value_type->is_int_literal() && target_type->is_integer()) {
            m_checker.coerce_int_literal(assign_expr.value, target_type);
            value_type = target_type;
        }
        // Check for compound assignment trait method (works for both structs and primitives)
        const char* method_name = assign_op_to_trait_method(assign_expr.op);
        if (method_name) {
            StringView name(method_name, static_cast<u32>(strlen(method_name)));
            const MethodInfo* mi = m_types.lookup_method(target_type, name);
            if (mi && mi->param_types.size() == 1 && mi->param_types[0] == value_type) {
                return target_type;
            }
        }
        // Fall back to binary op validation
        BinaryOp binop;
        switch (assign_expr.op) {
            case AssignOp::AddAssign: binop = BinaryOp::Add; break;
            case AssignOp::SubAssign: binop = BinaryOp::Sub; break;
            case AssignOp::MulAssign: binop = BinaryOp::Mul; break;
            case AssignOp::DivAssign: binop = BinaryOp::Div; break;
            case AssignOp::ModAssign: binop = BinaryOp::Mod; break;
            case AssignOp::BitAndAssign: binop = BinaryOp::BitAnd; break;
            case AssignOp::BitOrAssign:  binop = BinaryOp::BitOr; break;
            case AssignOp::BitXorAssign: binop = BinaryOp::BitXor; break;
            case AssignOp::ShlAssign:    binop = BinaryOp::Shl; break;
            case AssignOp::ShrAssign:    binop = BinaryOp::Shr; break;
            default: binop = BinaryOp::Add; break;
        }
        get_binary_result_type(binop, target_type, value_type, expr->loc);
    } else {
        m_checker.check_assignable(target_type, value_type, assign_expr.value->loc);
        m_checker.coerce_int_literal(assign_expr.value, target_type);
    }

    // Reject self-assignment of noncopyables (e.g. `x = x` on a uniq variable):
    // the target slot auto-deletes first and then the source "move" copies a
    // dangling pointer back in — a guaranteed use-after-free.
    if (assign_expr.op == AssignOp::Assign &&
        target_type && target_type->noncopyable() &&
        assign_expr.target->kind == AstKind::ExprIdentifier &&
        assign_expr.value->kind == AstKind::ExprIdentifier) {
        Symbol* tgt_sym = m_symbols.lookup(assign_expr.target->identifier.name);
        Symbol* src_sym = m_symbols.lookup(assign_expr.value->identifier.name);
        if (tgt_sym && tgt_sym == src_sym) {
            error_fmt(expr->loc,
                "self-assignment of noncopyable variable '{}' would cause use-after-free",
                assign_expr.target->identifier.name);
            return m_types.error_type();
        }
    }

    // Reassignment to owned variable: mark it live again (auto-destroy of old value happens in IR)
    if (assign_expr.target->kind == AstKind::ExprIdentifier &&
        target_type && target_type->noncopyable()) {
        mark_live(assign_expr.target->identifier.name);
    }

    // Consume noncopyable source (field-move check + mark source as moved)
    if (assign_expr.op == AssignOp::Assign && target_type && target_type->noncopyable()) {
        consume_noncopyable(assign_expr.value, assign_expr.value->loc);
    }

    // Validate index_mut exists for index assignment targets
    if (assign_expr.target->kind == AstKind::ExprIndex) {
        IndexExpr& index_target = assign_expr.target->index;
        Type* container_type = index_target.object->resolved_type;
        if (container_type) container_type = container_type->base_type();
        if (container_type) {
            StringView method_name("index_mut", 9);
            const MethodInfo* method_info = m_types.lookup_method(container_type, method_name);
            if (!method_info) {
                error(assign_expr.target->loc, "type has no 'index_mut' method for index assignment");
                return m_types.error_type();
            }
        }
    }

    return target_type;
}

Type* SemanticAnalyzer::analyze_grouping_expr(Expr* expr) {
    return analyze_expr(expr->grouping.expr);
}

Type* SemanticAnalyzer::analyze_this_expr(Expr* expr) {
    if (!m_symbols.is_in_struct()) {
        error(expr->loc, "'self' used outside of struct context");
        return m_types.error_type();
    }

    Type* struct_type = m_symbols.current_struct_type();

    // Closure capture: if we're inside a lambda body whose scope sits past a
    // ScopeKind::Lambda boundary relative to the enclosing struct scope, this
    // `self` reference must be captured into the lambda's env. Walk from the
    // current scope up — if we cross a Lambda boundary before reaching the
    // struct scope, this is a closure capture.
    if (!m_lambda_contexts.empty()) {
        bool crossed_lambda = false;
        for (Scope* sc = m_symbols.current_scope(); sc; sc = sc->parent) {
            if (sc->kind == ScopeKind::Struct) break;
            if (sc->kind == ScopeKind::Lambda) { crossed_lambda = true; break; }
        }

        if (crossed_lambda) {
            // Ensure every enclosing lambda context has self captured (implicit
            // ref-self) so the chain works for nested closures. If the
            // innermost already has an explicit [copy self] / [weak self], its
            // existing entry's type drives the rewrite — but the chain still
            // needs the OUTER contexts' implicit refs to be populated.
            u32 last_idx = static_cast<u32>(m_lambda_contexts.size()) - 1;
            LambdaCaptureContext& innermost = *m_lambda_contexts[last_idx];
            if (last_idx > 0) {
                ensure_self_captured_through(last_idx - 1, struct_type, expr->loc);
            }
            if (!innermost.has_self_capture) {
                ensure_self_captured_through(last_idx, struct_type, expr->loc);
            }

            CaptureInfo& info = innermost.captures[innermost.self_capture_index];

            // Rewrite the ExprThis in-place to `__env.__self`. The env field
            // type drives the resulting expr's resolved_type (ref Self for
            // implicit / `[ref]`-equivalent, value Self for [copy], weak Self
            // for [weak]).
            Type* env_ref = innermost.env_struct_type
                ? m_types.ref_type(innermost.env_struct_type)
                : nullptr;
            Expr* env_id = make_identifier_expr(StringView("__env", 5), env_ref, expr->loc);

            expr->kind = AstKind::ExprGet;
            expr->get.object = env_id;
            expr->get.name = StringView("__self", 6);
            return info.type;
        }
    }

    // 'self' is a ref to the current struct
    return m_types.ref_type(struct_type);
}

Type* SemanticAnalyzer::analyze_super_expr(Expr* expr) {
    SuperExpr& super_expr = expr->super_expr;

    if (!m_symbols.is_in_struct()) {
        error(expr->loc, "'super' used outside of struct context");
        return m_types.error_type();
    }

    Type* struct_type = m_symbols.current_struct_type();
    Type* parent_type = struct_type->struct_info.parent;

    if (!parent_type) {
        error(expr->loc, "'super' used in struct with no parent");
        return m_types.error_type();
    }

    // Look up method in parent (and its ancestors), NOT in child
    Type* found_in_type = nullptr;
    const MethodInfo* mi = lookup_method_in_hierarchy(parent_type, super_expr.method_name, &found_in_type);

    if (!mi) {
        error_fmt(expr->loc, "parent struct has no method '{}'", super_expr.method_name);
        return m_types.error_type();
    }

    return build_method_function_type(found_in_type, mi);
}

Type* SemanticAnalyzer::analyze_struct_literal_expr(Expr* expr) {
    StructLiteralExpr& sl = expr->struct_literal;

    Type* type = resolve_struct_literal_type(expr, sl);
    if (!type || type->is_error()) return m_types.error_type();

    check_struct_literal_fields(expr, sl, type);

    // Return uniq<type> for heap allocation, value type for stack
    return sl.is_heap ? m_types.uniq_type(type) : type;
}

Type* SemanticAnalyzer::resolve_struct_literal_type(Expr* expr, StructLiteralExpr& sl) {
    Type* type = nullptr;

    // Check for generic struct literal: Box<i32> { value = 42 }
    if (sl.type_args.size() > 0 && m_type_env.generics().is_generic_struct(sl.type_name)) {
        // Resolve type args
        Vector<Type*> type_arg_types;
        for (auto* type_arg : sl.type_args) {
            Type* arg_type = resolve_type_expr(type_arg);
            if (!arg_type || arg_type->is_error()) return m_types.error_type();
            type_arg_types.push_back(arg_type);
        }

        Span<Type*> type_args = m_allocator.alloc_span(type_arg_types);

        // Check trait bounds on type args
        const ResolvedTypeParams* bounds = m_type_env.generics().get_struct_bounds(sl.type_name);
        if (!check_type_arg_bounds(sl.type_name, type_args, bounds, expr->loc)) {
            return m_types.error_type();
        }

        StringView mangled = m_type_env.generics().instantiate_struct(sl.type_name, type_args);
        GenericStructInstance* inst = m_type_env.generics().find_struct_instance(mangled);
        resolve_generic_struct_fields(inst);
        type = inst->concrete_type;
        sl.mangled_name = mangled;
        sl.type_name = mangled;  // Update to mangled name for IR builder
    }

    // Generic struct literal WITHOUT type args — attempt inference
    if (!type && sl.type_args.size() == 0 && m_type_env.generics().is_generic_struct(sl.type_name)) {
        Decl* template_decl = m_type_env.generics().get_generic_struct_decl(sl.type_name);
        StructDecl& template_struct_decl = template_decl->struct_decl;

        auto inferred = infer_type_args_from_fields(
            template_struct_decl.type_params, template_struct_decl.fields,
            sl.fields, expr->loc);

        if (inferred.success) {
            Span<Type*> type_args = m_allocator.alloc_span(inferred.type_args);

            // Check trait bounds on inferred type args
            const ResolvedTypeParams* bounds = m_type_env.generics().get_struct_bounds(sl.type_name);
            if (!check_type_arg_bounds(sl.type_name, type_args, bounds, expr->loc)) {
                return m_types.error_type();
            }

            StringView mangled = m_type_env.generics().instantiate_struct(sl.type_name, type_args);
            GenericStructInstance* inst = m_type_env.generics().find_struct_instance(mangled);
            resolve_generic_struct_fields(inst);
            type = inst->concrete_type;
            sl.mangled_name = mangled;
            sl.type_name = mangled;
        } else {
            error_fmt(expr->loc,
                "cannot infer type arguments for generic struct '{}'; "
                "provide explicit type arguments", sl.type_name);
            return m_types.error_type();
        }
    }

    if (!type) {
        // Look up struct type
        type = m_type_env.named_type_by_name(sl.type_name);
        if (!type) {
            error_fmt(expr->loc, "unknown struct type '{}'", sl.type_name);
            return m_types.error_type();
        }
    }
    if (!type->is_struct()) {
        error_fmt(expr->loc, "'{}' is not a struct type", sl.type_name);
        return m_types.error_type();
    }
    return type;
}

void SemanticAnalyzer::check_struct_literal_fields(Expr* expr, StructLiteralExpr& sl, Type* type) {
    // Helper to find field default value by searching inheritance chain
    auto find_field_default = [](Type* struct_type, StringView field_name) -> Expr* {
        Type* current = struct_type;
        while (current && current->is_struct()) {
            if (!current->struct_info.decl) {
                current = current->struct_info.parent;
                continue;
            }
            StructDecl& struct_decl = current->struct_info.decl->struct_decl;
            for (const auto& field : struct_decl.fields) {
                if (field.name == field_name) {
                    return field.default_value;
                }
            }
            current = current->struct_info.parent;
        }
        return nullptr;
    };

    // Track which fields have been initialized
    Vector<bool> field_initialized(type->struct_info.fields.size(), false);

    // Track initialized variant fields by name
    tsl::robin_map<StringView, bool> variant_field_initialized;
    i64 discriminant_value = -1;  // Track which variant is selected

    // Validate each field initializer
    for (auto& fi : sl.fields) {

        // Find field in struct
        i32 field_idx = -1;
        for (u32 j = 0; j < type->struct_info.fields.size(); j++) {
            if (type->struct_info.fields[j].name == fi.name) {
                field_idx = static_cast<i32>(j);
                break;
            }
        }

        if (field_idx >= 0) {
            // Regular field
            if (field_initialized[field_idx]) {
                error_fmt(fi.loc, "duplicate field '{}'", fi.name);
                continue;
            }

            field_initialized[field_idx] = true;

            // Type-check field value
            Type* value_type = analyze_expr(fi.value);
            Type* field_type = type->struct_info.fields[field_idx].type;
            if (field_type && coerce_generic_template_ref(fi.value, field_type)) {
                value_type = fi.value->resolved_type;
            }
            m_checker.check_assignable(field_type, value_type, fi.loc);
            m_checker.coerce_int_literal(fi.value, field_type);

            // Consume noncopyable source (field-move check + mark source as moved)
            if (field_type && field_type->noncopyable()) {
                consume_noncopyable(fi.value, fi.loc);
            }

            // Track discriminant value if this is a discriminant field
            for (const auto& clause : type->struct_info.when_clauses) {
                if (clause.discriminant_name == fi.name) {
                    // Check if value is a static get (e.g., Attack from enum)
                    if (fi.value->kind == AstKind::ExprIdentifier) {
                        Symbol* sym = m_symbols.lookup(fi.value->identifier.name);
                        if (sym && sym->kind == SymbolKind::EnumVariant) {
                            discriminant_value = sym->enum_variant.value;
                        }
                    }
                }
            }
            continue;
        }

        // Check if it's a variant field
        const WhenClauseInfo* found_clause = nullptr;
        const VariantInfo* found_variant = nullptr;
        const VariantFieldInfo* variant_field_info = type->struct_info.find_variant_field(fi.name, &found_clause, &found_variant);

        if (variant_field_info) {
            // Variant field
            if (variant_field_initialized.find(fi.name) != variant_field_initialized.end()) {
                error_fmt(fi.loc, "duplicate field '{}'", fi.name);
                continue;
            }
            variant_field_initialized[fi.name] = true;

            bool same_module = type->struct_info.module_name.empty() || m_program->module_name.empty() ||
                               type->struct_info.module_name == m_program->module_name;
            if (!variant_field_info->is_pub && !same_module) {
                error_fmt(fi.loc, "variant field '{}' is private in struct '{}'",
                          fi.name, type->struct_info.name);
                continue;
            }

            // Type-check field value
            Type* value_type = analyze_expr(fi.value);
            m_checker.check_assignable(variant_field_info->type, value_type, fi.loc);
            m_checker.coerce_int_literal(fi.value, variant_field_info->type);

            // Consume noncopyable source (field-move check + mark source as moved)
            if (variant_field_info->type && variant_field_info->type->noncopyable()) {
                consume_noncopyable(fi.value, fi.loc);
            }
            continue;
        }

        error_fmt(fi.loc, "struct '{}' has no field '{}'", sl.type_name, fi.name);
    }

    // Check all fields without defaults are initialized
    for (u32 i = 0; i < field_initialized.size(); i++) {
        if (!field_initialized[i]) {
            FieldInfo& field_info = type->struct_info.fields[i];
            Expr* default_val = find_field_default(type, field_info.name);
            if (default_val == nullptr) {
                error_fmt(expr->loc, "missing field '{}' (no default value)", field_info.name);
            }
        }
    }
}

// ===== Type Checking Helpers =====

bool SemanticAnalyzer::coerce_generic_template_ref(Expr* expr, Type* expected) {
    if (!expr || expr->kind != AstKind::ExprIdentifier) return true;
    IdentifierExpr& id = expr->identifier;
    if (!id.is_generic_template_ref) return true;
    if (!expected || !expected->is_function()) {
        error_fmt(expr->loc,
            "cannot use generic function '{}' as a value here; it needs a "
            "concrete function-type context (e.g. a typed variable or a "
            "typed function parameter) to bind the type parameters", id.name);
        return false;
    }

    Decl* template_decl = m_type_env.generics().get_generic_fun_decl(id.name);
    if (!template_decl || template_decl->kind != AstKind::DeclFun) {
        error_fmt(expr->loc, "internal error: missing template decl for '{}'", id.name);
        return false;
    }
    FunDecl& template_fun_decl = template_decl->fun_decl;
    FunctionTypeInfo& expected_fti = expected->func_info;

    // Param-count mismatch ⇒ no plausible binding.
    if (template_fun_decl.params.size() != expected_fti.param_types.size()) {
        error_fmt(expr->loc,
            "generic function '{}' has {} parameters but expected function "
            "type takes {}", id.name,
            template_fun_decl.params.size(), expected_fti.param_types.size());
        return false;
    }

    // Bind type params by unifying each template param TypeExpr against the
    // corresponding concrete param type, then the return TypeExpr against the
    // concrete return type. unify_type_expr already handles Function-kind
    // patterns (semantic.cpp:4719) so nested fun(T)->T params bind too.
    Vector<Type*> bindings;
    bindings.resize(template_fun_decl.type_params.size());
    for (u32 i = 0; i < bindings.size(); i++) bindings[i] = nullptr;
    for (u32 i = 0; i < template_fun_decl.params.size(); i++) {
        if (!unify_type_expr(template_fun_decl.params[i].type,
                             expected_fti.param_types[i],
                             template_fun_decl.type_params, bindings)) {
            error_fmt(expr->loc,
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
            error_fmt(expr->loc,
                "cannot bind type parameters of '{}' against expected return type",
                id.name);
            return false;
        }
    }
    for (u32 i = 0; i < bindings.size(); i++) {
        if (!bindings[i]) {
            error_fmt(expr->loc,
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

Type* SemanticAnalyzer::resolve_explicit_generic_template_ref(Expr* expr) {
    IdentifierExpr& id = expr->identifier;
    Decl* template_decl = m_type_env.generics().get_generic_fun_decl(id.name);
    if (!template_decl || template_decl->kind != AstKind::DeclFun) {
        error_fmt(expr->loc, "internal error: missing template decl for '{}'", id.name);
        return m_types.error_type();
    }
    FunDecl& template_fun_decl = template_decl->fun_decl;
    if (id.generic_args.size() != template_fun_decl.type_params.size()) {
        error_fmt(expr->loc,
            "generic function '{}' expects {} type arguments but got {}",
            id.name, template_fun_decl.type_params.size(), id.generic_args.size());
        return m_types.error_type();
    }

    Vector<Type*> type_arg_types;
    type_arg_types.reserve(id.generic_args.size());
    for (auto* arg_expr : id.generic_args) {
        Type* arg_type = resolve_type_expr(arg_expr);
        if (!arg_type || arg_type->is_error()) return m_types.error_type();
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
        Type* pt = resolve_type_expr(p.type);
        param_types.push_back(pt ? pt : m_types.error_type());
    }
    Type* ret_type = inst_decl.return_type
        ? resolve_type_expr(inst_decl.return_type) : m_types.void_type();
    Type* fn_type = m_types.function_type(
        m_allocator.alloc_span(param_types), ret_type);
    expr->resolved_type = fn_type;
    return fn_type;
}

Type* SemanticAnalyzer::get_binary_result_type(BinaryOp op, Type* left, Type* right, SourceLocation loc) {
    switch (op) {
        case BinaryOp::Add:
        case BinaryOp::Sub:
        case BinaryOp::Mul:
        case BinaryOp::Div:
        case BinaryOp::Mod: {
            // String concatenation (Add only)
            if (op == BinaryOp::Add && left->kind == TypeKind::String && right->kind == TypeKind::String)
                return m_types.string_type();
            // Unified dispatch: primitives and structs
            if (Type* result = try_resolve_binary_op(op, left, right))
                return result;
            // Type mismatch check for better error messages
            if (left->is_numeric() && right->is_numeric()) {
                m_checker.require_types_match(left, right, loc, "arithmetic operator");
                return m_types.error_type();
            }
            error(loc, "invalid operands for arithmetic operator");
            return m_types.error_type();
        }

        case BinaryOp::Equal:
        case BinaryOp::NotEqual:
        case BinaryOp::Less:
        case BinaryOp::LessEq:
        case BinaryOp::Greater:
        case BinaryOp::GreaterEq: {
            // Reference/nil comparison: uniq/ref/weak == nil, nil == uniq/ref/weak
            if ((op == BinaryOp::Equal || op == BinaryOp::NotEqual) &&
                ((left->is_reference() && right->is_nil()) ||
                 (left->is_nil() && right->is_reference()))) {
                return m_types.bool_type();
            }
            if (Type* result = try_resolve_binary_op(op, left, right))
                return result;
            // Type mismatch check for better error messages
            if (left->is_numeric() && right->is_numeric()) {
                m_checker.require_types_match(left, right, loc, "comparison operator");
                return m_types.error_type();
            }
            error(loc, "invalid operands for comparison operator");
            return m_types.error_type();
        }

        case BinaryOp::And:
        case BinaryOp::Or:
            if (left->is_bool() && right->is_bool())
                return m_types.bool_type();
            error(loc, "logical operators require boolean operands");
            return m_types.error_type();

        case BinaryOp::BitAnd:
        case BinaryOp::BitOr:
        case BinaryOp::BitXor:
        case BinaryOp::Shl:
        case BinaryOp::Shr: {
            if (Type* result = try_resolve_binary_op(op, left, right))
                return result;
            if (left->is_integer() && right->is_integer()) {
                m_checker.require_types_match(left, right, loc, "bitwise operator");
                return m_types.error_type();
            }
            error(loc, "bitwise operators require integer operands");
            return m_types.error_type();
        }
    }

    return m_types.error_type();
}

Type* SemanticAnalyzer::get_unary_result_type(UnaryOp op, Type* operand, SourceLocation loc) {
    switch (op) {
        case UnaryOp::Negate:
            if (operand->is_int_literal())
                return m_types.int_literal_type();
            if (Type* result = try_resolve_unary_op(op, operand))
                return result;
            error(loc, "unary '-' requires numeric operand");
            return m_types.error_type();

        case UnaryOp::Not:
            if (operand->is_bool())
                return m_types.bool_type();
            error(loc, "unary '!' requires boolean operand");
            return m_types.error_type();

        case UnaryOp::BitNot:
            if (operand->is_int_literal())
                return m_types.int_literal_type();
            if (Type* result = try_resolve_unary_op(op, operand))
                return result;
            error(loc, "unary '~' requires integer operand");
            return m_types.error_type();

        case UnaryOp::Ref:
            // Handled in analyze_unary_expr before this function is called
            return m_types.error_type();
    }

    return m_types.error_type();
}

bool SemanticAnalyzer::is_lvalue(Expr* expr) const {
    if (!expr) return false;

    switch (expr->kind) {
        case AstKind::ExprIdentifier:
            return true;
        case AstKind::ExprIndex:
            return true;
        case AstKind::ExprGet:
            return true;
        case AstKind::ExprGrouping:
            return is_lvalue(expr->grouping.expr);
        default:
            return false;
    }
}

}
