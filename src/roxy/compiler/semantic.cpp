#include "roxy/compiler/semantic.hpp"
#include "roxy/compiler/operator_traits.hpp"
#include "roxy/compiler/module_registry.hpp"
#include "roxy/vm/natives.hpp"
#include "roxy/vm/binding/registry.hpp"

#include <cstring>

namespace rx {

static const ConstructorInfo* find_constructor(Span<ConstructorInfo> constructors, StringView name) {
    for (const auto& constructor : constructors) {
        if (constructor.name == name) {
            return &constructor;
        }
    }
    return nullptr;
}

// Returns number of u32 slots needed for a type
// 1 slot = 4 bytes (for bool, i8-i32, u8-u32, f32)
// 2 slots = 8 bytes (for i64, u64, f64, pointers)
static u32 get_type_slot_count(Type* type) {
    if (!type) return 0;
    
    switch (type->kind) {
        // 1 slot (4 bytes) - small types widened to 32-bit
        case TypeKind::Bool:
        case TypeKind::I8:  case TypeKind::U8:
        case TypeKind::I16: case TypeKind::U16:
        case TypeKind::I32: case TypeKind::U32:
        case TypeKind::F32:
        case TypeKind::Enum:  // Enums are stored as i32
        case TypeKind::IntLiteral:  // Safety net: defaults to i32 (1 slot)
            return 1;
        
        // 2 slots (8 bytes)
        case TypeKind::I64: case TypeKind::U64:
        case TypeKind::F64:
        case TypeKind::Uniq:
        case TypeKind::Ref:
        case TypeKind::Weak:
            return 2;
        
        // Structs: use computed slot_count
        case TypeKind::Struct:
            return type->struct_info.slot_count;
        
        // Lists and Maps are pointers (2 slots)
        case TypeKind::List:
        case TypeKind::Map:
            return 2;

        default:
            return 0;
    }
}

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
    , m_program(nullptr)
{
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
                exp.name, exp.type, SourceLocation{0, 0},
                BUILTIN_MODULE_NAME,
                exp.name, exp.index, exp.is_native);
        } else {
            // For structs/enums, define as regular types
            m_symbols.define(static_cast<SymbolKind>(
                exp.kind == ExportKind::Struct ? SymbolKind::Struct : SymbolKind::Enum),
                exp.name, exp.type, SourceLocation{0, 0}, exp.decl);
        }
    }
}

bool SemanticAnalyzer::analyze(Program* program) {
    m_program = program;

    // Pass 0a: Auto-import builtin module as prelude
    import_builtin_prelude();
    if (too_many_errors()) return false;

    // Pass 0b: Process user imports
    for (auto* decl : program->declarations) {
        if (decl && decl->kind == AstKind::DeclImport) {
            analyze_import_decl(decl);
        }
    }
    if (too_many_errors()) return false;

    // Pass 0c: Apply native function symbols from registry (non-method entries)
    if (m_registry) {
        m_registry->apply_to_symbols(m_symbols, m_types, m_allocator);
    }

    // Pass 1: Collect type declarations (struct/enum names)
    collect_type_declarations(program);
    if (too_many_errors()) return false;

    // Pass 1.5: Create native struct types from registry
    if (m_registry) {
        m_registry->apply_structs_to_types(m_type_env, m_allocator, m_symbols);
    }

    // Pass 1.6: Apply native methods to struct types
    if (m_registry) {
        m_registry->apply_methods_to_types(m_type_env, m_allocator);
    }

    // Pass 1.7: Register builtin Printable trait and primitive implementations
    // Guard against re-initialization since TypeEnv persists across modules
    if (!m_type_env.printable_type()) {
        // Create the Printable trait type
        Type* printable_type = m_types.trait_type(StringView("Printable", 9), nullptr);
        m_type_env.set_printable_type(printable_type);
        m_type_env.register_trait_type(StringView("Printable", 9), printable_type);

        // Add to_string() as the required trait method
        TraitMethodInfo trait_method_info;
        trait_method_info.name = StringView("to_string", 9);
        trait_method_info.param_types = Span<Type*>(nullptr, 0);  // no params besides self
        trait_method_info.return_type = m_types.string_type();
        trait_method_info.decl = nullptr;
        trait_method_info.has_default = false;

        TraitMethodInfo* tmi_data = reinterpret_cast<TraitMethodInfo*>(
            m_allocator.alloc_bytes(sizeof(TraitMethodInfo), alignof(TraitMethodInfo)));
        tmi_data[0] = trait_method_info;
        printable_type->trait_info.methods = Span<TraitMethodInfo>(tmi_data, 1);

        // Register to_string method and Printable trait for each primitive type
        TypeKind prim_kinds[] = {
            TypeKind::Bool, TypeKind::I32, TypeKind::I64,
            TypeKind::F32, TypeKind::F64, TypeKind::String
        };
        for (TypeKind tk : prim_kinds) {
            MethodInfo method_info;
            method_info.name = StringView("to_string", 9);
            method_info.param_types = Span<Type*>(nullptr, 0);
            method_info.return_type = m_types.string_type();
            method_info.decl = nullptr;
            m_types.register_primitive_method(tk, method_info);
            m_types.register_primitive_trait(tk, printable_type);
         }
    }

    // Pass 1.7b: Register builtin Hash trait and primitive implementations
    if (!m_type_env.hash_type()) {
        Type* hash_trait_type = m_types.trait_type(StringView("Hash", 4), nullptr);
        m_type_env.set_hash_type(hash_trait_type);
        m_type_env.register_trait_type(StringView("Hash", 4), hash_trait_type);

        // Add hash() as the required trait method
        TraitMethodInfo trait_method_info;
        trait_method_info.name = StringView("hash", 4);
        trait_method_info.param_types = Span<Type*>(nullptr, 0);
        trait_method_info.return_type = m_types.i64_type();
        trait_method_info.decl = nullptr;
        trait_method_info.has_default = false;

        TraitMethodInfo* tmi_data = reinterpret_cast<TraitMethodInfo*>(
            m_allocator.alloc_bytes(sizeof(TraitMethodInfo), alignof(TraitMethodInfo)));
        tmi_data[0] = trait_method_info;
        hash_trait_type->trait_info.methods = Span<TraitMethodInfo>(tmi_data, 1);

        // Register hash() method and Hash trait for hashable primitive types
        TypeKind hashable_kinds[] = {
            TypeKind::Bool,
            TypeKind::I8, TypeKind::I16, TypeKind::I32, TypeKind::I64,
            TypeKind::U8, TypeKind::U16, TypeKind::U32, TypeKind::U64,
            TypeKind::F32, TypeKind::F64,
            TypeKind::String
        };
        for (TypeKind tk : hashable_kinds) {
            MethodInfo method_info;
            method_info.name = StringView("hash", 4);
            method_info.param_types = Span<Type*>(nullptr, 0);
            method_info.return_type = m_types.i64_type();
            method_info.decl = nullptr;
            m_types.register_primitive_method(tk, method_info);
            m_types.register_primitive_trait(tk, hash_trait_type);
        }
    }

    // Pass 1.7c: Register builtin Exception trait
    if (!m_type_env.exception_type()) {
        Type* exception_trait_type = m_types.trait_type(StringView("Exception", 9), nullptr);
        m_type_env.set_exception_type(exception_trait_type);
        m_type_env.register_trait_type(StringView("Exception", 9), exception_trait_type);

        // Add message() as the required trait method
        TraitMethodInfo trait_method_info;
        trait_method_info.name = StringView("message", 7);
        trait_method_info.param_types = Span<Type*>(nullptr, 0);  // no params besides self
        trait_method_info.return_type = m_types.string_type();
        trait_method_info.decl = nullptr;
        trait_method_info.has_default = false;

        TraitMethodInfo* tmi_data = reinterpret_cast<TraitMethodInfo*>(
            m_allocator.alloc_bytes(sizeof(TraitMethodInfo), alignof(TraitMethodInfo)));
        tmi_data[0] = trait_method_info;
        exception_trait_type->trait_info.methods = Span<TraitMethodInfo>(tmi_data, 1);

        // Register message() method on ExceptionRef built-in type
        MethodInfo method_info;
        method_info.name = StringView("message", 7);
        method_info.param_types = Span<Type*>(nullptr, 0);
        method_info.return_type = m_types.string_type();
        method_info.decl = nullptr;
        m_types.register_primitive_method(TypeKind::ExceptionRef, method_info);
    }

    // Pass 1.8: Register built-in operator trait methods for primitive types
    register_primitive_operator_methods();

    // Pass 1.9: Resolve trait bounds on generic type parameters
    resolve_generic_bounds();
    if (too_many_errors()) return false;

    resolve_type_members(program);
    if (too_many_errors()) return false;

    // Pass 3: Analyze function bodies (full type checking)
    analyze_function_bodies(program);

    return !has_errors();
}

