#include "roxy/compiler/semantic.hpp"
#include "roxy/compiler/module_registry.hpp"
#include "roxy/vm/binding/registry.hpp"
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

SemanticAnalyzer::SemanticAnalyzer(BumpAllocator& allocator)
    : m_allocator(allocator)
    , m_types(allocator)
    , m_symbols(allocator)
    , m_registry(nullptr)
    , m_module_registry(nullptr)
{
    // Register built-in native functions (hardcoded fallback)
    register_builtins();
}

SemanticAnalyzer::SemanticAnalyzer(BumpAllocator& allocator, NativeRegistry* registry)
    : m_allocator(allocator)
    , m_types(allocator)
    , m_symbols(allocator)
    , m_registry(registry)
    , m_module_registry(nullptr)
{
    // Register native functions from registry
    register_builtins();
}

void SemanticAnalyzer::register_builtins() {
    // If we have a module registry with a builtin module, we don't need to
    // register builtins here - they'll be imported via import_builtin_prelude()
    if (m_module_registry) {
        ModuleInfo* builtin_module = m_module_registry->find_module(
            StringView(BUILTIN_MODULE_NAME, strlen(BUILTIN_MODULE_NAME)));
        if (builtin_module) {
            // Builtins will be auto-imported as prelude
            return;
        }
    }

    // If registry is provided (but no module registry), use it for native function registration
    // Use the SemanticAnalyzer's own TypeCache so types match during type checking
    if (m_registry) {
        m_registry->apply_to_symbols(m_symbols, m_types, m_allocator);
        return;
    }

    // Otherwise fall back to hardcoded registration for backwards compatibility
    // array_new_int(size: i32) -> i32[]
    // Creates function type: (i32) -> i32[]
    Type* i32_type = m_types.i32_type();
    Type* i32_array_type = m_types.array_type(i32_type);

    Type** param_types_1 = reinterpret_cast<Type**>(
        m_allocator.alloc_bytes(sizeof(Type*), alignof(Type*)));
    param_types_1[0] = i32_type;
    Type* array_new_int_type = m_types.function_type(
        Span<Type*>(param_types_1, 1), i32_array_type);

    m_symbols.define(SymbolKind::Function, StringView("array_new_int", 13),
                     array_new_int_type, SourceLocation{0, 0}, nullptr);

    // array_len(arr: i32[]) -> i32
    // Note: This works with any array type, but for simplicity we register
    // it with i32[] as the parameter type. Semantic analysis will still work
    // because array types are compatible.
    Type** param_types_2 = reinterpret_cast<Type**>(
        m_allocator.alloc_bytes(sizeof(Type*), alignof(Type*)));
    param_types_2[0] = i32_array_type;
    Type* array_len_type = m_types.function_type(
        Span<Type*>(param_types_2, 1), i32_type);

    m_symbols.define(SymbolKind::Function, StringView("array_len", 9),
                     array_len_type, SourceLocation{0, 0}, nullptr);

    // print(value: i32) -> void
    Type** param_types_3 = reinterpret_cast<Type**>(
        m_allocator.alloc_bytes(sizeof(Type*), alignof(Type*)));
    param_types_3[0] = i32_type;
    Type* print_type = m_types.function_type(
        Span<Type*>(param_types_3, 1), m_types.void_type());

    m_symbols.define(SymbolKind::Function, StringView("print", 5),
                     print_type, SourceLocation{0, 0}, nullptr);
}

