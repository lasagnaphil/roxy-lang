#include "roxy/compiler/semantic.hpp"
#include "roxy/compiler/module_registry.hpp"
#include "roxy/vm/natives.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace rx {

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
        
        // Arrays are pointers (2 slots)
        case TypeKind::Array:
            return 2;
        
        default:
            return 0;
    }
}

SemanticAnalyzer::SemanticAnalyzer(BumpAllocator& allocator, TypeCache& types, ModuleRegistry& modules)
    : m_allocator(allocator)
    , m_types(types)
    , m_modules(modules)
    , m_symbols(allocator)
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
    for (u32 i = 0; i < program->declarations.size(); i++) {
        Decl* decl = program->declarations[i];
        if (decl && decl->kind == AstKind::DeclImport) {
            analyze_import_decl(decl);
        }
    }
    if (too_many_errors()) return false;

    // Pass 1: Collect type declarations (struct/enum names)
    collect_type_declarations(program);
    if (too_many_errors()) return false;

    // Pass 2: Resolve type members (fields, methods, inheritance)
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

void SemanticAnalyzer::error_fmt(SourceLocation loc, const char* fmt, ...) {
    if (too_many_errors()) return;

    // Format the message
    char buffer[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    // Allocate a copy in the bump allocator
    u32 len = static_cast<u32>(strlen(buffer));
    char* msg = reinterpret_cast<char*>(m_allocator.alloc_bytes(len + 1, 1));
    memcpy(msg, buffer, len + 1);

    m_errors.push_back({loc, msg, true});
}

// Pass 1: Collect type declarations

void SemanticAnalyzer::collect_type_declarations(Program* program) {
    for (u32 i = 0; i < program->declarations.size(); i++) {
        Decl* decl = program->declarations[i];
        if (!decl) continue;

        if (decl->kind == AstKind::DeclStruct) {
            StringView name = decl->struct_decl.name;

            // Check for duplicate type names
            if (m_named_types.find(name) != m_named_types.end()) {
                error_fmt(decl->loc, "duplicate type declaration '%.*s'",
                         name.size(), name.data());
                continue;
            }

            // Create the struct type
            Type* type = m_types.struct_type(name, decl, m_program->module_name);
            m_named_types[name] = type;
            m_types.register_named_type(name, type);  // Register with TypeCache for IR builder access

            // Define in global scope
            m_symbols.define(SymbolKind::Struct, name, type, decl->loc, decl);
        }
        else if (decl->kind == AstKind::DeclEnum) {
            StringView name = decl->enum_decl.name;

            // Check for duplicate type names
            if (m_named_types.find(name) != m_named_types.end()) {
                error_fmt(decl->loc, "duplicate type declaration '%.*s'",
                         name.size(), name.data());
                continue;
            }

            // Create the enum type
            Type* type = m_types.enum_type(name, decl);
            m_named_types[name] = type;

            // Define in global scope
            m_symbols.define(SymbolKind::Enum, name, type, decl->loc, decl);
        }
    }
}

// Pass 2: Resolve type members

void SemanticAnalyzer::resolve_type_members(Program* program) {
    for (u32 i = 0; i < program->declarations.size(); i++) {
        Decl* decl = program->declarations[i];
        if (!decl) continue;

        if (decl->kind == AstKind::DeclStruct) {
            StructDecl& sd = decl->struct_decl;
            Type* type = m_named_types[sd.name];

            // Resolve parent type
            if (!sd.parent_name.empty()) {
                auto it = m_named_types.find(sd.parent_name);
                if (it == m_named_types.end()) {
                    error_fmt(decl->loc, "unknown parent type '%.*s'",
                             sd.parent_name.size(), sd.parent_name.data());
                } else if (it->second->kind != TypeKind::Struct) {
                    error_fmt(decl->loc, "parent type '%.*s' is not a struct",
                             sd.parent_name.size(), sd.parent_name.data());
                } else {
                    type->struct_info.parent = it->second;
                }
            }

            // Resolve field types
            Vector<FieldInfo> fields;

            // First, inherit parent fields
            if (type->struct_info.parent) {
                Type* parent = type->struct_info.parent;
                for (u32 j = 0; j < parent->struct_info.fields.size(); j++) {
                    fields.push_back(parent->struct_info.fields[j]);
                }
            }

            // Then add own fields
            for (u32 j = 0; j < sd.fields.size(); j++) {
                FieldDecl& fd = sd.fields[j];

                Type* field_type = resolve_type_expr(fd.type);
                if (!field_type) {
                    field_type = m_types.error_type();
                }

                // Check: ref types cannot be used in struct fields (prevents cycles)
                if (field_type->kind == TypeKind::Ref) {
                    error_fmt(fd.loc, "'ref' types cannot be used in struct fields");
                }

                FieldInfo info;
                info.name = fd.name;
                info.type = field_type;
                info.is_pub = fd.is_pub;
                info.index = static_cast<u32>(fields.size());
                info.slot_offset = 0;  // Will be computed below
                info.slot_count = 0;   // Will be computed below
                fields.push_back(info);
            }

            // Compute field slot offsets for regular fields
            u32 current_slot = 0;
            for (u32 j = 0; j < fields.size(); j++) {
                FieldInfo& fi = fields[j];
                fi.slot_count = get_type_slot_count(fi.type);
                fi.slot_offset = current_slot;
                current_slot += fi.slot_count;
            }

            // Process when clauses (tagged unions)
            Vector<WhenClauseInfo> when_clauses;
            for (u32 j = 0; j < sd.when_clauses.size(); j++) {
                WhenFieldDecl& wfd = sd.when_clauses[j];

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

                for (u32 k = 0; k < wfd.cases.size(); k++) {
                    WhenCaseFieldDecl& case_decl = wfd.cases[k];

                    // Validate case names are enum variants
                    i64 discriminant_value = 0;
                    for (u32 m = 0; m < case_decl.case_names.size(); m++) {
                        StringView case_name = case_decl.case_names[m];
                        Symbol* sym = m_symbols.lookup(case_name);
                        if (!sym || sym->kind != SymbolKind::EnumVariant) {
                            error_fmt(case_decl.loc, "'%.*s' is not a known enum variant",
                                     case_name.size(), case_name.data());
                        } else {
                            discriminant_value = sym->enum_variant.value;
                        }
                    }

                    // Process variant fields
                    Vector<VariantFieldInfo> var_fields;
                    u32 var_slot = 0;
                    for (u32 m = 0; m < case_decl.fields.size(); m++) {
                        FieldDecl& fd = case_decl.fields[m];
                        Type* field_type = resolve_type_expr(fd.type);
                        if (!field_type) field_type = m_types.error_type();

                        u32 slot_count = get_type_slot_count(field_type);
                        VariantFieldInfo vfi;
                        vfi.name = fd.name;
                        vfi.type = field_type;
                        vfi.slot_offset = var_slot;
                        vfi.slot_count = slot_count;
                        var_fields.push_back(vfi);
                        var_slot += slot_count;
                    }

                    // Create VariantInfo for each case name
                    for (u32 m = 0; m < case_decl.case_names.size(); m++) {
                        StringView case_name = case_decl.case_names[m];
                        Symbol* sym = m_symbols.lookup(case_name);
                        i64 value = sym ? sym->enum_variant.value : 0;

                        VariantFieldInfo* vf_data = reinterpret_cast<VariantFieldInfo*>(
                            m_allocator.alloc_bytes(sizeof(VariantFieldInfo) * var_fields.size(), alignof(VariantFieldInfo)));
                        for (u32 n = 0; n < var_fields.size(); n++) {
                            vf_data[n] = var_fields[n];
                        }

                        VariantInfo vi;
                        vi.case_name = case_name;
                        vi.discriminant_value = value;
                        vi.fields = Span<VariantFieldInfo>(vf_data, var_fields.size());
                        vi.variant_slot_count = var_slot;
                        variants.push_back(vi);
                    }

                    max_variant_slots = var_slot > max_variant_slots ? var_slot : max_variant_slots;
                }

                // Allocate variants
                VariantInfo* var_data = reinterpret_cast<VariantInfo*>(
                    m_allocator.alloc_bytes(sizeof(VariantInfo) * variants.size(), alignof(VariantInfo)));
                for (u32 k = 0; k < variants.size(); k++) {
                    var_data[k] = variants[k];
                }

                // Create WhenClauseInfo
                WhenClauseInfo clause;
                clause.discriminant_name = wfd.discriminant_name;
                clause.discriminant_type = disc_type;
                clause.discriminant_slot_offset = disc_slot_offset;
                clause.union_slot_offset = union_slot_offset;
                clause.union_slot_count = max_variant_slots;
                clause.variants = Span<VariantInfo>(var_data, variants.size());
                when_clauses.push_back(clause);

                current_slot += max_variant_slots;
            }

            type->struct_info.slot_count = current_slot;

            // Allocate fields in bump allocator
            FieldInfo* field_data = reinterpret_cast<FieldInfo*>(
                m_allocator.alloc_bytes(sizeof(FieldInfo) * fields.size(), alignof(FieldInfo)));
            for (u32 j = 0; j < fields.size(); j++) {
                field_data[j] = fields[j];
            }
            type->struct_info.fields = Span<FieldInfo>(field_data, fields.size());

            // Allocate when clauses
            WhenClauseInfo* when_data = reinterpret_cast<WhenClauseInfo*>(
                m_allocator.alloc_bytes(sizeof(WhenClauseInfo) * when_clauses.size(), alignof(WhenClauseInfo)));
            for (u32 j = 0; j < when_clauses.size(); j++) {
                when_data[j] = when_clauses[j];
            }
            type->struct_info.when_clauses = Span<WhenClauseInfo>(when_data, when_clauses.size());

            // Initialize empty constructor/destructor/method lists
            type->struct_info.constructors = Span<ConstructorInfo>(nullptr, 0);
            type->struct_info.destructors = Span<DestructorInfo>(nullptr, 0);
            type->struct_info.methods = Span<MethodInfo>(nullptr, 0);
        }
        else if (decl->kind == AstKind::DeclEnum) {
            EnumDecl& ed = decl->enum_decl;
            Type* type = m_named_types[ed.name];

            // Define enum variants in global scope (accessible as EnumName::Variant)
            i64 next_value = 0;
            for (u32 j = 0; j < ed.variants.size(); j++) {
                EnumVariant& v = ed.variants[j];

                i64 value = next_value;
                if (v.value) {
                    // Analyze the value expression
                    Type* vtype = analyze_expr(v.value);
                    if (vtype && !vtype->is_error() && !vtype->is_integer()) {
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
            // Register function in global scope (for forward references)
            FunDecl& fd = decl->fun_decl;

            // Resolve parameter types
            Vector<Type*> param_types;
            for (u32 j = 0; j < fd.params.size(); j++) {
                Type* ptype = resolve_type_expr(fd.params[j].type);
                if (!ptype) ptype = m_types.error_type();
                param_types.push_back(ptype);
            }

            // Resolve return type
            Type* return_type = fd.return_type ? resolve_type_expr(fd.return_type) : m_types.void_type();
            if (!return_type) return_type = m_types.error_type();

            // Create function type
            Type** ptypes = reinterpret_cast<Type**>(
                m_allocator.alloc_bytes(sizeof(Type*) * param_types.size(), alignof(Type*)));
            for (u32 j = 0; j < param_types.size(); j++) {
                ptypes[j] = param_types[j];
            }
            Type* func_type = m_types.function_type(
                Span<Type*>(ptypes, param_types.size()), return_type);

            // Define function in global scope
            Symbol* sym = m_symbols.define(SymbolKind::Function, fd.name, func_type, decl->loc, decl);
            sym->is_pub = fd.is_pub;
        }
        else if (decl->kind == AstKind::DeclVar) {
            // Global variable
            VarDecl& vd = decl->var_decl;

            Type* var_type = nullptr;
            if (vd.type) {
                var_type = resolve_type_expr(vd.type);
            }

            if (vd.initializer) {
                Type* init_type = analyze_expr(vd.initializer);
                if (!var_type) {
                    // Type inference
                    var_type = init_type;
                } else if (!check_assignable(var_type, init_type, decl->loc)) {
                    // Error already reported by check_assignable
                }
            } else if (!var_type) {
                error(decl->loc, "variable declaration requires type annotation or initializer");
                var_type = m_types.error_type();
            }

            vd.resolved_type = var_type;
            Symbol* sym = m_symbols.define(SymbolKind::Variable, vd.name, var_type, decl->loc, decl);
            sym->is_pub = vd.is_pub;
        }
        else if (decl->kind == AstKind::DeclConstructor) {
            analyze_constructor_decl(decl);
        }
        else if (decl->kind == AstKind::DeclDestructor) {
            analyze_destructor_decl(decl);
        }
        else if (decl->kind == AstKind::DeclMethod) {
            analyze_method_decl(decl);
        }
    }
}

// Pass 3: Analyze function bodies

void SemanticAnalyzer::analyze_function_bodies(Program* program) {
    for (u32 i = 0; i < program->declarations.size(); i++) {
        Decl* decl = program->declarations[i];
        if (!decl) continue;

        if (decl->kind == AstKind::DeclFun) {
            analyze_fun_decl(decl);
        }
        else if (decl->kind == AstKind::DeclStruct) {
            // Analyze struct methods
            StructDecl& sd = decl->struct_decl;
            Type* struct_type = m_named_types[sd.name];

            m_symbols.push_struct_scope(struct_type);

            // Define fields in struct scope
            for (u32 j = 0; j < struct_type->struct_info.fields.size(); j++) {
                FieldInfo& fi = struct_type->struct_info.fields[j];
                m_symbols.define_field(fi.name, fi.type, decl->loc, fi.index, fi.is_pub);
            }

            // Analyze methods
            for (u32 j = 0; j < sd.methods.size(); j++) {
                FunDecl* method = sd.methods[j];
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
            ConstructorDecl& cd = decl->constructor_decl;
            auto it = m_named_types.find(cd.struct_name);
            if (it != m_named_types.end()) {
                analyze_constructor_body(decl, it->second);
            }
        }
        else if (decl->kind == AstKind::DeclDestructor) {
            DestructorDecl& dd = decl->destructor_decl;
            auto it = m_named_types.find(dd.struct_name);
            if (it != m_named_types.end()) {
                analyze_destructor_body(decl, it->second);
            }
        }
        else if (decl->kind == AstKind::DeclMethod) {
            MethodDecl& md = decl->method_decl;
            auto it = m_named_types.find(md.struct_name);
            if (it != m_named_types.end()) {
                analyze_method_body(decl, it->second);
            }
        }
    }
}

// Type resolution

Type* SemanticAnalyzer::resolve_type_expr(TypeExpr* type_expr) {
    if (!type_expr) return nullptr;

    Type* base_type = nullptr;

    // Check for array type first
    if (type_expr->element_type) {
        Type* elem = resolve_type_expr(type_expr->element_type);
        if (!elem) return nullptr;
        base_type = m_types.array_type(elem);
    }
    else {
        // Look up by name
        base_type = m_types.primitive_by_name(type_expr->name);

        if (!base_type) {
            auto it = m_named_types.find(type_expr->name);
            if (it != m_named_types.end()) {
                base_type = it->second;
            }
        }

        if (!base_type) {
            error_fmt(type_expr->loc, "unknown type '%.*s'",
                     type_expr->name.size(), type_expr->name.data());
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

// Declaration analysis

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
    VarDecl& vd = decl->var_decl;

    // Check for duplicate in current scope
    if (m_symbols.lookup_local(vd.name)) {
        error_fmt(decl->loc, "redefinition of '%.*s'", vd.name.size(), vd.name.data());
        return;
    }

    Type* var_type = nullptr;
    if (vd.type) {
        var_type = resolve_type_expr(vd.type);
    }

    if (vd.initializer) {
        Type* init_type = analyze_expr(vd.initializer);
        if (!var_type) {
            // Type inference
            var_type = init_type;
            if (var_type->is_nil()) {
                error(decl->loc, "cannot infer type from nil literal");
                var_type = m_types.error_type();
            }
        } else if (!check_assignable(var_type, init_type, decl->loc)) {
            // Error already reported
        }
    } else if (!var_type) {
        error(decl->loc, "variable declaration requires type annotation or initializer");
        var_type = m_types.error_type();
    }

    vd.resolved_type = var_type;
    m_symbols.define(SymbolKind::Variable, vd.name, var_type, decl->loc, decl);
}

void SemanticAnalyzer::analyze_fun_decl(Decl* decl) {
    FunDecl& fd = decl->fun_decl;

    // Native functions don't have bodies
    if (fd.is_native) return;
    if (!fd.body) return;

    // Resolve return type
    Type* return_type = fd.return_type ? resolve_type_expr(fd.return_type) : m_types.void_type();

    // Push function scope
    m_symbols.push_function_scope(return_type);

    // Define parameters
    for (u32 i = 0; i < fd.params.size(); i++) {
        Param& p = fd.params[i];
        Type* ptype = resolve_type_expr(p.type);
        if (!ptype) ptype = m_types.error_type();

        // Check for duplicate parameter names
        if (m_symbols.lookup_local(p.name)) {
            error_fmt(p.loc, "duplicate parameter name '%.*s'", p.name.size(), p.name.data());
        } else {
            m_symbols.define_parameter(p.name, ptype, p.loc, i);
        }
    }

    // Analyze body
    analyze_stmt(fd.body);

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

void SemanticAnalyzer::analyze_import_decl(Decl* decl) {
    ImportDecl& imp = decl->import_decl;

    // Look up the module
    ModuleInfo* module = m_modules.find_module(imp.module_path);
    if (!module) {
        error_fmt(decl->loc, "unknown module '%.*s'",
                 imp.module_path.size(), imp.module_path.data());
        return;
    }

    if (imp.is_from_import) {
        // from math import sin, cos;
        for (u32 i = 0; i < imp.names.size(); i++) {
            ImportName& name = imp.names[i];

            // Find the export in the module
            const ModuleExport* exp = m_modules.find_export(module, name.name);
            if (!exp) {
                error_fmt(name.loc, "module '%.*s' has no export '%.*s'",
                         imp.module_path.size(), imp.module_path.data(),
                         name.name.size(), name.name.data());
                continue;
            }

            // Check visibility
            if (!exp->is_pub) {
                error_fmt(name.loc, "'%.*s' is not public in module '%.*s'",
                         name.name.size(), name.name.data(),
                         imp.module_path.size(), imp.module_path.data());
                continue;
            }

            // Determine local name (use alias if present)
            StringView local_name = name.alias.empty() ? name.name : name.alias;

            // Check for duplicate symbol
            if (m_symbols.lookup_local(local_name)) {
                error_fmt(name.loc, "redefinition of '%.*s'",
                         local_name.size(), local_name.data());
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
        // import math;
        // Register the module as a namespace symbol for qualified access
        m_symbols.define_module(imp.module_path, module, decl->loc);
    }
}

void SemanticAnalyzer::analyze_constructor_decl(Decl* decl) {
    ConstructorDecl& cd = decl->constructor_decl;

    // Look up the struct type
    auto it = m_named_types.find(cd.struct_name);
    if (it == m_named_types.end()) {
        error_fmt(decl->loc, "constructor for unknown struct '%.*s'",
                 cd.struct_name.size(), cd.struct_name.data());
        return;
    }

    Type* struct_type = it->second;
    if (struct_type->kind != TypeKind::Struct) {
        error_fmt(decl->loc, "'%.*s' is not a struct type",
                 cd.struct_name.size(), cd.struct_name.data());
        return;
    }

    // Check for duplicate constructor names
    for (u32 i = 0; i < struct_type->struct_info.constructors.size(); i++) {
        if (struct_type->struct_info.constructors[i].name == cd.name) {
            if (cd.name.empty()) {
                error_fmt(decl->loc, "duplicate default constructor for struct '%.*s'",
                         cd.struct_name.size(), cd.struct_name.data());
            } else {
                error_fmt(decl->loc, "duplicate constructor '%.*s' for struct '%.*s'",
                         cd.name.size(), cd.name.data(),
                         cd.struct_name.size(), cd.struct_name.data());
            }
            return;
        }
    }

    // Resolve parameter types
    Vector<Type*> param_types;
    for (u32 i = 0; i < cd.params.size(); i++) {
        Type* ptype = resolve_type_expr(cd.params[i].type);
        if (!ptype) ptype = m_types.error_type();
        param_types.push_back(ptype);
    }

    // Allocate param_types in bump allocator
    Type** ptypes = reinterpret_cast<Type**>(
        m_allocator.alloc_bytes(sizeof(Type*) * param_types.size(), alignof(Type*)));
    for (u32 i = 0; i < param_types.size(); i++) {
        ptypes[i] = param_types[i];
    }

    // Create constructor info
    ConstructorInfo ctor_info;
    ctor_info.name = cd.name;
    ctor_info.param_types = Span<Type*>(ptypes, param_types.size());
    ctor_info.decl = decl;

    // Add to struct's constructor list
    Vector<ConstructorInfo> ctors;
    for (u32 i = 0; i < struct_type->struct_info.constructors.size(); i++) {
        ctors.push_back(struct_type->struct_info.constructors[i]);
    }
    ctors.push_back(ctor_info);

    // Allocate and update
    ConstructorInfo* ctor_data = reinterpret_cast<ConstructorInfo*>(
        m_allocator.alloc_bytes(sizeof(ConstructorInfo) * ctors.size(), alignof(ConstructorInfo)));
    for (u32 i = 0; i < ctors.size(); i++) {
        ctor_data[i] = ctors[i];
    }
    struct_type->struct_info.constructors = Span<ConstructorInfo>(ctor_data, ctors.size());
}

void SemanticAnalyzer::analyze_destructor_decl(Decl* decl) {
    DestructorDecl& dd = decl->destructor_decl;

    // Look up the struct type
    auto it = m_named_types.find(dd.struct_name);
    if (it == m_named_types.end()) {
        error_fmt(decl->loc, "destructor for unknown struct '%.*s'",
                 dd.struct_name.size(), dd.struct_name.data());
        return;
    }

    Type* struct_type = it->second;
    if (struct_type->kind != TypeKind::Struct) {
        error_fmt(decl->loc, "'%.*s' is not a struct type",
                 dd.struct_name.size(), dd.struct_name.data());
        return;
    }

    // Check for duplicate destructor names
    for (u32 i = 0; i < struct_type->struct_info.destructors.size(); i++) {
        if (struct_type->struct_info.destructors[i].name == dd.name) {
            if (dd.name.empty()) {
                error_fmt(decl->loc, "duplicate default destructor for struct '%.*s'",
                         dd.struct_name.size(), dd.struct_name.data());
            } else {
                error_fmt(decl->loc, "duplicate destructor '%.*s' for struct '%.*s'",
                         dd.name.size(), dd.name.data(),
                         dd.struct_name.size(), dd.struct_name.data());
            }
            return;
        }
    }

    // Resolve parameter types
    Vector<Type*> param_types;
    for (u32 i = 0; i < dd.params.size(); i++) {
        Type* ptype = resolve_type_expr(dd.params[i].type);
        if (!ptype) ptype = m_types.error_type();
        param_types.push_back(ptype);
    }

    // Allocate param_types in bump allocator
    Type** ptypes = reinterpret_cast<Type**>(
        m_allocator.alloc_bytes(sizeof(Type*) * param_types.size(), alignof(Type*)));
    for (u32 i = 0; i < param_types.size(); i++) {
        ptypes[i] = param_types[i];
    }

    // Create destructor info
    DestructorInfo dtor_info;
    dtor_info.name = dd.name;
    dtor_info.param_types = Span<Type*>(ptypes, param_types.size());
    dtor_info.decl = decl;

    // Add to struct's destructor list
    Vector<DestructorInfo> dtors;
    for (u32 i = 0; i < struct_type->struct_info.destructors.size(); i++) {
        dtors.push_back(struct_type->struct_info.destructors[i]);
    }
    dtors.push_back(dtor_info);

    // Allocate and update
    DestructorInfo* dtor_data = reinterpret_cast<DestructorInfo*>(
        m_allocator.alloc_bytes(sizeof(DestructorInfo) * dtors.size(), alignof(DestructorInfo)));
    for (u32 i = 0; i < dtors.size(); i++) {
        dtor_data[i] = dtors[i];
    }
    struct_type->struct_info.destructors = Span<DestructorInfo>(dtor_data, dtors.size());
}

void SemanticAnalyzer::analyze_method_decl(Decl* decl) {
    MethodDecl& md = decl->method_decl;

    // Look up the struct type
    auto it = m_named_types.find(md.struct_name);
    if (it == m_named_types.end()) {
        error_fmt(decl->loc, "method for unknown struct '%.*s'",
                 md.struct_name.size(), md.struct_name.data());
        return;
    }

    Type* struct_type = it->second;
    if (struct_type->kind != TypeKind::Struct) {
        error_fmt(decl->loc, "'%.*s' is not a struct type",
                 md.struct_name.size(), md.struct_name.data());
        return;
    }

    // Check for duplicate method names
    for (u32 i = 0; i < struct_type->struct_info.methods.size(); i++) {
        if (struct_type->struct_info.methods[i].name == md.name) {
            error_fmt(decl->loc, "duplicate method '%.*s' for struct '%.*s'",
                     md.name.size(), md.name.data(),
                     md.struct_name.size(), md.struct_name.data());
            return;
        }
    }

    // Resolve parameter types
    Vector<Type*> param_types;
    for (u32 i = 0; i < md.params.size(); i++) {
        Type* ptype = resolve_type_expr(md.params[i].type);
        if (!ptype) ptype = m_types.error_type();
        param_types.push_back(ptype);
    }

    // Resolve return type
    Type* return_type = md.return_type ? resolve_type_expr(md.return_type) : m_types.void_type();
    if (!return_type) return_type = m_types.error_type();

    // Allocate param_types in bump allocator
    Type** ptypes = reinterpret_cast<Type**>(
        m_allocator.alloc_bytes(sizeof(Type*) * param_types.size(), alignof(Type*)));
    for (u32 i = 0; i < param_types.size(); i++) {
        ptypes[i] = param_types[i];
    }

    // Create method info
    MethodInfo method_info;
    method_info.name = md.name;
    method_info.param_types = Span<Type*>(ptypes, param_types.size());
    method_info.return_type = return_type;
    method_info.decl = decl;

    // Add to struct's method list
    Vector<MethodInfo> methods;
    for (u32 i = 0; i < struct_type->struct_info.methods.size(); i++) {
        methods.push_back(struct_type->struct_info.methods[i]);
    }
    methods.push_back(method_info);

    // Allocate and update
    MethodInfo* method_data = reinterpret_cast<MethodInfo*>(
        m_allocator.alloc_bytes(sizeof(MethodInfo) * methods.size(), alignof(MethodInfo)));
    for (u32 i = 0; i < methods.size(); i++) {
        method_data[i] = methods[i];
    }
    struct_type->struct_info.methods = Span<MethodInfo>(method_data, methods.size());
}

void SemanticAnalyzer::analyze_constructor_body(Decl* decl, Type* struct_type) {
    ConstructorDecl& cd = decl->constructor_decl;

    if (!cd.body) return;

    // Push struct scope so 'self' and fields are accessible
    m_symbols.push_struct_scope(struct_type);

    // Define fields in scope
    for (u32 i = 0; i < struct_type->struct_info.fields.size(); i++) {
        FieldInfo& fi = struct_type->struct_info.fields[i];
        m_symbols.define_field(fi.name, fi.type, decl->loc, fi.index, fi.is_pub);
    }

    // Push function scope (constructors return void)
    m_symbols.push_function_scope(m_types.void_type());

    // Define parameters
    for (u32 i = 0; i < cd.params.size(); i++) {
        Param& p = cd.params[i];
        Type* ptype = resolve_type_expr(p.type);
        if (!ptype) ptype = m_types.error_type();

        if (m_symbols.lookup_local(p.name)) {
            error_fmt(p.loc, "duplicate parameter name '%.*s'", p.name.size(), p.name.data());
        } else {
            m_symbols.define_parameter(p.name, ptype, p.loc, i);
        }
    }

    // Analyze body
    analyze_stmt(cd.body);

    m_symbols.pop_scope();  // function scope
    m_symbols.pop_scope();  // struct scope
}

void SemanticAnalyzer::analyze_destructor_body(Decl* decl, Type* struct_type) {
    DestructorDecl& dd = decl->destructor_decl;

    if (!dd.body) return;

    // Push struct scope so 'self' and fields are accessible
    m_symbols.push_struct_scope(struct_type);

    // Define fields in scope
    for (u32 i = 0; i < struct_type->struct_info.fields.size(); i++) {
        FieldInfo& fi = struct_type->struct_info.fields[i];
        m_symbols.define_field(fi.name, fi.type, decl->loc, fi.index, fi.is_pub);
    }

    // Push function scope (destructors return void)
    m_symbols.push_function_scope(m_types.void_type());

    // Define parameters
    for (u32 i = 0; i < dd.params.size(); i++) {
        Param& p = dd.params[i];
        Type* ptype = resolve_type_expr(p.type);
        if (!ptype) ptype = m_types.error_type();

        if (m_symbols.lookup_local(p.name)) {
            error_fmt(p.loc, "duplicate parameter name '%.*s'", p.name.size(), p.name.data());
        } else {
            m_symbols.define_parameter(p.name, ptype, p.loc, i);
        }
    }

    // Analyze body
    analyze_stmt(dd.body);

    m_symbols.pop_scope();  // function scope
    m_symbols.pop_scope();  // struct scope
}

void SemanticAnalyzer::analyze_method_body(Decl* decl, Type* struct_type) {
    MethodDecl& md = decl->method_decl;

    if (!md.body) return;

    // Push struct scope so 'self' and fields are accessible
    m_symbols.push_struct_scope(struct_type);

    // Define fields in scope
    for (u32 i = 0; i < struct_type->struct_info.fields.size(); i++) {
        FieldInfo& fi = struct_type->struct_info.fields[i];
        m_symbols.define_field(fi.name, fi.type, decl->loc, fi.index, fi.is_pub);
    }

    // Resolve return type
    Type* return_type = md.return_type ? resolve_type_expr(md.return_type) : m_types.void_type();
    if (!return_type) return_type = m_types.error_type();

    // Push function scope with return type
    m_symbols.push_function_scope(return_type);

    // Define parameters
    for (u32 i = 0; i < md.params.size(); i++) {
        Param& p = md.params[i];
        Type* ptype = resolve_type_expr(p.type);
        if (!ptype) ptype = m_types.error_type();

        if (m_symbols.lookup_local(p.name)) {
            error_fmt(p.loc, "duplicate parameter name '%.*s'", p.name.size(), p.name.data());
        } else {
            m_symbols.define_parameter(p.name, ptype, p.loc, i);
        }
    }

    // Analyze body
    analyze_stmt(md.body);

    m_symbols.pop_scope();  // function scope
    m_symbols.pop_scope();  // struct scope
}

// Statement analysis

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
    for (u32 i = 0; i < block.declarations.size(); i++) {
        Decl* decl = block.declarations[i];
        if (!decl) continue;

        if (decl->kind == AstKind::DeclVar) {
            analyze_var_decl(decl);
        } else if (decl->kind == AstKind::DeclFun) {
            // Local functions (if supported)
            error(decl->loc, "local function declarations are not supported");
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

    StructTypeInfo& sti = inner_type->struct_info;

    // Look up destructor by name
    const DestructorInfo* dtor = nullptr;
    for (u32 i = 0; i < sti.destructors.size(); i++) {
        if (sti.destructors[i].name == ds.destructor_name) {
            dtor = &sti.destructors[i];
            break;
        }
    }

    // If a destructor name was specified but not found
    if (!ds.destructor_name.empty() && !dtor) {
        error_fmt(stmt->loc, "struct '%.*s' has no destructor '%.*s'",
                 sti.name.size(), sti.name.data(),
                 ds.destructor_name.size(), ds.destructor_name.data());
        return;
    }

    // If we have a destructor, type-check the arguments
    if (dtor) {
        // Check argument count
        if (ds.arguments.size() != dtor->param_types.size()) {
            error_fmt(stmt->loc, "destructor expects %u arguments but got %u",
                     dtor->param_types.size(), ds.arguments.size());
            return;
        }

        // Get destructor's parameter info
        DestructorDecl* dtor_decl = &dtor->decl->destructor_decl;

        // Check argument types and modifiers
        for (u32 i = 0; i < ds.arguments.size(); i++) {
            CallArg& arg = ds.arguments[i];

            // Get expected modifier from DestructorDecl
            ParamModifier expected_mod = ParamModifier::None;
            if (i < dtor_decl->params.size()) {
                expected_mod = dtor_decl->params[i].modifier;
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
                check_assignable(dtor->param_types[i], arg_type, arg.expr->loc);
            }
        }
    } else {
        // No destructor defined with this name
        if (!ds.destructor_name.empty()) {
            error_fmt(stmt->loc, "struct '%.*s' has no destructor '%.*s'",
                     sti.name.size(), sti.name.data(),
                     ds.destructor_name.size(), ds.destructor_name.data());
            return;
        }

        // Named destructor arguments without a destructor is an error
        if (ds.arguments.size() > 0) {
            error_fmt(stmt->loc, "struct '%.*s' has no destructor to call",
                     sti.name.size(), sti.name.data());
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
    tsl::robin_map<StringView, bool, StringViewHash, StringViewEqual> covered_variants;

    // Analyze each case
    for (u32 i = 0; i < ws.cases.size(); i++) {
        WhenCase& wc = ws.cases[i];

        // Validate case names are valid enum variants
        for (u32 j = 0; j < wc.case_names.size(); j++) {
            StringView case_name = wc.case_names[j];

            // Look up the variant in the enum
            bool found = false;
            for (u32 k = 0; k < ed.variants.size(); k++) {
                if (ed.variants[k].name == case_name) {
                    found = true;
                    break;
                }
            }

            if (!found) {
                error_fmt(wc.loc, "'%.*s' is not a variant of enum '%.*s'",
                         case_name.size(), case_name.data(),
                         eti.name.size(), eti.name.data());
                continue;
            }

            // Check for duplicate case
            if (covered_variants.find(case_name) != covered_variants.end()) {
                error_fmt(wc.loc, "duplicate case '%.*s' in when statement",
                         case_name.size(), case_name.data());
                continue;
            }

            covered_variants[case_name] = true;
        }

        // Analyze case body in a new scope
        m_symbols.push_scope(ScopeKind::Block);
        for (u32 j = 0; j < wc.body.size(); j++) {
            Decl* decl = wc.body[j];
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
        for (u32 i = 0; i < ws.else_body.size(); i++) {
            Decl* decl = ws.else_body[i];
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

// Expression analysis

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
        default:
            result = m_types.error_type();
            break;
    }

    // Store the resolved type in the AST node for later use (e.g., IR generation)
    expr->resolved_type = result;
    return result;
}

Type* SemanticAnalyzer::analyze_literal_expr(Expr* expr) {
    LiteralExpr& lit = expr->literal;

    switch (lit.literal_kind) {
        case LiteralKind::Nil:
            return m_types.nil_type();
        case LiteralKind::Bool:
            return m_types.bool_type();
        case LiteralKind::I32:
            return m_types.i32_type();
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
        error_fmt(expr->loc, "undefined identifier '%.*s'", id.name.size(), id.name.data());
        return m_types.error_type();
    }

    return sym->type;
}

Type* SemanticAnalyzer::analyze_unary_expr(Expr* expr) {
    UnaryExpr& ue = expr->unary;

    Type* operand_type = analyze_expr(ue.operand);
    if (operand_type->is_error()) return m_types.error_type();

    return get_unary_result_type(ue.op, operand_type, expr->loc);
}

Type* SemanticAnalyzer::analyze_binary_expr(Expr* expr) {
    BinaryExpr& be = expr->binary;

    Type* left_type = analyze_expr(be.left);
    Type* right_type = analyze_expr(be.right);

    if (left_type->is_error() || right_type->is_error()) {
        return m_types.error_type();
    }

    return get_binary_result_type(be.op, left_type, right_type, expr->loc);
}

Type* SemanticAnalyzer::analyze_ternary_expr(Expr* expr) {
    TernaryExpr& te = expr->ternary;

    Type* cond_type = analyze_expr(te.condition);
    if (!cond_type->is_error()) {
        check_boolean(cond_type, te.condition->loc);
    }

    Type* then_type = analyze_expr(te.then_expr);
    Type* else_type = analyze_expr(te.else_expr);

    if (then_type->is_error()) return else_type;
    if (else_type->is_error()) return then_type;

    // Types must be compatible
    if (then_type == else_type) {
        return then_type;
    }

    // Check if one can be converted to the other
    if (check_assignable(then_type, else_type, te.else_expr->loc)) {
        return then_type;
    }
    if (check_assignable(else_type, then_type, te.then_expr->loc)) {
        return else_type;
    }

    error(expr->loc, "incompatible types in ternary expression");
    return m_types.error_type();
}

Type* SemanticAnalyzer::analyze_call_expr(Expr* expr) {
    CallExpr& ce = expr->call;

    // Check if this is a primitive type cast: i32(x), f64(x), bool(x)
    if (ce.callee->kind == AstKind::ExprIdentifier) {
        StringView type_name = ce.callee->identifier.name;
        Type* target_type = m_types.primitive_by_name(type_name);
        if (target_type != nullptr && !target_type->is_void() && !target_type->is_error()) {
            return analyze_primitive_cast(expr, target_type);
        }
    }

    // Check if this is a constructor call: callee is an identifier that resolves to a struct type
    if (ce.callee->kind == AstKind::ExprIdentifier) {
        StringView type_name = ce.callee->identifier.name;

        // Look up as a type first
        auto it = m_named_types.find(type_name);
        if (it != m_named_types.end() && it->second->is_struct()) {
            // This is a constructor call: Type()
            Type* struct_type = it->second;
            // Store the struct type in callee for IR builder
            ce.callee->resolved_type = struct_type;
            return analyze_constructor_call(expr, struct_type, ce.constructor_name, ce.is_heap);
        }
    }

    // Check if this is a named constructor call: Type.ctor_name(...)
    // The callee is a GetExpr where the object is a struct type identifier
    if (ce.callee->kind == AstKind::ExprGet) {
        GetExpr& ge = ce.callee->get;
        if (ge.object->kind == AstKind::ExprIdentifier) {
            StringView type_name = ge.object->identifier.name;

            // Check if it's a struct type
            auto it = m_named_types.find(type_name);
            if (it != m_named_types.end() && it->second->is_struct()) {
                // This is a named constructor call: Type.ctor_name(...)
                Type* struct_type = it->second;
                // Store the struct type in callee's object for IR builder
                ge.object->resolved_type = struct_type;
                // Set constructor_name in CallExpr for later use
                ce.constructor_name = ge.name;
                return analyze_constructor_call(expr, struct_type, ge.name, ce.is_heap);
            }
        }
    }

    // Check if this is a super call: super() / super(args) / super.ctor_name() / super.method_name()
    if (ce.callee->kind == AstKind::ExprSuper) {
        SuperExpr& se = ce.callee->super_expr;

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

        StructTypeInfo& parent_sti = parent_type->struct_info;

        // If method_name is empty, this is super() or super(args) - constructor call
        if (se.method_name.empty()) {
            // Look for default constructor
            const ConstructorInfo* ctor = nullptr;
            for (u32 i = 0; i < parent_sti.constructors.size(); i++) {
                if (parent_sti.constructors[i].name.empty()) {
                    ctor = &parent_sti.constructors[i];
                    break;
                }
            }

            // If no default constructor but there are args, error
            if (!ctor && ce.arguments.size() > 0) {
                error_fmt(expr->loc, "parent struct '%.*s' has no constructor that takes arguments",
                         static_cast<int>(parent_sti.name.size()), parent_sti.name.data());
                return m_types.void_type();
            }

            // Type-check arguments if constructor found
            if (ctor) {
                if (ce.arguments.size() != ctor->param_types.size()) {
                    error_fmt(expr->loc, "parent constructor expects %u arguments but got %u",
                             ctor->param_types.size(), ce.arguments.size());
                    return m_types.void_type();
                }

                for (u32 i = 0; i < ce.arguments.size(); i++) {
                    CallArg& arg = ce.arguments[i];
                    Type* arg_type = analyze_expr(arg.expr);
                    if (!check_assignable(ctor->param_types[i], arg_type, arg.expr->loc)) {
                        // Error already reported
                    }
                }
            }

            // Store parent type in callee for IR builder
            ce.callee->resolved_type = m_types.ref_type(parent_type);
            return m_types.void_type();
        }

        // method_name is not empty - this is super.name()
        // First try to find it as a constructor, then as a method
        const ConstructorInfo* ctor = nullptr;
        for (u32 i = 0; i < parent_sti.constructors.size(); i++) {
            if (parent_sti.constructors[i].name == se.method_name) {
                ctor = &parent_sti.constructors[i];
                break;
            }
        }

        if (ctor) {
            // It's a named constructor call
            if (ce.arguments.size() != ctor->param_types.size()) {
                error_fmt(expr->loc, "parent constructor expects %u arguments but got %u",
                         ctor->param_types.size(), ce.arguments.size());
                return m_types.void_type();
            }

            for (u32 i = 0; i < ce.arguments.size(); i++) {
                CallArg& arg = ce.arguments[i];
                Type* arg_type = analyze_expr(arg.expr);
                if (!check_assignable(ctor->param_types[i], arg_type, arg.expr->loc)) {
                    // Error already reported
                }
            }

            ce.callee->resolved_type = m_types.ref_type(parent_type);
            return m_types.void_type();
        }

        // Not a constructor - try method
        Type* found_in_type = nullptr;
        const MethodInfo* mi = lookup_method_in_hierarchy(parent_type, se.method_name, &found_in_type);

        if (mi) {
            // It's a super method call
            // Type-check arguments
            if (ce.arguments.size() != mi->param_types.size()) {
                error_fmt(expr->loc, "method '%.*s' expects %u arguments but got %u",
                         static_cast<int>(se.method_name.size()), se.method_name.data(),
                         mi->param_types.size(), ce.arguments.size());
                return mi->return_type;
            }

            for (u32 i = 0; i < ce.arguments.size(); i++) {
                CallArg& arg = ce.arguments[i];
                Type* arg_type = analyze_expr(arg.expr);
                if (!check_assignable(mi->param_types[i], arg_type, arg.expr->loc)) {
                    // Error already reported
                }
            }

            // Store found_in_type for IR builder to mangle correctly
            ce.callee->resolved_type = m_types.ref_type(found_in_type);
            return mi->return_type;
        }

        // Neither constructor nor method
        error_fmt(expr->loc, "parent struct '%.*s' has no constructor or method '%.*s'",
                 static_cast<int>(parent_sti.name.size()), parent_sti.name.data(),
                 static_cast<int>(se.method_name.size()), se.method_name.data());
        return m_types.error_type();
    }

    // Check if this is a method call: obj.method(args)
    // We need to detect this BEFORE calling analyze_expr(ce.callee) because
    // that would return a function type with 'self' as the first parameter.
    if (ce.callee->kind == AstKind::ExprGet) {
        GetExpr& ge = ce.callee->get;

        // First, analyze the object to get its type
        Type* obj_type = analyze_expr(ge.object);
        if (obj_type && !obj_type->is_error()) {
            Type* base_type = obj_type->base_type();
            if (base_type && base_type->is_struct()) {
                // Look for a method with this name (walks inheritance hierarchy)
                Type* found_in_type = nullptr;
                const MethodInfo* mi = lookup_method_in_hierarchy(base_type, ge.name, &found_in_type);
                if (mi) {
                    // This is a method call!

                    // Check argument count (NOT including implicit self)
                    if (ce.arguments.size() != mi->param_types.size()) {
                        error_fmt(expr->loc, "method expects %u arguments but got %u",
                                 mi->param_types.size(), ce.arguments.size());
                        return mi->return_type;
                    }

                    // Get MethodDecl for parameter modifiers
                    MethodDecl* method_decl = mi->decl ? &mi->decl->method_decl : nullptr;

                    // Check argument types and modifiers
                    for (u32 j = 0; j < ce.arguments.size(); j++) {
                        CallArg& arg = ce.arguments[j];

                        // Get expected modifier from MethodDecl
                        ParamModifier expected_mod = ParamModifier::None;
                        if (method_decl && j < method_decl->params.size()) {
                            expected_mod = method_decl->params[j].modifier;
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
                            if (!check_assignable(mi->param_types[j], arg_type, arg.expr->loc)) {
                                // Error already reported
                            }
                        }
                    }

                    // Set callee's resolved_type to a function type for IR builder
                    // The function type includes 'self' as the first parameter
                    // Use found_in_type for proper method resolution (inheritance)
                    u32 total_params = 1 + mi->param_types.size();
                    Type** ptypes = reinterpret_cast<Type**>(
                        m_allocator.alloc_bytes(sizeof(Type*) * total_params, alignof(Type*)));
                    ptypes[0] = m_types.ref_type(found_in_type);  // Use the type where method was found
                    for (u32 j = 0; j < mi->param_types.size(); j++) {
                        ptypes[j + 1] = mi->param_types[j];
                    }
                    ce.callee->resolved_type = m_types.function_type(
                        Span<Type*>(ptypes, total_params), mi->return_type);

                    // Store the struct type where method was found in the GetExpr
                    // IR builder will use this for correct method name mangling
                    ge.object->resolved_type = obj_type;  // Keep original object type

                    return mi->return_type;
                }
            }
        }
    }

    // Regular function call
    Type* callee_type = analyze_expr(ce.callee);
    if (callee_type->is_error()) return m_types.error_type();

    if (!callee_type->is_function()) {
        error(ce.callee->loc, "expression is not callable");
        return m_types.error_type();
    }

    FunctionTypeInfo& fti = callee_type->func_info;

    // Check argument count
    if (ce.arguments.size() != fti.param_types.size()) {
        error_fmt(expr->loc, "expected %u arguments but got %u",
                 fti.param_types.size(), ce.arguments.size());
        return fti.return_type;
    }

    // Try to get the FunDecl to access parameter modifiers
    FunDecl* fun_decl = nullptr;
    if (ce.callee->kind == AstKind::ExprIdentifier) {
        Symbol* sym = m_symbols.lookup(ce.callee->identifier.name);
        if (sym && sym->kind == SymbolKind::Function && sym->decl) {
            fun_decl = &sym->decl->fun_decl;
        }
    }

    // Check argument types and modifiers
    for (u32 i = 0; i < ce.arguments.size(); i++) {
        CallArg& arg = ce.arguments[i];

        // Get expected modifier from FunDecl
        ParamModifier expected_mod = ParamModifier::None;
        if (fun_decl && i < fun_decl->params.size()) {
            expected_mod = fun_decl->params[i].modifier;
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

        // Type check (skip for 'out' since it's write-only - callee doesn't read the value)
        if (arg.modifier != ParamModifier::Out) {
            if (!check_assignable(fti.param_types[i], arg_type, arg.expr->loc)) {
                // Error already reported
            }
        }
    }

    return fti.return_type;
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
    CallExpr& ce = expr->call;

    // Must have exactly one argument
    if (ce.arguments.size() != 1) {
        error_fmt(expr->loc, "type cast requires exactly 1 argument, got %u", ce.arguments.size());
        return m_types.error_type();
    }

    // Check for modifiers (out/inout not allowed)
    if (ce.arguments[0].modifier != ParamModifier::None) {
        error(expr->loc, "type cast argument cannot have 'out' or 'inout' modifier");
        return m_types.error_type();
    }

    // Analyze the source expression
    Type* source_type = analyze_expr(ce.arguments[0].expr);
    if (!source_type || source_type->is_error()) {
        return m_types.error_type();
    }

    // Check if the cast is valid
    if (!can_cast(source_type, target_type)) {
        Vector<char> source_str, target_str;
        type_to_string(source_type, source_str);
        type_to_string(target_type, target_str);
        source_str.push_back('\0');
        target_str.push_back('\0');
        error_fmt(expr->loc, "cannot cast '%s' to '%s'", source_str.data(), target_str.data());
        return m_types.error_type();
    }

    // Store target type in callee for IR builder to detect this is a cast
    ce.callee->resolved_type = target_type;

    return target_type;
}

Type* SemanticAnalyzer::analyze_constructor_call(Expr* expr, Type* struct_type, StringView ctor_name, bool is_heap) {
    CallExpr& ce = expr->call;
    StructTypeInfo& sti = struct_type->struct_info;

    // Look up constructor by name
    const ConstructorInfo* ctor = nullptr;
    for (u32 i = 0; i < sti.constructors.size(); i++) {
        if (sti.constructors[i].name == ctor_name) {
            ctor = &sti.constructors[i];
            break;
        }
    }

    // If a constructor name was specified but not found
    if (!ctor_name.empty() && !ctor) {
        error_fmt(expr->loc, "struct '%.*s' has no constructor '%.*s'",
                 sti.name.size(), sti.name.data(),
                 ctor_name.size(), ctor_name.data());
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
        if (ce.arguments.size() != ctor->param_types.size()) {
            error_fmt(expr->loc, "constructor expects %u arguments but got %u",
                     ctor->param_types.size(), ce.arguments.size());
            return result_type();
        }

        // Get constructor's parameter info
        ConstructorDecl* ctor_decl = &ctor->decl->constructor_decl;

        // Check argument types and modifiers
        for (u32 i = 0; i < ce.arguments.size(); i++) {
            CallArg& arg = ce.arguments[i];

            // Get expected modifier from ConstructorDecl
            ParamModifier expected_mod = ParamModifier::None;
            if (i < ctor_decl->params.size()) {
                expected_mod = ctor_decl->params[i].modifier;
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
                check_assignable(ctor->param_types[i], arg_type, arg.expr->loc);
            }
        }
    } else {
        // No constructor defined - either using default construction or error
        if (!ctor_name.empty()) {
            // Named constructor was requested but struct has no constructors
            error_fmt(expr->loc, "struct '%.*s' has no constructor '%.*s'",
                     sti.name.size(), sti.name.data(),
                     ctor_name.size(), ctor_name.data());
            return m_types.error_type();
        }

        // Default construction (no constructor, no arguments) - allowed
        // Arguments without a constructor is an error
        if (ce.arguments.size() > 0) {
            error_fmt(expr->loc, "struct '%.*s' has no constructor to call",
                     sti.name.size(), sti.name.data());
            return m_types.error_type();
        }
    }

    return result_type();
}

Type* SemanticAnalyzer::analyze_index_expr(Expr* expr) {
    IndexExpr& ie = expr->index;

    Type* obj_type = analyze_expr(ie.object);
    Type* idx_type = analyze_expr(ie.index);

    if (obj_type->is_error()) return m_types.error_type();

    // Unwrap reference types
    Type* base_type = obj_type->base_type();

    if (!base_type->is_array()) {
        error(ie.object->loc, "cannot index non-array type");
        return m_types.error_type();
    }

    if (!idx_type->is_error()) {
        check_integer(idx_type, ie.index->loc);
    }

    return base_type->array_info.element_type;
}

Type* SemanticAnalyzer::analyze_get_expr(Expr* expr) {
    GetExpr& ge = expr->get;

    // Check for module-qualified access: module.member
    if (ge.object->kind == AstKind::ExprIdentifier) {
        StringView name = ge.object->identifier.name;
        Symbol* sym = m_symbols.lookup(name);

        if (sym && sym->kind == SymbolKind::Module) {
            // This is module-qualified access
            ModuleInfo* module = static_cast<ModuleInfo*>(sym->module.module_info);
            const ModuleExport* exp = module->find_export(ge.name);

            if (!exp) {
                error_fmt(expr->loc, "module '%.*s' has no export '%.*s'",
                         name.size(), name.data(),
                         ge.name.size(), ge.name.data());
                return m_types.error_type();
            }

            // Check visibility
            if (!exp->is_pub) {
                error_fmt(expr->loc, "'%.*s' is not public in module '%.*s'",
                         ge.name.size(), ge.name.data(),
                         name.size(), name.data());
                return m_types.error_type();
            }

            // Mark the expression with the module info for later use by IR builder
            // We set resolved_type on the object to indicate it's a module reference
            ge.object->resolved_type = nullptr;  // Modules don't have a type

            return exp->type;
        }
    }

    Type* obj_type = analyze_expr(ge.object);
    if (obj_type->is_error()) return m_types.error_type();

    // Unwrap reference types
    Type* base_type = obj_type->base_type();

    if (!base_type->is_struct()) {
        error(ge.object->loc, "cannot access member of non-struct type");
        return m_types.error_type();
    }

    // Look up field
    StructTypeInfo& sti = base_type->struct_info;
    for (u32 i = 0; i < sti.fields.size(); i++) {
        if (sti.fields[i].name == ge.name) {
            // Check visibility: non-public fields can only be accessed from the same module
            // If either module name is empty, we're in single-file mode where all access is allowed
            bool same_module = sti.module_name.empty() || m_program->module_name.empty() ||
                               sti.module_name == m_program->module_name;
            if (!sti.fields[i].is_pub && !same_module) {
                error_fmt(expr->loc, "field '%.*s' is private in struct '%.*s'",
                         ge.name.size(), ge.name.data(),
                         sti.name.size(), sti.name.data());
                return m_types.error_type();
            }
            return sti.fields[i].type;
        }
    }

    // Look up variant field in when clauses (tagged union)
    const WhenClauseInfo* found_clause = nullptr;
    const VariantInfo* found_variant = nullptr;
    const VariantFieldInfo* vfi = sti.find_variant_field(ge.name, &found_clause, &found_variant);
    if (vfi) {
        // Variant field access - semantic analysis allows it, runtime will check discriminant
        return vfi->type;
    }

    // Look up method (walks inheritance hierarchy)
    Type* found_in_type = nullptr;
    const MethodInfo* mi = lookup_method_in_hierarchy(base_type, ge.name, &found_in_type);
    if (mi) {
        // Build function type with implicit self parameter
        // Method type: (ref<StructType>, param_types...) -> return_type

        // Build parameter types: ref<StructType> + method params
        // Use found_in_type for proper method resolution (inheritance)
        u32 total_params = 1 + mi->param_types.size();
        Type** ptypes = reinterpret_cast<Type**>(
            m_allocator.alloc_bytes(sizeof(Type*) * total_params, alignof(Type*)));
        ptypes[0] = m_types.ref_type(found_in_type);  // self parameter - type where method is defined
        for (u32 j = 0; j < mi->param_types.size(); j++) {
            ptypes[j + 1] = mi->param_types[j];
        }

        return m_types.function_type(Span<Type*>(ptypes, total_params), mi->return_type);
    }

    error_fmt(expr->loc, "struct '%.*s' has no member '%.*s'",
             sti.name.size(), sti.name.data(),
             ge.name.size(), ge.name.data());
    return m_types.error_type();
}

Type* SemanticAnalyzer::analyze_static_get_expr(Expr* expr) {
    StaticGetExpr& sge = expr->static_get;

    // Look up the type
    auto it = m_named_types.find(sge.type_name);
    if (it == m_named_types.end()) {
        error_fmt(expr->loc, "unknown type '%.*s'",
                 sge.type_name.size(), sge.type_name.data());
        return m_types.error_type();
    }

    Type* type = it->second;

    if (type->is_enum()) {
        // Look up enum variant
        Symbol* sym = m_symbols.lookup(sge.member_name);
        if (sym && sym->kind == SymbolKind::EnumVariant && sym->type == type) {
            return type;
        }
        error_fmt(expr->loc, "enum '%.*s' has no variant '%.*s'",
                 sge.type_name.size(), sge.type_name.data(),
                 sge.member_name.size(), sge.member_name.data());
        return m_types.error_type();
    }

    error(expr->loc, "static member access is only supported for enums");
    return m_types.error_type();
}

Type* SemanticAnalyzer::analyze_assign_expr(Expr* expr) {
    AssignExpr& ae = expr->assign;

    // Check lvalue
    if (!is_lvalue(ae.target)) {
        error(ae.target->loc, "cannot assign to non-lvalue expression");
        return m_types.error_type();
    }

    Type* target_type = analyze_expr(ae.target);
    Type* value_type = analyze_expr(ae.value);

    if (target_type->is_error() || value_type->is_error()) {
        return m_types.error_type();
    }

    // For compound assignment operators, check operand compatibility
    if (ae.op != AssignOp::Assign) {
        BinaryOp binop;
        switch (ae.op) {
            case AssignOp::AddAssign: binop = BinaryOp::Add; break;
            case AssignOp::SubAssign: binop = BinaryOp::Sub; break;
            case AssignOp::MulAssign: binop = BinaryOp::Mul; break;
            case AssignOp::DivAssign: binop = BinaryOp::Div; break;
            case AssignOp::ModAssign: binop = BinaryOp::Mod; break;
            default: binop = BinaryOp::Add; break;
        }
        get_binary_result_type(binop, target_type, value_type, expr->loc);
    } else {
        check_assignable(target_type, value_type, ae.value->loc);
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
    SuperExpr& se = expr->super_expr;

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
    const MethodInfo* mi = lookup_method_in_hierarchy(parent_type, se.method_name, &found_in_type);

    if (!mi) {
        error_fmt(expr->loc, "parent struct has no method '%.*s'",
                 static_cast<int>(se.method_name.size()), se.method_name.data());
        return m_types.error_type();
    }

    // Build function type with ref<ParentType> as self
    // The function type: (ref<ParentType>, param_types...) -> return_type
    u32 total_params = 1 + mi->param_types.size();
    Type** ptypes = reinterpret_cast<Type**>(
        m_allocator.alloc_bytes(sizeof(Type*) * total_params, alignof(Type*)));
    ptypes[0] = m_types.ref_type(found_in_type);  // self parameter - parent type
    for (u32 j = 0; j < mi->param_types.size(); j++) {
        ptypes[j + 1] = mi->param_types[j];
    }

    return m_types.function_type(Span<Type*>(ptypes, total_params), mi->return_type);
}

Type* SemanticAnalyzer::analyze_struct_literal_expr(Expr* expr) {
    StructLiteralExpr& sl = expr->struct_literal;

    // Look up struct type
    auto it = m_named_types.find(sl.type_name);
    if (it == m_named_types.end()) {
        error_fmt(expr->loc, "unknown struct type '%.*s'",
                 sl.type_name.size(), sl.type_name.data());
        return m_types.error_type();
    }

    Type* type = it->second;
    if (!type->is_struct()) {
        error_fmt(expr->loc, "'%.*s' is not a struct type",
                 sl.type_name.size(), sl.type_name.data());
        return m_types.error_type();
    }

    // Helper to find field default value by searching inheritance chain
    auto find_field_default = [](Type* struct_type, StringView field_name) -> Expr* {
        Type* current = struct_type;
        while (current && current->is_struct()) {
            StructDecl& sd = current->struct_info.decl->struct_decl;
            for (u32 j = 0; j < sd.fields.size(); j++) {
                if (sd.fields[j].name == field_name) {
                    return sd.fields[j].default_value;
                }
            }
            current = current->struct_info.parent;
        }
        return nullptr;
    };

    // Track which fields have been initialized
    Vector<bool> field_initialized(type->struct_info.fields.size(), false);

    // Track initialized variant fields by name
    tsl::robin_map<StringView, bool, StringViewHash, StringViewEqual> variant_field_initialized;
    i64 discriminant_value = -1;  // Track which variant is selected

    // Validate each field initializer
    for (u32 i = 0; i < sl.fields.size(); i++) {
        FieldInit& fi = sl.fields[i];

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
                error_fmt(fi.loc, "duplicate field '%.*s'",
                         fi.name.size(), fi.name.data());
                continue;
            }

            field_initialized[field_idx] = true;

            // Type-check field value
            Type* value_type = analyze_expr(fi.value);
            Type* field_type = type->struct_info.fields[field_idx].type;
            check_assignable(field_type, value_type, fi.loc);

            // Track discriminant value if this is a discriminant field
            for (u32 j = 0; j < type->struct_info.when_clauses.size(); j++) {
                const WhenClauseInfo& clause = type->struct_info.when_clauses[j];
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
        const VariantFieldInfo* vfi = type->struct_info.find_variant_field(fi.name, &found_clause, &found_variant);

        if (vfi) {
            // Variant field
            if (variant_field_initialized.find(fi.name) != variant_field_initialized.end()) {
                error_fmt(fi.loc, "duplicate field '%.*s'",
                         fi.name.size(), fi.name.data());
                continue;
            }
            variant_field_initialized[fi.name] = true;

            // Type-check field value
            Type* value_type = analyze_expr(fi.value);
            check_assignable(vfi->type, value_type, fi.loc);
            continue;
        }

        error_fmt(fi.loc, "struct '%.*s' has no field '%.*s'",
                 sl.type_name.size(), sl.type_name.data(),
                 fi.name.size(), fi.name.data());
    }

    // Check all fields without defaults are initialized
    for (u32 i = 0; i < field_initialized.size(); i++) {
        if (!field_initialized[i]) {
            FieldInfo& fi = type->struct_info.fields[i];
            Expr* default_val = find_field_default(type, fi.name);
            if (default_val == nullptr) {
                error_fmt(expr->loc, "missing field '%.*s' (no default value)",
                         fi.name.size(), fi.name.data());
            }
        }
    }

    // Return uniq<type> for heap allocation, value type for stack
    return sl.is_heap ? m_types.uniq_type(type) : type;
}

// Type checking helpers

bool SemanticAnalyzer::check_assignable(Type* target, Type* source, SourceLocation loc) {
    if (!target || !source) return false;
    if (target->is_error() || source->is_error()) return true;  // Don't cascade errors

    // Same type is always assignable
    if (target == source) return true;

    // nil is assignable to reference types
    if (source->is_nil() && target->is_reference()) return true;
    if (source->is_nil() && target->kind == TypeKind::Weak) return true;

    // Struct subtyping: Child assignable to Parent (value slicing for values)
    if (target->is_struct() && source->is_struct()) {
        if (is_subtype_of(source, target)) {
            return true;
        }
    }

    // Reference subtyping: uniq<Child> -> uniq<Parent>, ref<Child> -> ref<Parent>, etc. (covariant)
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

    Vector<char> target_str, source_str;
    type_to_string(target, target_str);
    type_to_string(source, source_str);
    target_str.push_back('\0');
    source_str.push_back('\0');

    error_fmt(loc, "cannot assign '%s' to '%s'", source_str.data(), target_str.data());
    return false;
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

bool SemanticAnalyzer::require_types_match(Type* left, Type* right, SourceLocation loc, const char* context) {
    if (left == right) return true;

    Vector<char> left_str, right_str;
    type_to_string(left, left_str);
    type_to_string(right, right_str);
    left_str.push_back('\0');
    right_str.push_back('\0');
    error_fmt(loc, "%s requires matching types, got '%s' and '%s'",
              context, left_str.data(), right_str.data());
    return false;
}

void SemanticAnalyzer::error_cannot_convert(Type* source, Type* target, SourceLocation loc, const char* context) {
    Vector<char> source_str, target_str;
    type_to_string(source, source_str);
    type_to_string(target, target_str);
    source_str.push_back('\0');
    target_str.push_back('\0');
    error_fmt(loc, "cannot %s '%s' to '%s'",
              context, source_str.data(), target_str.data());
}

Type* SemanticAnalyzer::get_binary_result_type(BinaryOp op, Type* left, Type* right, SourceLocation loc) {
    switch (op) {
        // Arithmetic operators
        case BinaryOp::Add:
        case BinaryOp::Sub:
        case BinaryOp::Mul:
        case BinaryOp::Div:
        case BinaryOp::Mod:
            if (left->is_numeric() && right->is_numeric()) {
                if (!require_types_match(left, right, loc, "arithmetic operator")) {
                    return m_types.error_type();
                }
                return left;
            }
            // String concatenation for Add
            if (op == BinaryOp::Add && left->kind == TypeKind::String && right->kind == TypeKind::String) {
                return m_types.string_type();
            }
            error(loc, "invalid operands for arithmetic operator");
            return m_types.error_type();

        // Comparison operators
        case BinaryOp::Equal:
        case BinaryOp::NotEqual:
        case BinaryOp::Less:
        case BinaryOp::LessEq:
        case BinaryOp::Greater:
        case BinaryOp::GreaterEq:
            if (left->is_numeric() && right->is_numeric()) {
                if (!require_types_match(left, right, loc, "comparison operator")) {
                    return m_types.error_type();
                }
                return m_types.bool_type();
            }
            if (left == right) {
                return m_types.bool_type();  // Same type comparison
            }
            error(loc, "invalid operands for comparison operator");
            return m_types.error_type();

        // Logical operators
        case BinaryOp::And:
        case BinaryOp::Or:
            if (left->is_bool() && right->is_bool()) {
                return m_types.bool_type();
            }
            error(loc, "logical operators require boolean operands");
            return m_types.error_type();

        // Bitwise operators
        case BinaryOp::BitAnd:
        case BinaryOp::BitOr:
            if (left->is_integer() && right->is_integer()) {
                if (!require_types_match(left, right, loc, "bitwise operator")) {
                    return m_types.error_type();
                }
                return left;
            }
            error(loc, "bitwise operators require integer operands");
            return m_types.error_type();
    }

    return m_types.error_type();
}

Type* SemanticAnalyzer::get_unary_result_type(UnaryOp op, Type* operand, SourceLocation loc) {
    switch (op) {
        case UnaryOp::Negate:
            if (operand->is_numeric()) {
                return operand;
            }
            error(loc, "unary '-' requires numeric operand");
            return m_types.error_type();

        case UnaryOp::Not:
            if (operand->is_bool()) {
                return m_types.bool_type();
            }
            error(loc, "unary '!' requires boolean operand");
            return m_types.error_type();

        case UnaryOp::BitNot:
            if (operand->is_integer()) {
                return operand;
            }
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