// Error reporting

void SemanticAnalyzer::error(SourceLocation loc, const char* message) {
    if (too_many_errors()) return;
    m_errors.push_back({loc, message, false});
}

// Pass 1: Collect type declarations

void SemanticAnalyzer::collect_type_declarations(Program* program) {
    for (auto* decl : program->declarations) {
        if (!decl) continue;

        // Register generic functions as templates (not concrete functions)
        if (decl->kind == AstKind::DeclFun && decl->fun_decl.type_params.size() > 0) {
            m_type_env.generics().register_generic_fun(decl->fun_decl.name, decl);
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

            // Check for duplicate type/trait names
            if (m_type_env.named_type_by_name(name) != nullptr ||
                m_type_env.trait_type_by_name(name) != nullptr) {
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

        if (decl->kind == AstKind::DeclStruct) {
            StructDecl& struct_decl = decl->struct_decl;

            // Skip generic struct templates - they have unresolved type params
            if (struct_decl.type_params.size() > 0) continue;

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
        else if (decl->kind == AstKind::DeclEnum) {
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
        else if (decl->kind == AstKind::DeclFun) {
            // Skip generic function templates - they have unresolved type params
            FunDecl& fun_decl = decl->fun_decl;
            if (fun_decl.type_params.size() > 0) continue;

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

            // Create function type
            Type* func_type = m_types.function_type(
                m_allocator.alloc_span(param_types), return_type);

            // Define function in global scope
            Symbol* sym = m_symbols.define(SymbolKind::Function, fun_decl.name, func_type, decl->loc, decl);
            sym->is_pub = fun_decl.is_pub;
        }
        else if (decl->kind == AstKind::DeclVar) {
            // Global variable
            VarDecl& var_decl = decl->var_decl;

            Type* var_type = nullptr;
            if (var_decl.type) {
                var_type = resolve_type_expr(var_decl.type);
            }

            if (var_decl.initializer) {
                Type* init_type = analyze_expr(var_decl.initializer);
                if (!var_type) {
                    // Type inference
                    var_type = init_type;
                    if (var_type->is_int_literal()) {
                        var_type = m_types.i32_type();
                        coerce_int_literal(var_decl.initializer, var_type);
                    }
                } else if (!check_assignable(var_type, init_type, decl->loc)) {
                    // Error already reported by check_assignable
                } else {
                    coerce_int_literal(var_decl.initializer, var_type);
                }
            } else if (!var_type) {
                error(decl->loc, "variable declaration requires type annotation or initializer");
                var_type = m_types.error_type();
            }

            var_decl.resolved_type = var_type;
            Symbol* sym = m_symbols.define(SymbolKind::Variable, var_decl.name, var_type, decl->loc, decl);
            sym->is_pub = var_decl.is_pub;
        }
        else if (decl->kind == AstKind::DeclConstructor) {
            analyze_constructor_decl(decl);
        }
        else if (decl->kind == AstKind::DeclDestructor) {
            analyze_destructor_decl(decl);
        }
        else if (decl->kind == AstKind::DeclMethod) {
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

                        // Resolve trait type args
                        Span<Type*> resolved_trait_type_args;
                        if (method_decl.trait_type_args.size() > 0) {
                            if (trait_type_info.type_params.size() == 0) {
                                error_fmt(decl->loc, "trait '{}' does not take type arguments", method_decl.trait_name);
                                continue;
                            }
                            if (method_decl.trait_type_args.size() != trait_type_info.type_params.size()) {
                                error_fmt(decl->loc, "trait '{}' expects {} type argument(s), got {}",
                                         method_decl.trait_name, trait_type_info.type_params.size(), method_decl.trait_type_args.size());
                                continue;
                            }
                            Vector<Type*> args;
                            for (auto* type_arg : method_decl.trait_type_args) {
                                Type* arg_type = resolve_type_expr(type_arg);
                                if (!arg_type) arg_type = m_types.error_type();
                                args.push_back(arg_type);
                            }
                            resolved_trait_type_args = m_allocator.alloc_span(args);
                        } else if (trait_type_info.type_params.size() > 0) {
                            // Default all type params to Self (the implementing struct type)
                            // This enables `for Add` as shorthand for `for Add<Vec2>` on struct Vec2
                            Vector<Type*> args;
                            for (u32 i = 0; i < trait_type_info.type_params.size(); i++) {
                                args.push_back(struct_type_lookup);
                            }
                            resolved_trait_type_args = m_allocator.alloc_span(args);
                        }

                        m_pending_trait_impls.push_back({decl, struct_type_lookup, trait_type, resolved_trait_type_args});
                    }
                }
            }
            else {
                // Regular method (no trait involvement)
                analyze_method_decl(decl);
            }
        }
        else if (decl->kind == AstKind::DeclTrait) {
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
    }

    // Now validate all trait implementations
    validate_trait_implementations();
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
            // Skip generic function templates - they're analyzed when instantiated
            if (decl->fun_decl.type_params.size() > 0) continue;
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
            Type* ctor_struct = m_type_env.named_type_by_name(constructor_decl.struct_name);
            if (ctor_struct) {
                analyze_constructor_body(decl, ctor_struct);
            }
        }
        else if (decl->kind == AstKind::DeclDestructor) {
            DestructorDecl& destructor_decl = decl->destructor_decl;
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
                // Register the concrete struct type
                m_type_env.register_named_type(inst->mangled_name, inst->concrete_type);

                // Resolve members (fields, slot layout) — mirrors resolve_type_members logic
                StructDecl& struct_decl = inst->instantiated_decl->struct_decl;
                StructTypeInfo& struct_type_info = inst->concrete_type->struct_info;

                Vector<FieldInfo> fields;
                u32 slot_offset = 0;
                for (u32 j = 0; j < struct_decl.fields.size(); j++) {
                    Type* field_type = resolve_type_expr(struct_decl.fields[j].type);
                    if (!field_type) field_type = m_types.error_type();

                    u32 slot_count = get_type_slot_count(field_type);

                    FieldInfo field_info;
                    field_info.name = struct_decl.fields[j].name;
                    field_info.type = field_type;
                    field_info.is_pub = struct_decl.fields[j].is_pub;
                    field_info.index = j;
                    field_info.slot_offset = slot_offset;
                    field_info.slot_count = slot_count;
                    fields.push_back(field_info);
                    slot_offset += slot_count;
                }

                // Allocate field info in bump allocator
                struct_type_info.fields = m_allocator.alloc_span(fields);
                struct_type_info.slot_count = slot_offset;

                inst->is_analyzed = true;
            }
        }

        // Process pending function instances
        if (m_type_env.generics().has_pending_funs()) {
            auto pending_funs = m_type_env.generics().take_pending_funs();
            for (auto* inst : pending_funs) {
                analyze_fun_decl(inst->instantiated_decl);
                inst->is_analyzed = true;
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

    Vector<FieldInfo> fields;
    u32 slot_offset = 0;
    for (u32 fi = 0; fi < struct_decl.fields.size(); fi++) {
        Type* field_type = resolve_type_expr(struct_decl.fields[fi].type);
        if (!field_type) field_type = m_types.error_type();

        u32 slot_count = get_type_slot_count(field_type);

        FieldInfo info;
        info.name = struct_decl.fields[fi].name;
        info.type = field_type;
        info.is_pub = struct_decl.fields[fi].is_pub;
        info.index = fi;
        info.slot_offset = slot_offset;
        info.slot_count = slot_count;
        fields.push_back(info);
        slot_offset += slot_count;
    }

    struct_type_info.fields = m_allocator.alloc_span(fields);
    struct_type_info.slot_count = slot_offset;
    inst->is_analyzed = true;
}

Type* SemanticAnalyzer::resolve_type_expr(TypeExpr* type_expr) {
    if (!type_expr) return nullptr;

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
        if (!var_type) {
            // Type inference
            var_type = init_type;
            if (var_type->is_nil()) {
                error(decl->loc, "cannot infer type from nil literal");
                var_type = m_types.error_type();
            } else if (var_type->is_int_literal()) {
                // Default unsuffixed integer literals to i32
                var_type = m_types.i32_type();
                coerce_int_literal(var_decl.initializer, var_type);
            }
        } else if (!check_assignable(var_type, init_type, decl->loc)) {
            // Error already reported
        } else {
            // Coerce int literals to the annotated type
            coerce_int_literal(var_decl.initializer, var_type);
        }
    } else if (!var_type) {
        error(decl->loc, "variable declaration requires type annotation or initializer");
        var_type = m_types.error_type();
    }

    var_decl.resolved_type = var_type;
    m_symbols.define(SymbolKind::Variable, var_decl.name, var_type, decl->loc, decl);
}

void SemanticAnalyzer::analyze_fun_decl(Decl* decl) {
    FunDecl& fun_decl = decl->fun_decl;

    // Native functions don't have bodies
    if (fun_decl.is_native) return;
    if (!fun_decl.body) return;

    // Resolve return type
    Type* return_type = fun_decl.return_type ? resolve_type_expr(fun_decl.return_type) : m_types.void_type();

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
    }

    // Analyze body
    analyze_stmt(fun_decl.body);

    // Check return paths (simplified - doesn't track all paths)
    // A more complete implementation would track control flow
    if (!return_type->is_void() && !return_type->is_error()) {
        // For now, we just warn if there's no return at all
        // A full implementation would check all paths
    }

    m_symbols.pop_scope();
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
            } else {
                // For structs/enums, define as regular types
                m_symbols.define(static_cast<SymbolKind>(
                    exp->kind == ExportKind::Struct ? SymbolKind::Struct : SymbolKind::Enum),
                    local_name, exp->type, name.loc, exp->decl);
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
    }

    // Analyze body
    analyze_stmt(body);

    m_symbols.pop_scope();  // function scope
    m_symbols.pop_scope();  // struct scope
}