void SemanticAnalyzer::import_builtin_prelude() {
    // Auto-import all exports from the "builtin" module if available
    if (!m_module_registry) return;

    ModuleInfo* builtin_module = m_module_registry->find_module(
        StringView(BUILTIN_MODULE_NAME, strlen(BUILTIN_MODULE_NAME)));
    if (!builtin_module) return;

    // Import all exports from the builtin module into global scope
    for (const ModuleExport& exp : builtin_module->exports) {
        // Skip if already defined (shouldn't happen, but be safe)
        if (m_symbols.lookup_local(exp.name)) continue;

        // Register the imported symbol based on its kind
        if (exp.kind == ExportKind::Function) {
            m_symbols.define_imported_function(
                exp.name, exp.type, SourceLocation{0, 0},
                StringView(BUILTIN_MODULE_NAME, strlen(BUILTIN_MODULE_NAME)),
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
            Type* type = m_types.struct_type(name, decl);
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

            // Compute field slot offsets
            u32 current_slot = 0;
            for (u32 j = 0; j < fields.size(); j++) {
                FieldInfo& fi = fields[j];
                fi.slot_count = get_type_slot_count(fi.type);
                fi.slot_offset = current_slot;
                current_slot += fi.slot_count;
            }
            type->struct_info.slot_count = current_slot;

            // Allocate fields in bump allocator
            FieldInfo* field_data = reinterpret_cast<FieldInfo*>(
                m_allocator.alloc_bytes(sizeof(FieldInfo) * fields.size(), alignof(FieldInfo)));
            for (u32 j = 0; j < fields.size(); j++) {
                field_data[j] = fields[j];
            }
            type->struct_info.fields = Span<FieldInfo>(field_data, fields.size());
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
                    if (v.value->kind == AstKind::ExprLiteral &&
                        v.value->literal.literal_kind == LiteralKind::Int) {
                        value = v.value->literal.int_value;
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

    // Check if module registry is available
    if (!m_module_registry) {
        error(decl->loc, "module imports not supported (no module registry configured)");
        return;
    }

    // Look up the module
    ModuleInfo* module = m_module_registry->find_module(imp.module_path);
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
            const ModuleExport* exp = m_module_registry->find_export(module, name.name);
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
        case AstKind::ExprNew:
            result = analyze_new_expr(expr);
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
        case LiteralKind::Int:
            return m_types.i64_type();  // Integer literals default to i64
        case LiteralKind::Float:
            return m_types.f64_type();  // Float literals default to f64
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
            // TODO: Check visibility (is_pub)
            return sti.fields[i].type;
        }
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
        error(expr->loc, "'this' used outside of struct context");
        return m_types.error_type();
    }

    Type* struct_type = m_symbols.current_struct_type();
    // 'this' is a ref to the current struct
    return m_types.ref_type(struct_type);
}

Type* SemanticAnalyzer::analyze_super_expr(Expr* expr) {
    SuperExpr& se = expr->super_expr;

    if (!m_symbols.is_in_struct()) {
        error(expr->loc, "'super' used outside of struct context");
        return m_types.error_type();
    }

    Type* struct_type = m_symbols.current_struct_type();
    if (!struct_type->struct_info.parent) {
        error(expr->loc, "'super' used in struct with no parent");
        return m_types.error_type();
    }

    // Look up method in parent
    Type* parent = struct_type->struct_info.parent;
    (void)parent;  // Unused for now

    error(expr->loc, "Method calls are not yet implemented");
    return m_types.error_type();
}

Type* SemanticAnalyzer::analyze_new_expr(Expr* expr) {
    NewExpr& ne = expr->new_expr;

    Type* type = resolve_type_expr(ne.type);
    if (!type || type->is_error()) return m_types.error_type();

    // new creates a uniq reference
    Type* base = type->base_type();
    if (!base->is_struct()) {
        error(expr->loc, "'new' can only create struct instances");
        return m_types.error_type();
    }

    // TODO: Check constructor arguments

    return m_types.uniq_type(base);
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

    // Get StructDecl to access default values
    StructDecl& sd = type->struct_info.decl->struct_decl;

    // Track which fields have been initialized
    Vector<bool> field_initialized(type->struct_info.fields.size(), false);

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

        if (field_idx < 0) {
            error_fmt(fi.loc, "struct '%.*s' has no field '%.*s'",
                     sl.type_name.size(), sl.type_name.data(),
                     fi.name.size(), fi.name.data());
            continue;
        }

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
    }

    // Check all fields without defaults are initialized
    for (u32 i = 0; i < field_initialized.size(); i++) {
        if (!field_initialized[i] && sd.fields[i].default_value == nullptr) {
            error_fmt(expr->loc, "missing field '%.*s' (no default value)",
                     sd.fields[i].name.size(), sd.fields[i].name.data());
        }
    }

    return type;
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

    // Check reference type conversions
    if (can_convert_ref(source, target)) return true;

    // Numeric conversions (widening only for safety)
    if (target->is_integer() && source->is_integer()) {
        // For simplicity, allow all integer-to-integer conversions
        // A stricter implementation would only allow widening
        return true;
    }

    if (target->is_float() && source->is_float()) {
        return true;
    }

    // Integer to float is allowed
    if (target->is_float() && source->is_integer()) {
        return true;
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

Type* SemanticAnalyzer::get_binary_result_type(BinaryOp op, Type* left, Type* right, SourceLocation loc) {
    switch (op) {
        // Arithmetic operators
        case BinaryOp::Add:
        case BinaryOp::Sub:
        case BinaryOp::Mul:
        case BinaryOp::Div:
        case BinaryOp::Mod:
            if (left->is_numeric() && right->is_numeric()) {
                // Result type is the "larger" of the two
                if (left->is_float() || right->is_float()) {
                    if (left->kind == TypeKind::F64 || right->kind == TypeKind::F64) {
                        return m_types.f64_type();
                    }
                    return m_types.f32_type();
                }
                // Both integers - for simplicity, return the left type
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

    // uniq -> ref conversion
    if (from->kind == TypeKind::Uniq && to->kind == TypeKind::Ref) {
        return from->ref_info.inner_type == to->ref_info.inner_type;
    }

    // ref -> weak conversion
    if (from->kind == TypeKind::Ref && to->kind == TypeKind::Weak) {
        return from->ref_info.inner_type == to->ref_info.inner_type;
    }

    // uniq -> weak conversion
    if (from->kind == TypeKind::Uniq && to->kind == TypeKind::Weak) {
        return from->ref_info.inner_type == to->ref_info.inner_type;
    }

    return false;
}

}