void SemanticAnalyzer::analyze_constructor_body(Decl* decl, Type* struct_type) {
    auto& cd = decl->constructor_decl;
    analyze_member_body(decl, struct_type, cd.params, cd.body, m_types.void_type());
}

void SemanticAnalyzer::analyze_destructor_body(Decl* decl, Type* struct_type) {
    auto& dd = decl->destructor_decl;
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

Type* SemanticAnalyzer::resolve_trait_type_expr(TypeExpr* type_expr,
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

    // Check for duplicate method names in this trait
    TraitTypeInfo& trait_type_info = trait_type->trait_info;
    for (const auto& trait_method : trait_type_info.methods) {
        if (trait_method.name == method_decl.name) {
            error_fmt(decl->loc, "duplicate trait method '{}' in trait '{}'",
                     method_decl.name, trait_type_info.name);
            return;
        }
    }

    // Resolve parameter types - use TypeKind::Self for Self, TypeParam for trait type params
    Vector<Type*> param_types;
    for (const auto& param : method_decl.params) {
        param_types.push_back(resolve_trait_type_expr(param.type, trait_type_info));
    }

    // Resolve return type (TypeKind::Self for Self, TypeParam for trait type params)
    Type* return_type = method_decl.return_type
        ? resolve_trait_type_expr(method_decl.return_type, trait_type_info)
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

// Helper to deep-clone an Expr tree
static Expr* clone_expr(BumpAllocator& alloc, Expr* expr);
static Stmt* clone_stmt(BumpAllocator& alloc, Stmt* stmt);

static Span<CallArg> clone_call_args(BumpAllocator& alloc, Span<CallArg> args) {
    if (args.size() == 0) return {};
    CallArg* data = reinterpret_cast<CallArg*>(alloc.alloc_bytes(sizeof(CallArg) * args.size(), alignof(CallArg)));
    for (u32 i = 0; i < args.size(); i++) {
        data[i] = args[i];
        data[i].expr = clone_expr(alloc, args[i].expr);
    }
    return Span<CallArg>(data, args.size());
}

static Span<FieldInit> clone_field_inits(BumpAllocator& alloc, Span<FieldInit> fields) {
    if (fields.size() == 0) return {};
    FieldInit* data = reinterpret_cast<FieldInit*>(alloc.alloc_bytes(sizeof(FieldInit) * fields.size(), alignof(FieldInit)));
    for (u32 i = 0; i < fields.size(); i++) {
        data[i] = fields[i];
        data[i].value = clone_expr(alloc, fields[i].value);
    }
    return Span<FieldInit>(data, fields.size());
}

static Expr* clone_expr(BumpAllocator& alloc, Expr* expr) {
    if (!expr) return nullptr;
    Expr* e = alloc.emplace<Expr>();
    *e = *expr;
    e->resolved_type = nullptr;  // Will be set by re-analysis

    switch (expr->kind) {
        case AstKind::ExprLiteral:
        case AstKind::ExprIdentifier:
        case AstKind::ExprThis:
            break;  // No child nodes to clone
        case AstKind::ExprUnary:
            e->unary.operand = clone_expr(alloc, expr->unary.operand);
            break;
        case AstKind::ExprBinary:
            e->binary.left = clone_expr(alloc, expr->binary.left);
            e->binary.right = clone_expr(alloc, expr->binary.right);
            break;
        case AstKind::ExprTernary:
            e->ternary.condition = clone_expr(alloc, expr->ternary.condition);
            e->ternary.then_expr = clone_expr(alloc, expr->ternary.then_expr);
            e->ternary.else_expr = clone_expr(alloc, expr->ternary.else_expr);
            break;
        case AstKind::ExprCall:
            e->call.callee = clone_expr(alloc, expr->call.callee);
            e->call.arguments = clone_call_args(alloc, expr->call.arguments);
            break;
        case AstKind::ExprIndex:
            e->index.object = clone_expr(alloc, expr->index.object);
            e->index.index = clone_expr(alloc, expr->index.index);
            break;
        case AstKind::ExprGet:
            e->get.object = clone_expr(alloc, expr->get.object);
            break;
        case AstKind::ExprStaticGet:
            break;  // No child nodes
        case AstKind::ExprAssign:
            e->assign.target = clone_expr(alloc, expr->assign.target);
            e->assign.value = clone_expr(alloc, expr->assign.value);
            break;
        case AstKind::ExprGrouping:
            e->grouping.expr = clone_expr(alloc, expr->grouping.expr);
            break;
        case AstKind::ExprSuper:
            break;  // No child nodes
        case AstKind::ExprStructLiteral:
            e->struct_literal.fields = clone_field_inits(alloc, expr->struct_literal.fields);
            break;
        case AstKind::ExprStringInterp: {
            // Clone expression children; StringView parts are shared (immutable)
            u32 n = expr->string_interp.expressions.size();
            if (n > 0) {
                Expr** exprs = reinterpret_cast<Expr**>(alloc.alloc_bytes(sizeof(Expr*) * n, alignof(Expr*)));
                for (u32 i = 0; i < n; i++) {
                    exprs[i] = clone_expr(alloc, expr->string_interp.expressions[i]);
                }
                e->string_interp.expressions = Span<Expr*>(exprs, n);
            }
            break;
        }
        default:
            break;
    }
    return e;
}

static Span<Decl*> clone_decl_list(BumpAllocator& alloc, Span<Decl*> decls);

static Stmt* clone_stmt(BumpAllocator& alloc, Stmt* stmt) {
    if (!stmt) return nullptr;
    Stmt* s = alloc.emplace<Stmt>();
    *s = *stmt;

    switch (stmt->kind) {
        case AstKind::StmtExpr:
            s->expr_stmt.expr = clone_expr(alloc, stmt->expr_stmt.expr);
            break;
        case AstKind::StmtBlock:
            s->block.declarations = clone_decl_list(alloc, stmt->block.declarations);
            break;
        case AstKind::StmtIf:
            s->if_stmt.condition = clone_expr(alloc, stmt->if_stmt.condition);
            s->if_stmt.then_branch = clone_stmt(alloc, stmt->if_stmt.then_branch);
            s->if_stmt.else_branch = clone_stmt(alloc, stmt->if_stmt.else_branch);
            break;
        case AstKind::StmtWhile:
            s->while_stmt.condition = clone_expr(alloc, stmt->while_stmt.condition);
            s->while_stmt.body = clone_stmt(alloc, stmt->while_stmt.body);
            break;
        case AstKind::StmtFor:
            // initializer is a Decl*
            s->for_stmt.condition = clone_expr(alloc, stmt->for_stmt.condition);
            s->for_stmt.increment = clone_expr(alloc, stmt->for_stmt.increment);
            s->for_stmt.body = clone_stmt(alloc, stmt->for_stmt.body);
            break;
        case AstKind::StmtReturn:
            s->return_stmt.value = clone_expr(alloc, stmt->return_stmt.value);
            break;
        case AstKind::StmtBreak:
        case AstKind::StmtContinue:
            break;
        case AstKind::StmtDelete:
            s->delete_stmt.expr = clone_expr(alloc, stmt->delete_stmt.expr);
            s->delete_stmt.arguments = clone_call_args(alloc, stmt->delete_stmt.arguments);
            break;
        case AstKind::StmtWhen:
            s->when_stmt.discriminant = clone_expr(alloc, stmt->when_stmt.discriminant);
            // Clone when cases
            if (stmt->when_stmt.cases.size() > 0) {
                WhenCase* cases = reinterpret_cast<WhenCase*>(
                    alloc.alloc_bytes(sizeof(WhenCase) * stmt->when_stmt.cases.size(), alignof(WhenCase)));
                for (u32 i = 0; i < stmt->when_stmt.cases.size(); i++) {
                    cases[i] = stmt->when_stmt.cases[i];
                    cases[i].body = clone_decl_list(alloc, stmt->when_stmt.cases[i].body);
                }
                s->when_stmt.cases = Span<WhenCase>(cases, stmt->when_stmt.cases.size());
            }
            s->when_stmt.else_body = clone_decl_list(alloc, stmt->when_stmt.else_body);
            break;
        default:
            break;
    }
    return s;
}

static Decl* clone_decl(BumpAllocator& alloc, Decl* decl) {
    if (!decl) return nullptr;
    Decl* d = alloc.emplace<Decl>();
    *d = *decl;

    switch (decl->kind) {
        case AstKind::DeclVar:
            d->var_decl.initializer = clone_expr(alloc, decl->var_decl.initializer);
            break;
        case AstKind::StmtExpr:
            d->stmt.expr_stmt.expr = clone_expr(alloc, decl->stmt.expr_stmt.expr);
            break;
        default:
            // For statement declarations embedded in Decl, clone the statement
            if (decl->kind >= AstKind::StmtExpr && decl->kind <= AstKind::StmtTry) {
                Stmt* cloned = clone_stmt(alloc, &decl->stmt);
                if (cloned) d->stmt = *cloned;
            }
            break;
    }
    return d;
}

static Span<Decl*> clone_decl_list(BumpAllocator& alloc, Span<Decl*> decls) {
    if (decls.size() == 0) return {};
    Decl** data = reinterpret_cast<Decl**>(alloc.alloc_bytes(sizeof(Decl*) * decls.size(), alignof(Decl*)));
    for (u32 i = 0; i < decls.size(); i++) {
        data[i] = clone_decl(alloc, decls[i]);
    }
    return Span<Decl*>(data, decls.size());
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

void SemanticAnalyzer::resolve_generic_bounds() {
    // Resolve bounds for all generic function templates
    for (const auto& entry : m_type_env.generics().generic_funs_map()) {
        StringView name = entry.first;
        Decl* decl = entry.second;
        FunDecl& fun_decl = decl->fun_decl;

        bool has_bounds = false;
        for (const auto& type_param : fun_decl.type_params) {
            if (type_param.bounds.size() > 0) { has_bounds = true; break; }
        }
        if (!has_bounds) continue;

        Vector<Span<TraitBound>> all_param_bounds;
        for (const auto& type_param : fun_decl.type_params) {
            Span<TraitBound> bounds = resolve_type_param_bounds(type_param.bounds, type_param.loc);
            all_param_bounds.push_back(bounds);
        }

        ResolvedTypeParams resolved;
        resolved.param_bounds = m_allocator.alloc_span(all_param_bounds);
        m_type_env.generics().set_fun_bounds(name, resolved);
    }

    // Resolve bounds for all generic struct templates
    for (const auto& entry : m_type_env.generics().generic_structs_map()) {
        StringView name = entry.first;
        Decl* decl = entry.second;
        StructDecl& struct_decl = decl->struct_decl;

        bool has_bounds = false;
        for (const auto& type_param : struct_decl.type_params) {
            if (type_param.bounds.size() > 0) { has_bounds = true; break; }
        }
        if (!has_bounds) continue;

        Vector<Span<TraitBound>> all_param_bounds;
        for (const auto& type_param : struct_decl.type_params) {
            Span<TraitBound> bounds = resolve_type_param_bounds(type_param.bounds, type_param.loc);
            all_param_bounds.push_back(bounds);
        }

        ResolvedTypeParams resolved;
        resolved.param_bounds = m_allocator.alloc_span(all_param_bounds);
        m_type_env.generics().set_struct_bounds(name, resolved);
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
                auto concrete_str = type_string(concrete_type);
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

void SemanticAnalyzer::validate_trait_implementations() {
    // Group pending impls by (struct, trait, type_args)
    struct TraitImplGroup {
        Type* struct_type;
        Type* trait_type;
        Span<Type*> trait_type_args;
        Vector<Decl*> impl_decls;
    };

    // Use a simple list of groups since we don't expect many
    Vector<TraitImplGroup> groups;

    for (auto& pending : m_pending_trait_impls) {
        // Find or create group - match on struct, trait, AND type args
        TraitImplGroup* group = nullptr;
        for (auto& g : groups) {
            if (g.struct_type == pending.struct_type && g.trait_type == pending.trait_type) {
                // Also check type args match element-wise
                bool args_match = g.trait_type_args.size() == pending.trait_type_args.size();
                if (args_match) {
                    for (u32 i = 0; i < g.trait_type_args.size(); i++) {
                        if (g.trait_type_args[i] != pending.trait_type_args[i]) {
                            args_match = false;
                            break;
                        }
                    }
                }
                if (args_match) {
                    group = &g;
                    break;
                }
            }
        }
        if (!group) {
            groups.push_back({pending.struct_type, pending.trait_type, pending.trait_type_args, {}});
            group = &groups.back();
        }
        group->impl_decls.push_back(pending.decl);
    }

    // Process each group
    for (auto& group : groups) {
        Type* struct_type = group.struct_type;
        Type* trait_type = group.trait_type;
        Span<Type*> trait_type_args = group.trait_type_args;
        TraitTypeInfo& trait_type_info = trait_type->trait_info;
        StructTypeInfo& struct_type_info = struct_type->struct_info;

        // Check parent trait requirement
        if (trait_type_info.parent) {
            // Check that struct also implements parent trait
            bool has_parent = false;
            for (const auto& impl : struct_type_info.implemented_traits) {
                if (impl.trait == trait_type_info.parent) {
                    has_parent = true;
                    break;
                }
            }
            // Also check if we're implementing it in this batch
            if (!has_parent) {
                for (auto& other_group : groups) {
                    if (other_group.struct_type == struct_type && other_group.trait_type == trait_type_info.parent) {
                        has_parent = true;
                        break;
                    }
                }
            }
            if (!has_parent) {
                error_fmt(group.impl_decls[0]->loc,
                         "trait '{}' requires parent trait '{}' to be implemented for '{}'",
                         trait_type_info.name, trait_type_info.parent->trait_info.name, struct_type_info.name);
                continue;
            }
        }

        // Track which trait methods are implemented
        Vector<bool> implemented(trait_type_info.methods.size(), false);

        for (Decl* decl : group.impl_decls) {
            MethodDecl& method_decl = decl->method_decl;

            // Find the trait method this implements
            bool found = false;
            for (u32 i = 0; i < trait_type_info.methods.size(); i++) {
                if (trait_type_info.methods[i].name == method_decl.name) {
                    found = true;
                    implemented[i] = true;

                    // Validate parameter count matches
                    if (method_decl.params.size() != trait_type_info.methods[i].param_types.size()) {
                        error_fmt(decl->loc, "method '{}' parameter count mismatch with trait '{}'",
                                 method_decl.name, trait_type_info.name);
                    }

                    // Register as a regular method on the struct
                    // Resolve param types with Self -> struct_type
                    Vector<Type*> param_types;
                    for (const auto& param : method_decl.params) {
                        Type* ptype = resolve_type_expr(param.type);
                        if (!ptype) ptype = m_types.error_type();
                        param_types.push_back(ptype);
                    }

                    Type* return_type = method_decl.return_type ? resolve_type_expr(method_decl.return_type) : m_types.void_type();
                    if (!return_type) return_type = m_types.error_type();

                    // Validate parameter types match trait signature
                    if (method_decl.params.size() == trait_type_info.methods[i].param_types.size()) {
                        for (u32 p = 0; p < param_types.size(); p++) {
                            if (param_types[p]->is_error()) continue;
                            Type* expected = resolve_trait_type(trait_type_info.methods[i].param_types[p],
                                                                struct_type, trait_type_args);
                            if (param_types[p] != expected) {
                                auto got_str = type_string(param_types[p]);
                                auto exp_str = type_string(expected);
                                error_fmt(decl->loc,
                                         "method '{}' parameter {} has type '{}' but trait '{}' expects '{}'",
                                         method_decl.name, p + 1, got_str.data(), trait_type_info.name, exp_str.data());
                            }
                        }
                    }

                    // Validate return type matches trait signature
                    if (!return_type->is_error()) {
                        Type* expected_ret = resolve_trait_type(trait_type_info.methods[i].return_type,
                                                                struct_type, trait_type_args);
                        if (return_type != expected_ret) {
                            auto got_str = type_string(return_type);
                            auto exp_str = type_string(expected_ret);
                            error_fmt(decl->loc,
                                     "method '{}' return type is '{}' but trait '{}' expects '{}'",
                                     method_decl.name, got_str.data(), trait_type_info.name, exp_str.data());
                        }
                    }

                    // Check for duplicate method name on struct
                    bool is_duplicate = false;
                    for (const auto& method : struct_type_info.methods) {
                        if (method.name == method_decl.name) {
                            is_duplicate = true;
                            break;
                        }
                    }

                    if (!is_duplicate) {
                        MethodInfo method_info;
                        method_info.name = method_decl.name;
                        method_info.param_types = m_allocator.alloc_span(param_types);
                        method_info.return_type = return_type;
                        method_info.decl = decl;

                        // Add to struct's method list
                        append_method(struct_type_info, method_info);
                    }
                    break;
                }
            }

            if (!found) {
                error_fmt(decl->loc, "method '{}' is not defined in trait '{}'",
                         method_decl.name, trait_type_info.name);
            }
        }

        // Check for missing required methods; inject defaults for unimplemented default methods
        for (u32 i = 0; i < trait_type_info.methods.size(); i++) {
            if (!implemented[i]) {
                if (trait_type_info.methods[i].has_default) {
                    // Inject default implementation
                    inject_default_method(struct_type, trait_type, trait_type_info.methods[i], trait_type_args);
                } else {
                    error_fmt(group.impl_decls[0]->loc,
                             "trait '%.*s' requires method '%.*s' which is not implemented for '%.*s'",
                             trait_type_info.name.size(), trait_type_info.name.data(),
                             trait_type_info.methods[i].name.size(), trait_type_info.methods[i].name.data(),
                             struct_type_info.name.size(), struct_type_info.name.data());
                }
            }
        }

        // Update struct's implemented_traits list
        Vector<TraitImplRecord> trait_records;
        for (const auto& impl : struct_type_info.implemented_traits) {
            trait_records.push_back(impl);
        }
        trait_records.push_back(TraitImplRecord{trait_type, trait_type_args});

        struct_type_info.implemented_traits = m_allocator.alloc_span(trait_records);
    }
}

Type* SemanticAnalyzer::resolve_trait_type(Type* abstract_type, Type* struct_type, Span<Type*> trait_type_args) {
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
        param_types.push_back(resolve_trait_type(param_type, struct_type, trait_type_args));
    }
    Type* return_type = resolve_trait_type(trait_method_info.return_type, struct_type, trait_type_args);

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

    m_symbols.pop_scope();
}

void SemanticAnalyzer::analyze_if_stmt(Stmt* stmt) {
    IfStmt& is = stmt->if_stmt;

    Type* cond_type = analyze_expr(is.condition);
    if (cond_type && !cond_type->is_error()) {
        check_boolean(cond_type, is.condition->loc);
    }

    analyze_stmt(is.then_branch);
    if (is.else_branch) {
        analyze_stmt(is.else_branch);
    }
}

void SemanticAnalyzer::analyze_while_stmt(Stmt* stmt) {
    WhileStmt& ws = stmt->while_stmt;

    Type* cond_type = analyze_expr(ws.condition);
    if (cond_type && !cond_type->is_error()) {
        check_boolean(cond_type, ws.condition->loc);
    }

    m_symbols.push_loop_scope();
    analyze_stmt(ws.body);
    m_symbols.pop_scope();
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
            check_boolean(cond_type, fs.condition->loc);
        }
    }

    if (fs.increment) {
        analyze_expr(fs.increment);
    }

    m_symbols.push_loop_scope();
    analyze_stmt(fs.body);
    m_symbols.pop_scope();

    m_symbols.pop_scope();
}

void SemanticAnalyzer::analyze_return_stmt(Stmt* stmt) {
    ReturnStmt& rs = stmt->return_stmt;

    if (!m_symbols.is_in_function()) {
        error(stmt->loc, "'return' statement outside of function");
        return;
    }

    Type* expected = m_symbols.current_return_type();

    if (rs.value) {
        Type* actual = analyze_expr(rs.value);
        if (!check_assignable(expected, actual, stmt->loc)) {
            // Error already reported
        } else {
            coerce_int_literal(rs.value, expected);
        }
    } else {
        if (!expected->is_void()) {
            error(stmt->loc, "non-void function must return a value");
        }
    }

    m_symbols.mark_return();
}

void SemanticAnalyzer::analyze_break_stmt(Stmt* stmt) {
    if (!m_symbols.is_in_loop()) {
        error(stmt->loc, "'break' statement outside of loop");
    }
}

void SemanticAnalyzer::analyze_continue_stmt(Stmt* stmt) {
    if (!m_symbols.is_in_loop()) {
        error(stmt->loc, "'continue' statement outside of loop");
    }
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

    // Get the inner struct type
    Type* inner_type = type->ref_info.inner_type;
    if (!inner_type || !inner_type->is_struct()) {
        // No destructor validation needed for non-struct types
        return;
    }

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

        // Check argument types and modifiers
        DestructorDecl* dtor_decl = &dtor->decl->destructor_decl;
        check_call_args(ds.arguments, dtor->param_types, dtor_decl->params, stmt->loc);
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
    }

    // Analyze else body if present
    if (ws.else_body.size() > 0) {
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
    }
}

void SemanticAnalyzer::analyze_throw_stmt(Stmt* stmt) {
    ThrowStmt& ts = stmt->throw_stmt;

    Type* expr_type = analyze_expr(ts.expr);
    if (!expr_type || expr_type->is_error()) return;

    // The thrown expression must be a struct type that implements Exception trait
    Type* base = expr_type->base_type();
    if (!base->is_struct()) {
        error(stmt->loc, "throw expression must be a struct type that implements Exception");
        return;
    }

    Type* exception_trait = m_type_env.exception_type();
    if (!m_types.implements_trait(base, exception_trait)) {
        error_fmt(stmt->loc, "thrown type '{}' does not implement the Exception trait",
                  base->struct_info.name);
    }
}

void SemanticAnalyzer::analyze_try_stmt(Stmt* stmt) {
    TryStmt& ts = stmt->try_stmt;

    // Analyze try body
    analyze_stmt(ts.try_body);

    bool has_catch_all = false;

    // Analyze each catch clause
    for (u32 i = 0; i < ts.catches.size(); i++) {
        CatchClause& clause = ts.catches[i];

        if (has_catch_all) {
            error(clause.loc, "catch clause after catch-all is unreachable");
            continue;
        }

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

        analyze_stmt(clause.body);
        m_symbols.pop_scope();
    }

    // Analyze finally body if present
    if (ts.finally_body) {
        analyze_stmt(ts.finally_body);
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
            coerce_int_literal(expression, m_types.i32_type());
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

    Symbol* sym = m_symbols.lookup(id.name);
    if (!sym) {
        error_fmt(expr->loc, "undefined identifier '{}'", id.name);
        return m_types.error_type();
    }

    return sym->type;
}

Type* SemanticAnalyzer::analyze_unary_expr(Expr* expr) {
    UnaryExpr& unary_expr = expr->unary;

    Type* operand_type = analyze_expr(unary_expr.operand);
    if (operand_type->is_error()) return m_types.error_type();

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
        coerce_int_literal(binary_expr.left, right_type);
        left_type = right_type;
    } else if (right_type->is_int_literal() && left_type->is_integer()) {
        coerce_int_literal(binary_expr.right, left_type);
        right_type = left_type;
    } else if (left_type->is_int_literal() && right_type->is_int_literal()) {
        coerce_int_literal(binary_expr.left, m_types.i32_type());
        coerce_int_literal(binary_expr.right, m_types.i32_type());
        left_type = m_types.i32_type();
        right_type = m_types.i32_type();
    } else if (right_type->is_int_literal() && !left_type->is_int_literal()) {
        // Right is IntLiteral, left is non-integer (e.g., struct) — coerce to method's param type
        const char* method_name = binary_op_to_trait_method(binary_expr.op);
        if (method_name) {
            StringView name(method_name, static_cast<u32>(strlen(method_name)));
            const MethodInfo* mi = m_types.lookup_method(left_type, name);
            if (mi && mi->param_types.size() == 1 && mi->param_types[0]->is_integer()) {
                coerce_int_literal(binary_expr.right, mi->param_types[0]);
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
                coerce_int_literal(binary_expr.left, mi->param_types[0]);
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
        check_boolean(cond_type, ternary_expr.condition->loc);
    }

    Type* then_type = analyze_expr(ternary_expr.then_expr);
    Type* else_type = analyze_expr(ternary_expr.else_expr);

    if (then_type->is_error()) return else_type;
    if (else_type->is_error()) return then_type;

    // Coerce int literals in ternary branches
    if (then_type->is_int_literal() && else_type->is_integer()) {
        coerce_int_literal(ternary_expr.then_expr, else_type);
        then_type = else_type;
    } else if (else_type->is_int_literal() && then_type->is_integer()) {
        coerce_int_literal(ternary_expr.else_expr, then_type);
        else_type = then_type;
    } else if (then_type->is_int_literal() && else_type->is_int_literal()) {
        coerce_int_literal(ternary_expr.then_expr, m_types.i32_type());
        coerce_int_literal(ternary_expr.else_expr, m_types.i32_type());
        then_type = m_types.i32_type();
        else_type = m_types.i32_type();
    }

    // Types must be compatible
    if (then_type == else_type) {
        return then_type;
    }

    // Check if one can be converted to the other (probe without errors)
    if (is_assignable(then_type, else_type)) {
        return then_type;
    }
    if (is_assignable(else_type, then_type)) {
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

        // Type check (skip for 'out' since it's write-only)
        if (arg.modifier != ParamModifier::Out) {
            check_assignable(param_types[i], arg_type, arg.expr->loc);
            coerce_int_literal(arg.expr, param_types[i]);
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

    // Instantiate the generic function
    StringView mangled = m_type_env.generics().instantiate_fun(func_name, type_args);
    ce.mangled_name = mangled;

    // Use the instantiated function's substituted types for type checking
    GenericFunInstance* inst = m_type_env.generics().find_fun_instance(mangled);
    FunDecl& inst_fun_decl = inst->instantiated_decl->fun_decl;

    // Resolve parameter types and type-check arguments
    if (ce.arguments.size() != inst_fun_decl.params.size()) {
        error_fmt(expr->loc, "function '{}' expects {} arguments but got {}",
                 func_name, inst_fun_decl.params.size(), ce.arguments.size());
        return m_types.error_type();
    }

    for (u32 i = 0; i < ce.arguments.size(); i++) {
        CallArg& arg = ce.arguments[i];
        Type* arg_type = analyze_expr(arg.expr);

        // Resolve the parameter type from the instantiated function
        Type* param_type = nullptr;
        if (inst_fun_decl.params[i].type) {
            param_type = resolve_type_expr(inst_fun_decl.params[i].type);
        }

        if (param_type && !param_type->is_error() && arg_type && !arg_type->is_error()) {
            check_assignable(param_type, arg_type, arg.expr->loc);
            coerce_int_literal(arg.expr, param_type);
        }
    }

    // Resolve return type from the instantiated function
    Type* return_type = m_types.void_type();
    if (inst_fun_decl.return_type) {
        return_type = resolve_type_expr(inst_fun_decl.return_type);
    }

    return return_type;
}

void SemanticAnalyzer::populate_enum_methods(Type* type) {
    assert(type && type->is_enum());

    // eq(other: Self): bool, ne(other: Self): bool
    MethodInfo* methods = reinterpret_cast<MethodInfo*>(
        m_allocator.alloc_bytes(sizeof(MethodInfo) * 2, alignof(MethodInfo)));

    Type** enum_param = reinterpret_cast<Type**>(
        m_allocator.alloc_bytes(sizeof(Type*), alignof(Type*)));
    enum_param[0] = type;
    Span<Type*> param_span(enum_param, 1);

    methods[0].name = StringView("eq", 2);
    methods[0].param_types = param_span;
    methods[0].return_type = m_types.bool_type();
    methods[0].decl = nullptr;
    methods[0].native_name = StringView();

    methods[1].name = StringView("ne", 2);
    methods[1].param_types = param_span;
    methods[1].return_type = m_types.bool_type();
    methods[1].decl = nullptr;
    methods[1].native_name = StringView();

    type->enum_info.methods = Span<MethodInfo>(methods, 2);
}

NativeRegistry* SemanticAnalyzer::get_builtin_registry() {
    ModuleInfo* builtin = m_modules.find_module(BUILTIN_MODULE_NAME);
    NativeRegistry* registry = builtin ? builtin->natives : nullptr;
    if (!registry) registry = m_registry;
    return registry;
}

void SemanticAnalyzer::populate_list_methods(Type* type) {
    assert(type && type->is_list());
    if (type->list_info.methods.size() > 0) return;

    NativeRegistry* registry = get_builtin_registry();
    if (!registry) return;

    Type* type_args[] = { type->list_info.element_type };
    Span<Type*> ta(type_args, 1);
    Span<MethodInfo> native_methods = registry->instantiate_generic_methods(
        "List", ta, m_allocator, m_types);
    type->list_info.alloc_native_name = registry->get_generic_alloc_name("List");
    type->list_info.copy_native_name = registry->get_generic_copy_name("List");

    type->list_info.methods = native_methods;
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
            check_assignable(ctor.param_types[i], arg_type, ce.arguments[i].expr->loc);
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
    // Structs implementing Hash trait (not yet supported at runtime)
    // if (type->is_struct() && m_types.implements_trait(type, m_type_env.hash_type())) return true;
    return false;
}

void SemanticAnalyzer::populate_map_methods(Type* type) {
    assert(type && type->is_map());
    if (type->map_info.methods.size() > 0) return;

    NativeRegistry* registry = get_builtin_registry();
    if (!registry) return;

    Type* type_args[] = { type->map_info.key_type, type->map_info.value_type };
    Span<Type*> ta(type_args, 2);
    Span<MethodInfo> native_methods = registry->instantiate_generic_methods(
        "Map", ta, m_allocator, m_types);
    type->map_info.alloc_native_name = registry->get_generic_alloc_name("Map");
    type->map_info.copy_native_name = registry->get_generic_copy_name("Map");

    type->map_info.methods = native_methods;
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
            check_assignable(m_types.i32_type(), arg_type, ce.arguments[i].expr->loc);
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
                if (!check_assignable(ctor->param_types[i], arg_type, arg.expr->loc)) {
                    // Error already reported
                } else {
                    coerce_int_literal(arg.expr, ctor->param_types[i]);
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
            if (!check_assignable(ctor->param_types[i], arg_type, arg.expr->loc)) {
                // Error already reported
            } else {
                coerce_int_literal(arg.expr, ctor->param_types[i]);
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
            if (!check_assignable(mi->param_types[i], arg_type, arg.expr->loc)) {
                // Error already reported
            } else {
                coerce_int_literal(arg.expr, mi->param_types[i]);
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

    // Generic function call WITHOUT explicit type args — attempt inference
    if (call_expr.type_args.size() == 0 && call_expr.callee->kind == AstKind::ExprIdentifier) {
        StringView func_name = call_expr.callee->identifier.name;
        if (m_type_env.generics().is_generic_fun(func_name)) {
            Decl* template_decl = m_type_env.generics().get_generic_fun_decl(func_name);
            FunDecl& template_fun_decl = template_decl->fun_decl;

            auto inferred = infer_type_args_from_call(
                template_fun_decl.type_params, template_fun_decl.params,
                call_expr.arguments, expr->loc);

            if (inferred.success) {
                Span<Type*> type_args = m_allocator.alloc_span(inferred.type_args);

                // Check trait bounds on inferred type args
                const ResolvedTypeParams* bounds = m_type_env.generics().get_fun_bounds(func_name);
                if (!check_type_arg_bounds(func_name, type_args, bounds, expr->loc)) {
                    return m_types.error_type();
                }

                StringView mangled = m_type_env.generics().instantiate_fun(func_name, type_args);
                call_expr.mangled_name = mangled;

                // Type-check arguments against instantiated function
                GenericFunInstance* inst = m_type_env.generics().find_fun_instance(mangled);
                FunDecl& inst_fun_decl = inst->instantiated_decl->fun_decl;

                if (call_expr.arguments.size() != inst_fun_decl.params.size()) {
                    error_fmt(expr->loc, "function '{}' expects {} arguments but got {}",
                             func_name, inst_fun_decl.params.size(), call_expr.arguments.size());
                    return m_types.error_type();
                }

                for (u32 i = 0; i < call_expr.arguments.size(); i++) {
                    // Args were already analyzed in infer_type_args_from_call,
                    // so just resolve param type and check assignability
                    Type* arg_type = call_expr.arguments[i].expr->resolved_type;
                    Type* param_type = nullptr;
                    if (inst_fun_decl.params[i].type) {
                        param_type = resolve_type_expr(inst_fun_decl.params[i].type);
                    }
                    if (param_type && !param_type->is_error() && arg_type && !arg_type->is_error()) {
                        check_assignable(param_type, arg_type, call_expr.arguments[i].expr->loc);
                    }
                }

                Type* return_type = m_types.void_type();
                if (inst_fun_decl.return_type) {
                    return_type = resolve_type_expr(inst_fun_decl.return_type);
                }
                return return_type;
            } else {
                error_fmt(expr->loc,
                    "cannot infer type arguments for generic function '{}'; "
                    "provide explicit type arguments", func_name);
                return m_types.error_type();
            }
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
            if (base_type && base_type->is_struct()) {
                Type* result = analyze_struct_method_call(expr, call_expr, get_expr, obj_type, base_type);
                if (result) return result;
            }
        }
    }

    // Regular function call (fallback)
    return analyze_regular_fun_call(expr, call_expr);
}

bool SemanticAnalyzer::can_cast(Type* source, Type* target) {
    if (!source || !target) return false;
    if (source->is_error() || target->is_error()) return true;  // Allow error types to avoid cascading errors

    // Same type is always castable (no-op)
    if (source == target) return true;

    // Numeric to numeric: allowed
    if (source->is_numeric() && target->is_numeric()) return true;

    // Integer/float to bool: allowed
    if ((source->is_numeric()) && target->is_bool()) return true;

    // Bool to integer/float: allowed
    if (source->is_bool() && target->is_numeric()) return true;

    // String and void casts are not allowed
    if (source->kind == TypeKind::String || target->kind == TypeKind::String) return false;
    if (source->is_void() || target->is_void()) return false;

    return false;
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
    if (!can_cast(source_type, target_type)) {
        auto source_str = type_string(source_type);
        auto target_str = type_string(target_type);
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
            check_assignable(method_info->param_types[0], idx_type, index_expr.index->loc);
            coerce_int_literal(index_expr.index, method_info->param_types[0]);
        }
        return method_info->return_type;
    }

    error(index_expr.object->loc, "type has no 'index' method for subscript operator");
    return m_types.error_type();
}

Type* SemanticAnalyzer::analyze_get_expr(Expr* expr) {
    GetExpr& get_expr = expr->get;

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

    Type* target_type = analyze_expr(assign_expr.target);
    Type* value_type = analyze_expr(assign_expr.value);

    if (target_type->is_error() || value_type->is_error()) {
        return m_types.error_type();
    }

    // For compound assignment operators, check operand compatibility
    if (assign_expr.op != AssignOp::Assign) {
        // Coerce int literals to the target type before trait lookup
        if (value_type->is_int_literal() && target_type->is_integer()) {
            coerce_int_literal(assign_expr.value, target_type);
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
        check_assignable(target_type, value_type, assign_expr.value->loc);
        coerce_int_literal(assign_expr.value, target_type);
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
            check_assignable(field_type, value_type, fi.loc);
            coerce_int_literal(fi.value, field_type);

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

            // Type-check field value
            Type* value_type = analyze_expr(fi.value);
            check_assignable(variant_field_info->type, value_type, fi.loc);
            coerce_int_literal(fi.value, variant_field_info->type);
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

    // Return uniq<type> for heap allocation, value type for stack
    return sl.is_heap ? m_types.uniq_type(type) : type;
}

// ===== Type Checking Helpers =====

bool SemanticAnalyzer::is_assignable(Type* target, Type* source) const {
    if (!target || !source) return false;
    if (target->is_error() || source->is_error()) return true;
    if (target == source) return true;
    if (source->is_nil() && target->is_reference()) return true;
    if (target->is_struct() && source->is_struct()) {
        if (is_subtype_of(source, target)) return true;
    }
    if (target->is_reference() && source->is_reference()) {
        if (target->kind == source->kind) {
            Type* target_inner = target->ref_info.inner_type;
            Type* source_inner = source->ref_info.inner_type;
            if (is_subtype_of(source_inner, target_inner)) return true;
        }
    }
    if (can_convert_ref(source, target)) return true;
    if (source->is_int_literal() && target->is_integer()) return true;
    return false;
}

bool SemanticAnalyzer::check_assignable(Type* target, Type* source, SourceLocation loc) {
    if (!target || !source) return false;
    if (target->is_error() || source->is_error()) return true;  // Don't cascade errors

    // Same type is always assignable
    if (target == source) return true;

    // nil is assignable to reference types
    if (source->is_nil() && target->is_reference()) return true;

    // Struct subtyping: Child assignable to Parent (value slicing for values)
    if (target->is_struct() && source->is_struct()) {
        if (is_subtype_of(source, target)) {
            return true;
        }
    }

    // Reference subtyping: ref<Child> -> ref<Parent>, etc. (covariant)
    // Safe because struct inheritance uses prefix layout — parent fields are
    // at the same offsets in child objects, and dispatch is static.
    if (target->is_reference() && source->is_reference()) {
        if (target->kind == source->kind) {
            Type* target_inner = target->ref_info.inner_type;
            Type* source_inner = source->ref_info.inner_type;
            if (is_subtype_of(source_inner, target_inner)) {
                return true;
            }
        }
    }

    // Check reference type conversions
    if (can_convert_ref(source, target)) return true;

    // IntLiteral is assignable to any concrete integer type
    if (source->is_int_literal() && target->is_integer()) return true;

    // Strict numeric typing: no implicit conversions between numeric types
    if (target->is_numeric() && source->is_numeric() && target != source) {
        error_cannot_convert(source, target, loc, "implicitly convert");
        return false;
    }

    // Specific error messages for forbidden reference conversions
    if (source->kind == TypeKind::Ref && target->kind == TypeKind::Uniq) {
        error(loc, "cannot convert 'ref' to 'uniq': borrowing does not transfer ownership");
        return false;
    }
    if (source->kind == TypeKind::Weak && target->kind == TypeKind::Uniq) {
        error(loc, "cannot convert 'weak' to 'uniq': weak reference cannot become owner");
        return false;
    }
    if (source->kind == TypeKind::Weak && target->kind == TypeKind::Ref) {
        error(loc, "cannot convert 'weak' to 'ref': weak reference cannot become strong borrow");
        return false;
    }

    // nil can only be assigned to reference types
    if (source->is_nil() && !target->is_reference()) {
        error(loc, "'nil' can only be assigned to reference types (uniq, ref, weak)");
        return false;
    }

    auto source_str = type_string(source);
    auto target_str = type_string(target);
    error_fmt(loc, "cannot assign '{}' to '{}'", source_str.data(), target_str.data());
    return false;
}

void SemanticAnalyzer::coerce_int_literal(Expr* expr, Type* target) {
    if (!expr || !target || !target->is_integer()) return;
    if (!expr->resolved_type || !expr->resolved_type->is_int_literal()) return;
    expr->resolved_type = target;
    // Recursively concretize through transparent wrappers
    if (expr->kind == AstKind::ExprGrouping)
        coerce_int_literal(expr->grouping.expr, target);
    else if (expr->kind == AstKind::ExprUnary)
        coerce_int_literal(expr->unary.operand, target);
}

bool SemanticAnalyzer::check_numeric(Type* type, SourceLocation loc) {
    if (!type || type->is_error()) return false;
    if (!type->is_numeric()) {
        error(loc, "expected numeric type");
        return false;
    }
    return true;
}

bool SemanticAnalyzer::check_integer(Type* type, SourceLocation loc) {
    if (!type || type->is_error()) return false;
    if (!type->is_integer()) {
        error(loc, "expected integer type");
        return false;
    }
    return true;
}

bool SemanticAnalyzer::check_boolean(Type* type, SourceLocation loc) {
    if (!type || type->is_error()) return false;
    if (!type->is_bool()) {
        error(loc, "expected boolean type");
        return false;
    }
    return true;
}

String SemanticAnalyzer::type_string(Type* type) {
    String str;
    type_to_string(type, str);
    str.push_back('\0');
    return str;
}

bool SemanticAnalyzer::require_types_match(Type* left, Type* right, SourceLocation loc, const char* context) {
    if (left == right) return true;

    auto left_str = type_string(left);
    auto right_str = type_string(right);
    error_fmt(loc, "{} requires matching types, got '{}' and '{}'",
              context, left_str.data(), right_str.data());
    return false;
}

void SemanticAnalyzer::error_cannot_convert(Type* source, Type* target, SourceLocation loc, const char* context) {
    auto source_str = type_string(source);
    auto target_str = type_string(target);
    error_fmt(loc, "cannot {} '{}' to '{}'",
              context, source_str.data(), target_str.data());
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
                require_types_match(left, right, loc, "arithmetic operator");
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
            if (Type* result = try_resolve_binary_op(op, left, right))
                return result;
            // Type mismatch check for better error messages
            if (left->is_numeric() && right->is_numeric()) {
                require_types_match(left, right, loc, "comparison operator");
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
                require_types_match(left, right, loc, "bitwise operator");
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

bool SemanticAnalyzer::can_convert_ref(Type* from, Type* to) const {
    if (!from || !to) return false;

    // Helper to check inner type compatibility (same type or subtype)
    auto inner_compatible = [](Type* from_inner, Type* to_inner) -> bool {
        if (from_inner == to_inner) return true;
        // Covariant: Child -> Parent
        if (from_inner && to_inner && from_inner->is_struct() && to_inner->is_struct()) {
            return is_subtype_of(from_inner, to_inner);
        }
        return false;
    };

    // uniq -> ref conversion
    if (from->kind == TypeKind::Uniq && to->kind == TypeKind::Ref) {
        return inner_compatible(from->ref_info.inner_type, to->ref_info.inner_type);
    }

    // ref -> weak conversion
    if (from->kind == TypeKind::Ref && to->kind == TypeKind::Weak) {
        return inner_compatible(from->ref_info.inner_type, to->ref_info.inner_type);
    }

    // uniq -> weak conversion
    if (from->kind == TypeKind::Uniq && to->kind == TypeKind::Weak) {
        return inner_compatible(from->ref_info.inner_type, to->ref_info.inner_type);
    }

    return false;
}

}
