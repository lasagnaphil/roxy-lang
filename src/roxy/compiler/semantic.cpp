#include "roxy/compiler/semantic.hpp"
#include "roxy/compiler/operator_traits.hpp"
#include "roxy/compiler/module_registry.hpp"
#include "roxy/core/scoped_value.hpp"
#include "roxy/vm/natives.hpp"
#include "roxy/vm/binding/registry.hpp"

#include <cstring>

namespace rx {

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

// Recursively determine whether a statement (sub)tree contains a `yield`.
// A Coro<T>-returning function is a real coroutine (state machine) only if its
// body yields; otherwise it is an ordinary function producing/forwarding a
// first-class coroutine value. Deliberately does NOT descend into nested
// lambda/function bodies — a yield there belongs to that body (and is rejected
// separately), never to the enclosing function.
static bool stmt_contains_yield(Stmt* stmt);

static bool decl_contains_yield(Decl* decl) {
    if (!decl) return false;
    // A "declaration" that is actually a wrapped statement (block bodies hold
    // Span<Decl*>). Real declarations (nested var/fun/struct) never contribute
    // a yield to this function.
    if (decl->kind >= AstKind::StmtExpr && decl->kind <= AstKind::StmtYield) {
        return stmt_contains_yield(&decl->stmt);
    }
    return false;
}

static bool stmt_contains_yield(Stmt* stmt) {
    if (!stmt) return false;
    switch (stmt->kind) {
        case AstKind::StmtYield:
            return true;
        case AstKind::StmtBlock:
            for (Decl* d : stmt->block.declarations) {
                if (decl_contains_yield(d)) return true;
            }
            return false;
        case AstKind::StmtIf:
            return stmt_contains_yield(stmt->if_stmt.then_branch)
                || stmt_contains_yield(stmt->if_stmt.else_branch);
        case AstKind::StmtWhile:
            return stmt_contains_yield(stmt->while_stmt.body);
        case AstKind::StmtFor:
            return decl_contains_yield(stmt->for_stmt.initializer)
                || stmt_contains_yield(stmt->for_stmt.body);
        case AstKind::StmtWhen: {
            for (const WhenCase& c : stmt->when_stmt.cases) {
                for (Decl* d : c.body) {
                    if (decl_contains_yield(d)) return true;
                }
            }
            for (Decl* d : stmt->when_stmt.else_body) {
                if (decl_contains_yield(d)) return true;
            }
            return false;
        }
        case AstKind::StmtTry: {
            if (stmt_contains_yield(stmt->try_stmt.try_body)) return true;
            for (const CatchClause& cc : stmt->try_stmt.catches) {
                if (stmt_contains_yield(cc.body)) return true;
            }
            return stmt_contains_yield(stmt->try_stmt.finally_body);
        }
        default:
            return false;
    }
}

// A `while (true)` / `while (false)` literal condition. Only the literal is
// recognized — constant-foldable expressions (e.g. `1 == 1`) are not, so a loop
// with such a condition is treated as possibly-exiting.
static bool is_const_true_condition(Expr* cond) {
    return cond && cond->kind == AstKind::ExprLiteral
        && cond->literal.literal_kind == LiteralKind::Bool
        && cond->literal.bool_value;
}

static bool stmt_has_direct_break(Stmt* stmt);

static bool decl_has_direct_break(Decl* decl) {
    if (!decl) return false;
    if (decl->kind >= AstKind::StmtExpr && decl->kind <= AstKind::StmtYield) {
        return stmt_has_direct_break(&decl->stmt);
    }
    return false;
}

// True if `stmt` contains a `break` that targets the enclosing loop — i.e. one
// not nested inside a deeper loop (whose `break` binds to that inner loop). Used
// to distinguish an infinite `while (true) { ... }` (control never falls past
// it) from one that can exit via `break`.
static bool stmt_has_direct_break(Stmt* stmt) {
    if (!stmt) return false;
    switch (stmt->kind) {
        case AstKind::StmtBreak:
            return true;
        // Nested loops capture their own `break` — do not descend.
        case AstKind::StmtWhile:
        case AstKind::StmtFor:
            return false;
        case AstKind::StmtBlock:
            for (Decl* d : stmt->block.declarations) {
                if (decl_has_direct_break(d)) return true;
            }
            return false;
        case AstKind::StmtIf:
            return stmt_has_direct_break(stmt->if_stmt.then_branch)
                || stmt_has_direct_break(stmt->if_stmt.else_branch);
        case AstKind::StmtWhen: {
            for (const WhenCase& c : stmt->when_stmt.cases) {
                for (Decl* d : c.body) {
                    if (decl_has_direct_break(d)) return true;
                }
            }
            for (Decl* d : stmt->when_stmt.else_body) {
                if (decl_has_direct_break(d)) return true;
            }
            return false;
        }
        case AstKind::StmtTry: {
            if (stmt_has_direct_break(stmt->try_stmt.try_body)) return true;
            for (const CatchClause& cc : stmt->try_stmt.catches) {
                if (stmt_has_direct_break(cc.body)) return true;
            }
            return stmt_has_direct_break(stmt->try_stmt.finally_body);
        }
        default:
            return false;
    }
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
    , m_context{m_allocator, m_type_env, m_types, m_modules, m_symbols, m_reporter, m_checker,
                this, &SemanticAnalyzer::resolve_type_expr_thunk,
                &SemanticAnalyzer::analyze_expr_thunk, &SemanticAnalyzer::analyze_stmt_thunk}
    , m_lifetimes(m_context)
    , m_traits(m_context, m_synthetic_decls)
    , m_generic_calls(m_context, m_lifetimes, m_function_context, m_synthetic_decls)
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
    , m_context{m_allocator, m_type_env, m_types, m_modules, m_symbols, m_reporter, m_checker,
                this, &SemanticAnalyzer::resolve_type_expr_thunk,
                &SemanticAnalyzer::analyze_expr_thunk, &SemanticAnalyzer::analyze_stmt_thunk}
    , m_lifetimes(m_context)
    , m_traits(m_context, m_synthetic_decls)
    , m_generic_calls(m_context, m_lifetimes, m_function_context, m_synthetic_decls)
    , m_program(nullptr)
{
}

void SemanticAnalyzer::set_lsp_mode(bool enable) { m_reporter.set_lsp_mode(enable); }
bool SemanticAnalyzer::lsp_mode() const { return m_reporter.lsp_mode(); }

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

    // Pass 1.7: Register builtin traits (Printable, Hash, Eq, Exception,
    // Index/IndexMut) — guarded against re-initialization
    m_traits.register_builtin_traits();

    // Pass 1.8: Register built-in operator trait methods for primitive types
    m_traits.register_primitive_operator_methods();

    // Pass 1.9: Resolve trait bounds on generic type parameters
    m_generic_calls.resolve_generic_bounds();

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

bool SemanticAnalyzer::drain_pending_fun_instance(GenericFunInstance* inst) {
    // Abstract Phase-B artifacts (e.g. identity$$T) exist only so a bounded
    // template body's call type-checks; their body names the bare type param
    // and can't be analyzed outside the bounds context — drop them (the IR
    // builder skips them too), mirroring the abstract-struct-instance handling.
    if (inst->is_abstract) return false;

    StringView this_module = m_program ? m_program->module_name : StringView{};
    bool owns_template = inst->template_module.empty()
                      || this_module.empty()
                      || inst->template_module == this_module;
    if (owns_template) {
        analyze_fun_body(inst->instantiated_decl);
        inst->is_analyzed = true;
        return true;
    }
    m_type_env.generics().sideline_cross_module_fun(inst);
    return false;
}

u32 SemanticAnalyzer::analyze_owned_pending_fun_instances() {
    // Drain pending generic-fun instances whose template was registered by
    // this analyzer's module. Cross-module-owned instances are sidelined
    // for the next compiler-level pass; new instances triggered by analysis
    // (which land back in m_pending_funs) get drained on the next iteration
    // of the compiler's outer loop.
    if (!m_type_env.generics().has_pending_funs()) return 0;
    auto pending = m_type_env.generics().take_pending_funs();
    u32 drained = 0;
    for (auto* inst : pending) {
        if (drain_pending_fun_instance(inst)) drained++;
    }
    return drained;
}

void SemanticAnalyzer::analyze_single_function(Decl* decl) {
    if (!decl) return;

    // Import builtin prelude so symbols are available
    import_builtin_prelude();

    // Dispatch to the BODY analyzer for the declaration's kind — this entry
    // point re-analyzes one body against the already-populated TypeEnv; the
    // signatures were registered by the declaration passes. (It previously
    // dispatched members to the signature-registration methods — then named
    // analyze_*_decl — which re-reported "duplicate method/constructor" into
    // the LSP diagnostics and never analyzed the body.) Member bodies need
    // their struct type; if the name doesn't resolve to a struct (broken or
    // stale declaration state in LSP mode), there is no receiver context to
    // analyze against — skip quietly.
    switch (decl->kind) {
        case AstKind::DeclFun:
            analyze_fun_body(decl);
            break;
        case AstKind::DeclMethod: {
            Type* struct_type = m_type_env.named_type_by_name(decl->method_decl.struct_name);
            if (struct_type && struct_type->is_struct()) {
                analyze_method_body(decl, struct_type);
            }
            break;
        }
        case AstKind::DeclConstructor: {
            Type* struct_type = m_type_env.named_type_by_name(decl->constructor_decl.struct_name);
            if (struct_type && struct_type->is_struct()) {
                analyze_constructor_body(decl, struct_type);
            }
            break;
        }
        case AstKind::DeclDestructor: {
            Type* struct_type = m_type_env.named_type_by_name(decl->destructor_decl.struct_name);
            if (struct_type && struct_type->is_struct()) {
                analyze_destructor_body(decl, struct_type);
            }
            break;
        }
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
            m_traits.collect_trait_declaration(decl);
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
            case AstKind::DeclFun:         register_fun_signature(decl); break;
            case AstKind::DeclVar:         resolve_global_var(decl); break;
            case AstKind::DeclConstructor: resolve_constructor_member(decl); break;
            case AstKind::DeclDestructor:  resolve_destructor_member(decl); break;
            case AstKind::DeclMethod:      resolve_method_member(decl); break;
            case AstKind::DeclTrait:       m_traits.resolve_trait_parent(decl); break;
            default: break;
        }
    }

    generate_synthetic_destructors(program);

    // Now validate all trait implementations
    m_traits.validate_trait_implementations();
}

bool SemanticAnalyzer::ensure_struct_members_resolved(Type* struct_type, SourceLocation loc) {
    if (!struct_type || struct_type->kind != TypeKind::Struct) return true;
    StructTypeInfo& info = struct_type->struct_info;
    if (info.members_resolved) return true;

    // A struct currently being resolved higher up this recursion is a genuine
    // value-type cycle: its layout depends (directly or transitively) on ours
    // and ours on its — infinite size. This subsumes the old post-hoc
    // detect_mutual_struct_recursion pass AND the direct self-reference check
    // (the struct itself sits in m_resolving_structs while its own fields
    // resolve).
    if (m_resolving_structs.count(struct_type)) {
        error_fmt(loc,
            "recursive struct type '{}' has infinite size; "
            "use 'uniq {}' for indirection",
            info.name, info.name);
        return false;
    }

    // No AST decl (registry-built native structs, synthesized lambda envs):
    // the layout is owned elsewhere; nothing to resolve here.
    if (!info.decl) return true;

    resolve_struct_members(info.decl);
    return true;
}

void SemanticAnalyzer::resolve_struct_members(Decl* decl) {
    StructDecl& struct_decl = decl->struct_decl;

    // Skip generic struct templates - they have unresolved type params
    if (struct_decl.type_params.size() > 0) return;

    Type* type = m_type_env.named_type_by_name(struct_decl.name);
    if (!type || !type->is_struct()) return;

    // Memoized: a dependent struct's field may have pulled this in already,
    // ahead of its source position (declaration order doesn't matter).
    if (type->struct_info.members_resolved) return;
    m_resolving_structs.insert(type);
    bool has_cycle = false;

    // Resolve parent type
    if (!struct_decl.parent_name.empty()) {
        Type* parent = m_type_env.named_type_by_name(struct_decl.parent_name);
        if (!parent) {
            error_fmt(decl->loc, "unknown parent type '{}'", struct_decl.parent_name);
        } else if (parent->kind != TypeKind::Struct) {
            error_fmt(decl->loc, "parent type '{}' is not a struct", struct_decl.parent_name);
        } else {
            // The parent's fields are embedded below, so its members must be
            // resolved first (it may be declared later in the file).
            if (!ensure_struct_members_resolved(parent, decl->loc)) has_cycle = true;
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

        // A value-embedded struct field contributes its full layout, so the
        // field's struct must be resolved first (recurses; reports the
        // infinite-size error on genuine cycles). Reference-wrapped fields
        // (uniq/ref/weak) are pointer-sized and impose no layout dependency —
        // that's exactly what makes recursive types via `uniq` legal.
        if (field_type->kind == TypeKind::Struct) {
            if (!ensure_struct_members_resolved(field_type, field.loc)) has_cycle = true;
        }

        // A `ref` field is a counted borrow: the struct is move-only (noncopyable)
        // and ref_incs the field on construction / ref_decs on drop, so a borrow
        // stored in a struct keeps the owner alive (or traps) exactly like a
        // `List<ref T>` element (docs/internals/lifetimes.md "Value lifecycle"). The
        // synthetic-destructor pass (driven by member_needs_drop) makes such a
        // struct move-only and gives it field-walk cleanup.
        FieldInfo info;
        info.name = field.name;
        info.type = field_type;
        info.is_pub = field.is_pub;
        info.index = static_cast<u32>(fields.size());
        info.slot_offset = 0;  // Will be computed below
        info.slot_count = 0;   // Will be computed below
        fields.push_back(info);
    }

    if (has_cycle) {
        // Error(s) already reported at the offending field(s). Mark resolved
        // so other reference sites don't re-resolve and duplicate the errors;
        // compilation fails regardless.
        m_resolving_structs.erase(type);
        type->struct_info.members_resolved = true;
        return;
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

    // Constructor/destructor/method spans are deliberately NOT reset here:
    // they start empty (zeroed by the Type factory), and member declarations
    // appearing before the struct decl in source order may already have
    // appended to them.

    m_resolving_structs.erase(type);
    type->struct_info.members_resolved = true;
}

namespace {
// Fold a compile-time constant integer expression (for enum variant values).
// Handles integer literals, grouping, unary -/~, and binary arithmetic/bitwise
// ops over constants. Writes the result to `out` and returns true on success;
// returns false (out untouched) for anything non-constant, a division by zero,
// or an out-of-range shift.
bool eval_const_int(Expr* e, i64& out) {
    if (!e) return false;
    switch (e->kind) {
        case AstKind::ExprLiteral: {
            LiteralExpr& lit = e->literal;
            switch (lit.literal_kind) {
                case LiteralKind::I32: case LiteralKind::I64:
                case LiteralKind::U32: case LiteralKind::U64:
                    out = lit.int_value;
                    return true;
                default:
                    return false;
            }
        }
        case AstKind::ExprGrouping:
            return eval_const_int(e->grouping.expr, out);
        case AstKind::ExprUnary: {
            i64 v;
            if (!eval_const_int(e->unary.operand, v)) return false;
            switch (e->unary.op) {
                case UnaryOp::Negate: out = -v; return true;
                case UnaryOp::BitNot: out = ~v; return true;
                default: return false;
            }
        }
        case AstKind::ExprBinary: {
            i64 l, r;
            if (!eval_const_int(e->binary.left, l) || !eval_const_int(e->binary.right, r)) {
                return false;
            }
            switch (e->binary.op) {
                case BinaryOp::Add: out = l + r; return true;
                case BinaryOp::Sub: out = l - r; return true;
                case BinaryOp::Mul: out = l * r; return true;
                case BinaryOp::Div: if (r == 0) return false; out = l / r; return true;
                case BinaryOp::Mod: if (r == 0) return false; out = l % r; return true;
                case BinaryOp::BitAnd: out = l & r; return true;
                case BinaryOp::BitOr:  out = l | r; return true;
                case BinaryOp::BitXor: out = l ^ r; return true;
                case BinaryOp::Shl: if (r < 0 || r >= 64) return false; out = l << r; return true;
                case BinaryOp::Shr: if (r < 0 || r >= 64) return false; out = l >> r; return true;
                default: return false;  // comparisons / logical ops aren't integer-valued
            }
        }
        default:
            return false;
    }
}
}

void SemanticAnalyzer::resolve_enum_members(Decl* decl) {
    EnumDecl& ed = decl->enum_decl;
    Type* type = m_type_env.named_type_by_name(ed.name);

    Vector<EnumVariantInfo> variants;
    i64 next_value = 0;
    for (auto& v : ed.variants) {

        i64 value = next_value;
        if (v.value) {
            // Analyze the value expression, then require it to fold to a
            // compile-time integer constant. Previously only bare integer
            // literals were honored — `A = 1 + 2` or `A = -1` silently fell
            // through to auto-increment (a wrong value, no diagnostic).
            Type* vtype = analyze_expr(v.value);
            if (vtype && !vtype->is_error()) {
                if (!vtype->is_integer() && !vtype->is_int_literal()) {
                    error_fmt(v.loc, "enum variant value must be an integer");
                } else if (!eval_const_int(v.value, value)) {
                    error_fmt(v.loc, "enum variant value must be a compile-time integer constant");
                }
            }
        }

        // The variant table on the enum type is the authoritative resolution
        // source (Enum::Variant, when-statement cases, tagged-union when
        // clauses) — collision-proof across enums.
        variants.push_back(EnumVariantInfo{v.name, value});

        // Also define the bare name in scope for unqualified references.
        // NOTE: this namespace is flat — same-named variants of different
        // enums shadow each other here, which is inherent to bare names.
        m_symbols.define_enum_variant(v.name, type, v.loc, value);

        next_value = value + 1;
    }

    type->enum_info.variants = m_allocator.alloc_span(variants);
}

void SemanticAnalyzer::register_fun_signature(Decl* decl) {
    // Skip generic function templates - they have unresolved type params
    FunDecl& fun_decl = decl->fun_decl;
    if (fun_decl.type_params.size() > 0) return;

    // Register function in global scope (for forward references)
    Span<Type*> param_types = resolve_param_types(fun_decl.params);

    // Resolve return type
    Type* return_type = fun_decl.return_type ? resolve_type_expr(fun_decl.return_type) : m_types.void_type();

    // A Coro<T>-returning function is a real coroutine only if its body yields.
    // A non-yielding one merely produces/forwards a first-class coroutine value
    // and stays an ordinary function (its return type is the interned generic
    // Coro<T>, already carrying resume/done methods from resolve_type_expr).
    // Real coroutines get a function-specific coroutine type (carrying func_name
    // for the state-struct wiring in coroutine lowering).
    if (return_type && return_type->is_coroutine()) {
        fun_decl.is_coroutine = stmt_contains_yield(fun_decl.body);
        if (fun_decl.is_coroutine) {
            return_type = m_types.coroutine_type_for_func(
                return_type->coro_info.yield_type, fun_decl.name);
            populate_coro_methods(return_type);
        }
    }

    // Create function type
    Type* func_type = m_types.function_type(param_types, return_type);

    // Define function in global scope
    Symbol* sym = m_symbols.define(SymbolKind::Function, fun_decl.name, func_type, decl->loc, decl);
    sym->is_pub = fun_decl.is_pub;
}

void SemanticAnalyzer::resolve_global_var(Decl* decl) {
    VarDecl& var_decl = decl->var_decl;

    // Same duplicate/inference/move-tracking rules as a local var (see
    // analyze_var_decl); module globals are analyzed once, here in the
    // declaration pass, so their initializers' move states track between
    // globals (a later global moving an earlier uniq global is caught).
    if (m_symbols.lookup_local(var_decl.name)) {
        error_fmt(decl->loc, "redefinition of '{}'", var_decl.name);
        return;
    }

    Type* var_type = var_decl.type ? resolve_type_expr(var_decl.type) : nullptr;
    var_type = analyze_var_initializer(var_decl, decl, var_type);

    var_decl.resolved_type = var_type;
    Symbol* sym = m_symbols.define(SymbolKind::Variable, var_decl.name, var_type, decl->loc, decl);
    sym->is_pub = var_decl.is_pub;

    if (var_type && var_type->noncopyable()) {
        m_lifetimes.track_live(sym);
    }
}

void SemanticAnalyzer::resolve_constructor_member(Decl* decl) {
    ConstructorDecl& constructor_decl = decl->constructor_decl;
    if (generics().is_generic_struct(constructor_decl.struct_name)) {
        generics().register_generic_struct_constructor(constructor_decl.struct_name, decl);
        return;
    }
    register_constructor_signature(decl);
}

void SemanticAnalyzer::resolve_destructor_member(Decl* decl) {
    DestructorDecl& destructor_decl = decl->destructor_decl;
    if (generics().is_generic_struct(destructor_decl.struct_name)) {
        generics().register_generic_struct_destructor(destructor_decl.struct_name, decl);
        return;
    }
    register_destructor_signature(decl);
}

void SemanticAnalyzer::resolve_method_member(Decl* decl) {
    MethodDecl& method_decl = decl->method_decl;

    // Coroutine trait methods are not supported in v1 (scope: non-generic,
    // non-trait instance methods). Reject them BEFORE the trait pipeline — the
    // trait-impl validator doesn't handle a Coro<T> return and misbehaves.
    // (`Coro` is a reserved builtin type name, so a cheap name check suffices;
    // a yielding body is what makes it a coroutine vs. a forwarding method.)
    bool is_coro_method = method_decl.return_type
        && method_decl.return_type->name == "Coro"_sv
        && stmt_contains_yield(method_decl.body);

    // Check if struct_name is actually a trait name
    Type* trait_lookup = m_type_env.trait_type_by_name(method_decl.struct_name);
    if (trait_lookup && method_decl.trait_name.empty()) {
        // This is a trait method declaration (fun TraitName.method(...))
        if (is_coro_method) {
            error(decl->loc, "coroutine methods are not yet supported on generic structs or in traits");
            return;
        }
        m_traits.register_trait_method_signature(decl, trait_lookup);
    }
    else if (!method_decl.trait_name.empty()) {
        // This is a trait implementation (fun Type.method(...) for Trait<Args>)
        if (is_coro_method) {
            error(decl->loc, "coroutine methods are not yet supported on generic structs or in traits");
            return;
        }
        m_traits.resolve_trait_impl_member(decl);
    }
    else {
        // Regular method (no trait involvement)
        // Check if struct_name is a generic struct template
        if (generics().is_generic_struct(method_decl.struct_name)) {
            generics().register_generic_struct_method(method_decl.struct_name, decl);
            return;  // Skip normal method analysis; handled in worklist
        }
        register_method_signature(decl);
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
            if (!struct_needs_synthetic_dtor(struct_info)) continue;

            add_synthetic_default_dtor(m_allocator, struct_info);
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
        if (!disc_type->is_error() && !disc_type->is_enum()) {
            error_fmt(wfd.loc, "'when' discriminant must be an enum type");
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
            // Validate case names against the DISCRIMINANT enum's own variant
            // table — collision-proof, and rejects other enums' variants. If
            // the discriminant type already failed to resolve as an enum, the
            // error was reported above; skip per-case noise.
            i64 discriminant_value = 0;
            if (disc_type->is_enum()) {
                for (const auto& case_name : case_decl.case_names) {
                    const EnumVariantInfo* variant = disc_type->enum_info.find_variant(case_name);
                    if (!variant) {
                        error_fmt(case_decl.loc, "'{}' is not a variant of enum '{}'",
                                  case_name, disc_type->enum_info.name);
                    } else {
                        discriminant_value = variant->value;
                    }
                }
            }

            // Process variant fields
            Vector<VariantFieldInfo> var_fields;
            u32 var_slot = 0;
            for (auto& field : case_decl.fields) {
                Type* field_type = resolve_type_expr(field.type);

                // Value-embedded struct variant fields contribute layout, so
                // the field's struct must be resolved first (same rule as
                // regular fields; cycles report the infinite-size error).
                if (field_type->kind == TypeKind::Struct) {
                    ensure_struct_members_resolved(field_type, field.loc);
                }

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

            // Create VariantInfo for each case name. Invalid cases were
            // already reported above and compilation will fail; give them a
            // placeholder value of 0.
            for (const auto& case_name : case_decl.case_names) {
                const EnumVariantInfo* variant = disc_type->is_enum()
                    ? disc_type->enum_info.find_variant(case_name)
                    : nullptr;
                i64 value = variant ? variant->value : 0;

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
                m_generic_calls.analyze_generic_template_body(decl);
                continue;
            }
            analyze_fun_body(decl);
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

                analyze_fun_body(method_decl);
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
                // Abstract Phase-B artifacts (e.g. Holder$$T) exist only so
                // the template-body walk has field/method types to check;
                // their member bodies reference bare type params that don't
                // resolve outside the Phase B bounds context, and they are
                // never codegen'd (the IR builder skips them too).
                if (inst->is_abstract) continue;

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
                if (struct_needs_synthetic_dtor(concrete_info)) {
                    add_synthetic_default_dtor(m_allocator, concrete_info);
                }
            }
        }

        // Process pending function instances. Only handle instances whose
        // template was defined in THIS module — cross-module instances are
        // sidelined for the compiler's post-pass to drain in the right
        // module's context (re-queueing them here would infinite-loop the
        // outer worklist).
        if (m_type_env.generics().has_pending_funs()) {
            auto pending_funs = m_type_env.generics().take_pending_funs();
            for (auto* inst : pending_funs) {
                drain_pending_fun_instance(inst);
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
    bool has_cycle = false;
    for (u32 fi = 0; fi < struct_decl.fields.size(); fi++) {
        Type* field_type = resolve_type_expr(struct_decl.fields[fi].type);

        // Same rule as resolve_struct_members: a value-embedded struct field
        // must have its members resolved first (recurses for declaration-order
        // independence; genuine cycles — including cycles through this very
        // instance — report the infinite-size error).
        if (field_type->kind == TypeKind::Struct) {
            if (!ensure_struct_members_resolved(field_type, struct_decl.fields[fi].loc)) {
                has_cycle = true;
            }
        }

        FieldInfo info;
        info.name = struct_decl.fields[fi].name;
        info.type = field_type;
        info.is_pub = struct_decl.fields[fi].is_pub;
        info.index = fi;
        info.slot_offset = 0;
        info.slot_count = 0;
        fields.push_back(info);
    }

    m_resolving_structs.erase(inst->concrete_type);

    if (has_cycle) {
        inst->is_analyzed = true;
        inst->concrete_type->struct_info.members_resolved = true;
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
        ConstructorInfo ctor_info;
        ctor_info.name = ctor.name;
        ctor_info.param_types = resolve_param_types(ctor.params);
        ctor_info.decl = ctor_decl;
        append_constructor(m_allocator, struct_type_info, ctor_info);
    }

    // Register DestructorInfo for external destructors
    for (Decl* dtor_decl : inst->instantiated_destructors) {
        DestructorDecl& dtor = dtor_decl->destructor_decl;
        DestructorInfo dtor_info;
        dtor_info.name = dtor.name;
        dtor_info.param_types = resolve_param_types(dtor.params);
        dtor_info.decl = dtor_decl;
        append_destructor(m_allocator, struct_type_info, dtor_info);
    }

    // Register MethodInfo for external methods so call sites can resolve them
    for (Decl* method_decl : inst->instantiated_methods) {
        MethodDecl& method = method_decl->method_decl;
        MethodInfo method_info;
        method_info.name = method.name;
        method_info.param_types = resolve_param_types(method.params);
        method_info.return_type = method.return_type
            ? resolve_type_expr(method.return_type) : m_types.void_type();
        method_info.decl = method_decl;
        append_method(m_allocator, struct_type_info, method_info);
    }

    // Generate synthetic default destructor if struct has fields needing cleanup
    if (struct_needs_synthetic_dtor(struct_type_info)) {
        add_synthetic_default_dtor(m_allocator, struct_type_info);
    }

    inst->is_analyzed = true;
    struct_type_info.members_resolved = true;
}

Type* SemanticAnalyzer::resolve_type_expr(TypeExpr* type_expr) {
    // Never returns null: a null TypeExpr (possible in LSP-recovered ASTs)
    // and every resolution failure yield error_type. Callers that treat "no
    // annotation" as a signal (e.g. var-decl inference) must branch on the
    // TypeExpr itself, not on the result.
    if (!type_expr) return m_types.error_type();

    // Function type: fun(T1, T2) -> R
    if (type_expr->kind == TypeExprKind::Function) {
        Vector<Type*> params;
        for (auto* param_expr : type_expr->type_args) {
            Type* pt = resolve_type_expr(param_expr);
            if (pt->is_error()) return m_types.error_type();
            params.push_back(pt);
        }
        Type* ret = type_expr->return_type
            ? resolve_type_expr(type_expr->return_type)
            : m_types.void_type();
        if (ret->is_error()) return m_types.error_type();
        Type* base_type = m_types.function_type(m_allocator.alloc_span(params), ret);

        switch (type_expr->ref_kind) {
            case RefKind::Uniq: base_type = m_types.uniq_type(base_type); break;
            case RefKind::Ref:  base_type = m_types.ref_type(base_type); break;
            case RefKind::Weak: base_type = m_types.weak_type(base_type); break;
            case RefKind::None: break;
        }
        if (type_expr->is_borrowed) {
            base_type = m_types.borrowed(base_type);
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
        if (!base_type) {
            base_type = m_generic_calls.resolve_active_type_param(type_expr->name);
        }

        // Check for built-in List<T> type
        if (!base_type && type_expr->name == "List") {
            if (type_expr->type_args.size() != 1) {
                error(type_expr->loc, "List requires exactly 1 type argument");
                return m_types.error_type();
            }
            Type* elem = resolve_type_expr(type_expr->type_args[0]);
            if (elem->is_error()) return m_types.error_type();
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
            if (yield_type->is_error()) return m_types.error_type();
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
            if (key_type->is_error()) return m_types.error_type();
            if (val_type->is_error()) return m_types.error_type();
            if (!is_hashable_key_type(key_type)) {
                error(type_expr->loc, "Map key type must be a primitive, enum, or struct");
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
                if (arg_type->is_error()) return m_types.error_type();
                type_arg_types.push_back(arg_type);
            }

            // Validate arg count
            Decl* template_decl = m_type_env.generics().get_generic_struct_decl(type_expr->name);
            if (template_decl->struct_decl.type_params.size() != type_arg_types.size()) {
                error_fmt(type_expr->loc, "generic struct '{}' expects {} type argument(s) but got {}",
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

    // `borrowed`: demote the resolved type to a borrow (uniq T -> ref T,
    // fun -> ref fun, everything else unchanged).
    if (type_expr->is_borrowed) {
        base_type = m_types.borrowed(base_type);
    }

    return base_type;
}

// ===== Declaration Analysis =====

Type* SemanticAnalyzer::analyze_var_initializer(VarDecl& var_decl, Decl* decl, Type* var_type) {
    if (var_decl.initializer) {
        Type* init_type = analyze_expr(var_decl.initializer);
        // Resolve generic-template-ref initializers against the declared type
        // before assignability checking; updates init_type via resolved_type.
        if (var_type && m_generic_calls.coerce_generic_template_ref(var_decl.initializer, var_type)) {
            init_type = var_decl.initializer->resolved_type;
        }
        if (!var_type) {
            // Type inference
            var_type = init_type;
            if (var_type->is_nil()) {
                error(decl->loc, "cannot infer type from nil literal");
                var_type = m_types.error_type();
            } else if (var_type->is_numeric_literal()) {
                // Nothing to infer from: settle the literal on its default
                // (i32 for an integer literal, f64 for a float one).
                var_type = default_literal_type(var_type);
                m_checker.coerce_numeric_literal(var_decl.initializer, var_type);
            }
        } else if (!m_checker.check_assignable(var_type, init_type, decl->loc)) {
            // Error already reported
        } else {
            // Coerce int literals to the annotated type
            m_checker.coerce_numeric_literal(var_decl.initializer, var_type);
        }

        // Consume noncopyable source (field-move check + mark source as moved)
        if (var_type && var_type->noncopyable()) {
            m_lifetimes.consume_noncopyable(var_decl.initializer, decl->loc);
        }
    } else if (!var_type) {
        error(decl->loc, "variable declaration requires type annotation or initializer");
        var_type = m_types.error_type();
    }
    return var_type;
}

bool SemanticAnalyzer::check_no_local_shadowing(StringView name, SourceLocation loc) {
    Symbol* shadowed = m_symbols.lookup_function_local(name);
    if (!shadowed) return true;
    const char* what = shadowed->kind == SymbolKind::Parameter ? "parameter" : "variable";
    error_fmt(loc,
              "declaration of '{}' shadows the {} declared at line {}; "
              "shadowing is not allowed within a function",
              name, what, shadowed->loc.line);
    return false;
}

void SemanticAnalyzer::analyze_var_decl(Decl* decl) {
    VarDecl& var_decl = decl->var_decl;

    // Check for duplicate in current scope
    if (m_symbols.lookup_local(var_decl.name)) {
        error_fmt(decl->loc, "redefinition of '{}'", var_decl.name);
        return;
    }
    if (!check_no_local_shadowing(var_decl.name, decl->loc)) {
        return;
    }

    Type* var_type = var_decl.type ? resolve_type_expr(var_decl.type) : nullptr;
    var_type = analyze_var_initializer(var_decl, decl, var_type);

    var_decl.resolved_type = var_type;
    Symbol* var_sym = m_symbols.define(SymbolKind::Variable, var_decl.name, var_type, decl->loc, decl);

    // Track owned variables for move semantics (uniq refs and value structs with destructors)
    if (var_type && var_type->noncopyable()) {
        m_lifetimes.track_live(var_sym);
    }
}

void SemanticAnalyzer::analyze_fun_body(Decl* decl) {
    FunDecl& fun_decl = decl->fun_decl;

    // Native functions don't have bodies
    if (fun_decl.is_native) return;
    if (!fun_decl.body) return;

    // Single-shot rule: analysis mutates the AST it walks (see the annotation
    // contract in ast.hpp) — re-lower or clone a fresh tree instead of
    // re-analyzing this one.
    assert(!decl->body_analyzed && "AST body re-analysis (single-shot rule; see ast.hpp)");
    decl->body_analyzed = true;

    // Resolve return type
    Type* return_type = fun_decl.return_type ? resolve_type_expr(fun_decl.return_type) : m_types.void_type();

    // Fresh per-function context (coroutine/destructor/finally slots) and
    // lifetime state for this body, restored as one unit on exit. `is_coroutine`
    // (classified by yield-presence in register_fun_signature) gates `yield`
    // legality and the no-value-return rule — a non-yielding Coro<T>-returning
    // function is ordinary and may `return <coro value>`.
    bool is_coroutine = fun_decl.is_coroutine;
    FunctionContextScope context_scope(m_function_context, m_lifetimes);
    if (is_coroutine) {
        m_function_context.in_coroutine = true;
        m_function_context.coro_yield_type = return_type->coro_info.yield_type;
    }

    // Push function scope
    m_symbols.push_function_scope(return_type);

    // Define parameters
    for (u32 i = 0; i < fun_decl.params.size(); i++) {
        Param& p = fun_decl.params[i];
        Type* ptype = resolve_type_expr(p.type);
        p.resolved_type = ptype;

        // Check for duplicate parameter names
        if (m_symbols.lookup_local(p.name)) {
            error_fmt(p.loc, "duplicate parameter name '{}'", p.name);
        } else {
            m_symbols.define_parameter(p.name, ptype, p.loc, i,
                                       p.modifier != ParamModifier::None);
        }

        // Track owned parameters as Live (uniq refs and value structs with destructors)
        if (ptype && ptype->noncopyable()) {
            m_lifetimes.track_live(m_symbols.lookup(p.name));
        }
    }

    // Analyze body
    analyze_stmt(fun_decl.body);

    // All-paths-return: a non-void function must terminate (return/throw, or an
    // unreachable fall-through) on every path. branch_terminates() now accounts
    // for exhaustive no-else `when`s (all arms terminating) and infinite loops,
    // so it no longer false-flags those. Coroutines never return a value (they
    // yield), and constructors/destructors go through their own body analyzers,
    // so neither reaches here as a non-void case. Lambdas analyze via
    // analyze_lambda_expr and are not covered by this check.
    Type* ret_type = m_symbols.current_return_type();
    if (ret_type && !ret_type->is_void()
            && !decl->fun_decl.is_coroutine
            && !m_lifetimes.branch_terminates()) {
        error_fmt(decl->loc, "not all code paths return a value in function '{}'",
                  decl->fun_decl.name);
    }

    m_lifetimes.check_scope_exit_uniq_destructors(m_symbols.current_scope(), decl->loc);
    m_symbols.pop_scope();
    // context_scope restores the outer per-function context on return.
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

                // Also register bare variant names for unqualified references.
                // The defining module already populated the type's variant
                // table (the authoritative source — no value recomputation).
                if (exp->type && exp->type->kind == TypeKind::Enum) {
                    for (const auto& variant : exp->type->enum_info.variants) {
                        m_symbols.define_enum_variant(variant.name, exp->type, name.loc, variant.value);
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

Span<Type*> SemanticAnalyzer::resolve_param_types(Span<Param> params) {
    Vector<Type*> param_types;
    for (const auto& param : params) {
        param_types.push_back(resolve_type_expr(param.type));
    }
    return m_allocator.alloc_span(param_types);
}

Type* SemanticAnalyzer::resolve_member_struct(SourceLocation loc, StringView struct_name,
                                              const char* noun) {
    Type* struct_type = m_type_env.named_type_by_name(struct_name);
    if (!struct_type) {
        error_fmt(loc, "{} for unknown struct '{}'", noun, struct_name);
        return nullptr;
    }
    if (struct_type->kind != TypeKind::Struct) {
        error_fmt(loc, "'{}' is not a struct type", struct_name);
        return nullptr;
    }
    return struct_type;
}

template<typename InfoT>
bool SemanticAnalyzer::report_duplicate_member(SourceLocation loc, Span<InfoT> existing,
                                               StringView name, StringView struct_name,
                                               const char* noun) {
    for (const auto& member : existing) {
        if (member.name == name) {
            if (name.empty()) {
                error_fmt(loc, "duplicate default {} for struct '{}'", noun, struct_name);
            } else {
                error_fmt(loc, "duplicate {} '{}' for struct '{}'", noun, name, struct_name);
            }
            return true;
        }
    }
    return false;
}

void SemanticAnalyzer::register_constructor_signature(Decl* decl) {
    ConstructorDecl& constructor_decl = decl->constructor_decl;

    Type* struct_type = resolve_member_struct(decl->loc, constructor_decl.struct_name, "constructor");
    if (!struct_type) return;
    if (report_duplicate_member(decl->loc, struct_type->struct_info.constructors,
                                constructor_decl.name, constructor_decl.struct_name, "constructor")) {
        return;
    }

    ConstructorInfo ctor_info;
    ctor_info.name = constructor_decl.name;
    ctor_info.param_types = resolve_param_types(constructor_decl.params);
    ctor_info.decl = decl;
    append_constructor(m_allocator, struct_type->struct_info, ctor_info);
}

void SemanticAnalyzer::register_destructor_signature(Decl* decl) {
    DestructorDecl& destructor_decl = decl->destructor_decl;

    Type* struct_type = resolve_member_struct(decl->loc, destructor_decl.struct_name, "destructor");
    if (!struct_type) return;
    if (report_duplicate_member(decl->loc, struct_type->struct_info.destructors,
                                destructor_decl.name, destructor_decl.struct_name, "destructor")) {
        return;
    }

    DestructorInfo dtor_info;
    dtor_info.name = destructor_decl.name;
    dtor_info.param_types = resolve_param_types(destructor_decl.params);
    dtor_info.decl = decl;
    append_destructor(m_allocator, struct_type->struct_info, dtor_info);
}

void SemanticAnalyzer::register_method_signature(Decl* decl) {
    MethodDecl& method_decl = decl->method_decl;

    Type* struct_type = resolve_member_struct(decl->loc, method_decl.struct_name, "method");
    if (!struct_type) return;
    if (report_duplicate_member(decl->loc, struct_type->struct_info.methods,
                                method_decl.name, method_decl.struct_name, "method")) {
        return;
    }

    MethodInfo method_info;
    method_info.name = method_decl.name;
    method_info.param_types = resolve_param_types(method_decl.params);
    method_info.return_type = method_decl.return_type
        ? resolve_type_expr(method_decl.return_type) : m_types.void_type();
    method_info.decl = decl;

    // A Coro<T>-returning method is a real coroutine only if its body yields
    // (same yield-based classification as free functions — see
    // register_fun_signature). A non-yielding one forwards a first-class coroutine
    // value and stays an ordinary method. Real coroutine methods get a
    // function-specific coroutine type whose func_name MUST equal the IR function
    // name `mangle_method(struct, name)` = "<struct>$$<method>" (IRBuilder::
    // mangle_method), since coroutine lowering builds the state struct / $$delete
    // from that name and nested coro-in-coro $$delete recursion keys on func_name.
    if (method_info.return_type && method_info.return_type->is_coroutine()) {
        method_decl.is_coroutine = stmt_contains_yield(method_decl.body);
        if (method_decl.is_coroutine) {
            StringView mangled = format_to_arena(m_allocator, "{}$${}",
                                                 method_decl.struct_name, method_decl.name);
            method_info.return_type = m_types.coroutine_type_for_func(
                method_info.return_type->coro_info.yield_type, mangled);
            populate_coro_methods(method_info.return_type);
        }
    }

    append_method(m_allocator, struct_type->struct_info, method_info);
}

void SemanticAnalyzer::analyze_member_body(Decl* decl, Type* struct_type,
                                            Span<Param> params, Stmt* body,
                                            Type* return_type,
                                            bool is_delete_destructor) {
    if (!body) return;

    // Single-shot rule: analysis mutates the AST it walks (see the annotation
    // contract in ast.hpp) — re-lower or clone a fresh tree instead of
    // re-analyzing this one.
    assert(!decl->body_analyzed && "AST body re-analysis (single-shot rule; see ast.hpp)");
    decl->body_analyzed = true;

    // Fresh per-function context (coroutine/destructor/finally slots) and
    // lifetime state for this body, restored as one unit on exit.
    FunctionContextScope context_scope(m_function_context, m_lifetimes);
    m_function_context.in_delete_destructor = is_delete_destructor;

    // Coroutine-method context. A method whose body yields (classified in
    // register_method_signature) is a real coroutine — its `self` is captured as
    // a `ref` param by coroutine lowering, exactly like a free-function coroutine.
    // A non-yielding Coro<T>-returning method takes neither branch: it is an
    // ordinary method that may `return <coro value>` (first-class forwarding).
    // Ctors/dtors return void, so they never enter here.
    bool is_coro_method = decl->kind == AstKind::DeclMethod && decl->method_decl.is_coroutine;
    if (is_coro_method) {
        m_function_context.in_coroutine = true;
        m_function_context.coro_yield_type = return_type->coro_info.yield_type;
    } else if (return_type && return_type->is_coroutine() && stmt_contains_yield(body)) {
        // A yielding Coro<T>-returning member that was NOT classified as a
        // coroutine method — i.e. a generic-struct or trait method, which take a
        // different registration path (register_generic_struct_method /
        // resolve_trait_impl_member) that doesn't set is_coroutine. Not supported
        // in v1: report a clear error (still enter coroutine context to suppress
        // cascading "yield outside coroutine" errors).
        error(decl->loc, "coroutine methods are not yet supported on generic structs or in traits");
        m_function_context.in_coroutine = true;
        m_function_context.coro_yield_type = return_type->coro_info.yield_type;
    }

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

        if (m_symbols.lookup_local(p.name)) {
            error_fmt(p.loc, "duplicate parameter name '{}'", p.name);
        } else {
            m_symbols.define_parameter(p.name, ptype, p.loc, i,
                                       p.modifier != ParamModifier::None);
        }

        // Track owned parameters as Live (uniq refs and value structs with destructors)
        if (ptype && ptype->noncopyable()) {
            m_lifetimes.track_live(m_symbols.lookup(p.name));
        }
    }

    // Analyze body
    analyze_stmt(body);

    m_lifetimes.check_scope_exit_uniq_destructors(m_symbols.current_scope(), decl->loc);
    m_symbols.pop_scope();  // function scope
    m_symbols.pop_scope();  // struct scope
    // context_scope restores the outer function's context on return.
}

void SemanticAnalyzer::analyze_constructor_body(Decl* decl, Type* struct_type) {
    auto& cd = decl->constructor_decl;
    analyze_member_body(decl, struct_type, cd.params, cd.body, m_types.void_type());
}

void SemanticAnalyzer::analyze_destructor_body(Decl* decl, Type* struct_type) {
    auto& dd = decl->destructor_decl;
    // A *delete* (unnamed) destructor body forbids throw; named destructors
    // (dd.name non-empty) are explicitly called and can throw.
    analyze_member_body(decl, struct_type, dd.params, dd.body, m_types.void_type(),
                        /*is_delete_destructor=*/dd.name.empty());
}

void SemanticAnalyzer::analyze_method_body(Decl* decl, Type* struct_type) {
    MethodDecl& method_decl = decl->method_decl;
    Type* return_type = method_decl.return_type ? resolve_type_expr(method_decl.return_type) : m_types.void_type();
    analyze_member_body(decl, struct_type, method_decl.params, method_decl.body, return_type);
}

// ===== Operator Dispatch Helpers =====

Type* SemanticAnalyzer::try_resolve_binary_op(BinaryOp op, Type* left, Type* right) {
    StringView name = binary_op_to_trait_method(op);
    if (name.empty()) return nullptr;
    // Primitive operands resolve through a dense (kind, op) table instead of the
    // name-keyed hash + linear scan in lookup_primitive_method (§3.5).
    const MethodInfo* mi = left->is_primitive()
        ? m_types.lookup_primitive_binary_op(left->kind, op)
        : m_types.lookup_method(left, name);
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
    if (left->is_type_param() && m_generic_calls.has_active_bounds()) {
        Type* found_in_trait = nullptr;
        const TraitMethodInfo* trait_method = m_generic_calls.lookup_type_param_method(left, name, &found_in_trait);
        if (trait_method && trait_method->param_types.size() == 1) {
            Type* return_type = m_generic_calls.substitute_trait_types(trait_method->return_type, left, found_in_trait);
            Type* param_type = m_generic_calls.substitute_trait_types(trait_method->param_types[0], left, found_in_trait);
            // Check right operand compatibility
            if (param_type == right || m_checker.is_assignable(param_type, right)) {
                return return_type;
            }
        }
    }
    return nullptr;
}

Type* SemanticAnalyzer::try_resolve_unary_op(UnaryOp op, Type* operand) {
    StringView name = unary_op_to_trait_method(op);
    if (name.empty()) return nullptr;
    const MethodInfo* mi = operand->is_primitive()
        ? m_types.lookup_primitive_unary_op(operand->kind, op)
        : m_types.lookup_method(operand, name);
    if (mi && mi->param_types.size() == 0 && mi->return_type) {
        return mi->return_type;
    }

    // Phase B: TypeParam path — look up through trait bounds
    if (operand->is_type_param() && m_generic_calls.has_active_bounds()) {
        Type* found_in_trait = nullptr;
        const TraitMethodInfo* trait_method = m_generic_calls.lookup_type_param_method(operand, name, &found_in_trait);
        if (trait_method && trait_method->param_types.size() == 0 && trait_method->return_type) {
            return m_generic_calls.substitute_trait_types(trait_method->return_type, operand, found_in_trait);
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

    m_lifetimes.check_scope_exit_uniq_destructors(m_symbols.current_scope(), stmt->loc);
    m_symbols.pop_scope();
}

void SemanticAnalyzer::analyze_if_stmt(Stmt* stmt) {
    IfStmt& is = stmt->if_stmt;

    Type* cond_type = analyze_expr(is.condition);
    if (cond_type && !cond_type->is_error()) {
        m_checker.check_boolean(cond_type, is.condition->loc);
    }

    // Save move states before branching
    MoveStateSnapshot pre_branch_states = m_lifetimes.save_move_states();

    // Reset termination flag for the then-branch; capture whether it ended
    // with an unconditional return/throw/break/continue.
    m_lifetimes.set_branch_terminates(false);
    analyze_stmt(is.then_branch);
    MoveStateSnapshot then_states = m_lifetimes.save_move_states();
    bool then_terminates = m_lifetimes.branch_terminates();

    if (is.else_branch) {
        // Restore to pre-branch state for else analysis
        m_lifetimes.restore_move_states(pre_branch_states);
        m_lifetimes.set_branch_terminates(false);
        analyze_stmt(is.else_branch);
        MoveStateSnapshot else_states = m_lifetimes.save_move_states();
        bool else_terminates = m_lifetimes.branch_terminates();

        m_lifetimes.merge_two_branches(pre_branch_states, then_states, else_states,
                           then_terminates, else_terminates);
    } else {
        // No else branch — the implicit else is the pre-branch state (no moves)
        // and can never terminate. merge_two_branches therefore leaves the
        // branch-terminates flag false: a no-else if never proves termination
        // because the condition may be false.
        m_lifetimes.merge_two_branches(pre_branch_states, then_states, pre_branch_states,
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
    MoveStateSnapshot pre_loop_states = m_lifetimes.save_move_states();

    // The loop body may execute zero times, so any termination inside the
    // body (return/throw/break/continue) does not prove the loop itself
    // terminates. Preserve the caller's flag across the body.
    bool pre_loop_terminates = m_lifetimes.branch_terminates();

    m_symbols.push_loop_scope();
    analyze_stmt(ws.body);
    m_lifetimes.check_scope_exit_uniq_destructors(m_symbols.current_scope(), stmt->loc);
    m_symbols.pop_scope();

    // An infinite loop with no reachable `break` never falls through — control
    // after it is unreachable — so the loop counts as terminating (e.g. a
    // function whose body ends in `while (true) { ... }` needs no trailing
    // return). Otherwise the body may execute zero times, so restore the
    // caller's flag (see note above).
    if (is_const_true_condition(ws.condition) && !stmt_has_direct_break(ws.body)) {
        m_lifetimes.set_branch_terminates(true);
    } else {
        m_lifetimes.set_branch_terminates(pre_loop_terminates);
    }

    MoveStateSnapshot post_body_states = m_lifetimes.save_move_states();

    m_lifetimes.check_loop_cross_iteration_moves(ws.body, pre_loop_states, post_body_states, stmt->loc);

    // After loop: merge pre-loop with post-body (loop may execute 0 times)
    m_lifetimes.restore_move_states(pre_loop_states);
    m_lifetimes.merge_move_states(post_body_states, pre_loop_states);
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
    MoveStateSnapshot pre_loop_states = m_lifetimes.save_move_states();

    // The loop body may execute zero times — see note in analyze_while_stmt.
    bool pre_loop_terminates = m_lifetimes.branch_terminates();

    m_symbols.push_loop_scope();
    analyze_stmt(fs.body);
    m_lifetimes.check_scope_exit_uniq_destructors(m_symbols.current_scope(), stmt->loc);
    m_symbols.pop_scope();

    m_lifetimes.set_branch_terminates(pre_loop_terminates);

    if (fs.increment) {
        analyze_expr(fs.increment);
    }

    MoveStateSnapshot post_body_states = m_lifetimes.save_move_states();

    m_lifetimes.check_loop_cross_iteration_moves(fs.body, pre_loop_states, post_body_states, stmt->loc);

    // After loop: merge pre-loop with post-body (loop may execute 0 times)
    m_lifetimes.restore_move_states(pre_loop_states);
    m_lifetimes.merge_move_states(post_body_states, pre_loop_states);

    m_lifetimes.check_scope_exit_uniq_destructors(m_symbols.current_scope(), stmt->loc);
    m_symbols.pop_scope();
}

void SemanticAnalyzer::analyze_return_stmt(Stmt* stmt) {
    ReturnStmt& rs = stmt->return_stmt;

    if (!m_symbols.is_in_function()) {
        error(stmt->loc, "'return' statement outside of function");
        return;
    }

    // In coroutine functions, only bare 'return;' is allowed (no return value)
    if (m_function_context.in_coroutine) {
        if (rs.value) {
            error(stmt->loc, "coroutine functions cannot return a value; use 'yield' instead");
        }
        m_lifetimes.set_branch_terminates(true);
        return;
    }

    Type* expected = m_symbols.current_return_type();

    if (rs.value) {
        Type* actual = analyze_expr(rs.value);
        if (expected && m_generic_calls.coerce_generic_template_ref(rs.value, expected)) {
            actual = rs.value->resolved_type;
        }
        if (!m_checker.check_assignable(expected, actual, stmt->loc)) {
            // Error already reported
        } else {
            m_checker.coerce_numeric_literal(rs.value, expected);
        }

        // Consume noncopyable return value (field-move check + mark source as moved)
        m_lifetimes.consume_noncopyable(rs.value, stmt->loc);
    } else {
        if (!expected->is_void()) {
            error(stmt->loc, "non-void function must return a value");
        }
    }

    m_lifetimes.check_all_scopes_uniq_destructors(stmt->loc, ScopeKind::Function);
    m_lifetimes.set_branch_terminates(true);
}

void SemanticAnalyzer::analyze_break_stmt(Stmt* stmt) {
    if (!m_symbols.is_in_loop()) {
        error(stmt->loc, "'break' statement outside of loop");
    } else {
        m_lifetimes.check_all_scopes_uniq_destructors(stmt->loc, ScopeKind::Loop);
    }
    m_lifetimes.set_branch_terminates(true);
}

void SemanticAnalyzer::analyze_continue_stmt(Stmt* stmt) {
    if (!m_symbols.is_in_loop()) {
        error(stmt->loc, "'continue' statement outside of loop");
    } else {
        m_lifetimes.check_all_scopes_uniq_destructors(stmt->loc, ScopeKind::Loop);
    }
    m_lifetimes.set_branch_terminates(true);
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

    // Deleting through a field moves that field out. check_not_field_move
    // permits it for a uniq *pointer* field (the IR builder nulls the slot in
    // the root so its destructor won't double-free) but rejects value-struct
    // fields and moves through a reference chain. See check_not_field_move.
    if (!m_lifetimes.check_not_field_move(ds.expr, stmt->loc)) return;

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
                error_fmt(stmt->loc, "destructor expects {} argument(s) but got {}",
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
    m_lifetimes.consume_noncopyable(ds.expr, stmt->loc);
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

    // Case names resolve through the enum type's own variant table (not the
    // decl, which is null for native enums; not the flat symbol namespace,
    // which collides across enums).
    EnumTypeInfo& eti = discrim_type->enum_info;

    // Track which enum variants have been covered (for duplicate detection)
    tsl::robin_map<StringView, bool> covered_variants;

    // Save move states before branching
    MoveStateSnapshot pre_when_states = m_lifetimes.save_move_states();
    Vector<MoveStateSnapshot> case_snapshots;
    Vector<bool> case_terminates;

    // Analyze each case
    for (auto& wc : ws.cases) {
        // Validate case names are valid enum variants
        for (const auto& case_name : wc.case_names) {
            if (!eti.find_variant(case_name)) {
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
        m_lifetimes.restore_move_states(pre_when_states);
        m_lifetimes.set_branch_terminates(false);

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

        case_snapshots.push_back(m_lifetimes.save_move_states());
        case_terminates.push_back(m_lifetimes.branch_terminates());
    }

    // Exhaustive iff the cases cover every variant of the discriminant enum.
    // `covered_variants` holds exactly the distinct valid variants matched
    // (invalid names are never inserted, grouped/duplicate names are deduped),
    // so an equal count means full coverage. Read by the IR builder to trap the
    // (then unreachable) no-else fall-through.
    ws.is_exhaustive = covered_variants.size() == eti.variants.size();

    // Analyze else body if present
    bool has_else = ws.else_body.size() > 0;
    if (has_else) {
        m_lifetimes.restore_move_states(pre_when_states);
        m_lifetimes.set_branch_terminates(false);

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

        case_snapshots.push_back(m_lifetimes.save_move_states());
        case_terminates.push_back(m_lifetimes.branch_terminates());
    } else if (!ws.is_exhaustive) {
        // Non-exhaustive no-else — an unmatched enum value falls past the whole
        // statement, so the pre-when state is a non-terminating survivor path.
        case_snapshots.push_back(pre_when_states);
        case_terminates.push_back(false);
    }
    // Exhaustive no-else: no survivor fall-through path (an unmatched value is
    // impossible), so merge only the case arms — like a `when` with an else.

    // Merge the surviving (non-terminating) case paths. If every path
    // terminates, the code after the when is unreachable and termination
    // propagates upward. For a non-exhaustive no-else `when` the injected
    // fall-through path is always non-terminating, so this stays false.
    m_lifetimes.set_branch_terminates(m_lifetimes.merge_branch_snapshots(case_snapshots, case_terminates));
}

void SemanticAnalyzer::analyze_throw_stmt(Stmt* stmt) {
    ThrowStmt& ts = stmt->throw_stmt;

    if (m_function_context.in_delete_destructor) {
        error(stmt->loc, "'throw' is not allowed inside a delete destructor");
        return;
    }

    Type* expr_type = analyze_expr(ts.expr);
    if (!expr_type || expr_type->is_error()) {
        m_lifetimes.set_branch_terminates(true);
        return;
    }

    // The thrown expression must be a struct type that implements Exception trait
    Type* base = expr_type->base_type();
    if (!base->is_struct()) {
        error(stmt->loc, "throw expression must be a struct type that implements the Exception trait");
        m_lifetimes.set_branch_terminates(true);
        return;
    }

    Type* exception_trait = m_type_env.exception_type();
    if (!m_types.implements_trait(base, exception_trait)) {
        error_fmt(stmt->loc, "thrown type '{}' does not implement the Exception trait",
                  base->struct_info.name);
    }
    m_lifetimes.set_branch_terminates(true);
}

void SemanticAnalyzer::analyze_try_stmt(Stmt* stmt) {
    TryStmt& ts = stmt->try_stmt;

    // Save move states before try body
    MoveStateSnapshot pre_try = m_lifetimes.save_move_states();

    // Analyze try body (yield is allowed here)
    m_lifetimes.set_branch_terminates(false);
    analyze_stmt(ts.try_body);
    bool try_terminates = m_lifetimes.branch_terminates();

    // Save post-try states
    MoveStateSnapshot post_try = m_lifetimes.save_move_states();

    // Compute catch entry state: merge(pre_try, post_try)
    // An exception can be thrown at any point in the try body, so catch clauses
    // must see the conservative merge of pre-try and post-try states
    m_lifetimes.restore_move_states(pre_try);
    m_lifetimes.merge_move_states(pre_try, post_try);
    MoveStateSnapshot catch_entry = m_lifetimes.save_move_states();

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
        m_lifetimes.restore_move_states(catch_entry);
        m_lifetimes.set_branch_terminates(false);

        m_symbols.push_scope(ScopeKind::Block);

        // The catch variable is a local declaration, so the shadowing ban
        // applies. On a shadow, skip the define (the body then reports uses
        // as undefined — same cascade as the duplicate-parameter path).
        bool catch_var_ok = check_no_local_shadowing(clause.var_name, clause.loc);

        if (clause.exception_type) {
            // Typed catch: catch (e: Type)
            Type* catch_type = resolve_type_expr(clause.exception_type);
            if (!catch_type->is_error()) {
                Type* base = catch_type->base_type();
                if (!base->is_struct()) {
                    error(clause.loc, "catch type must be a struct type that implements the Exception trait");
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
                if (catch_var_ok) {
                    m_symbols.define(SymbolKind::Variable, clause.var_name, ref_catch_type,
                                     clause.loc);
                }
            }
        } else {
            // Catch-all: catch (e)
            has_catch_all = true;
            clause.resolved_type = nullptr;
            // Define as ExceptionRef (opaque handle, only message() callable)
            if (catch_var_ok) {
                m_symbols.define(SymbolKind::Variable, clause.var_name,
                                 m_types.exception_ref_type(), clause.loc);
            }
        }

        analyze_stmt(clause.body);  // yield is allowed in catch bodies
        m_symbols.pop_scope();

        exit_paths.push_back(m_lifetimes.save_move_states());
        exit_terminates.push_back(m_lifetimes.branch_terminates());
    }

    // Merge the surviving (non-terminating) exit paths. If every exit path
    // terminates, code after the try is unreachable and all_terminate is true.
    bool all_terminate = m_lifetimes.merge_branch_snapshots(exit_paths, exit_terminates);

    // Analyze finally body if present (yield is NOT allowed here).
    if (ts.finally_body) {
        // Snapshot the normal (non-terminating) continuation before repurposing
        // the move-state map to build the finally-entry state.
        MoveStateSnapshot normal_continuation = m_lifetimes.save_move_states();

        // A finally runs on EVERY path leaving the try — the normal exit, each
        // catch clause (even ones that return/re-throw), and the uncaught
        // pass-through (an exception matching no catch). Analyze it against the
        // conservative union of all those paths' move states, so a
        // use-after-move that happens only on, say, a catch-then-return path is
        // still caught. merge_branch_snapshots above drops terminating paths —
        // correct for the continuation, unsound for the finally body.
        // catch_entry covers the try-body moves and the pass-through; folding
        // in every exit_paths snapshot adds each catch's moves (terminating or
        // not). check_not_moved rejects both Moved and MaybeValid, so a move on
        // any single path is enough to flag a use here.
        m_lifetimes.restore_move_states(catch_entry);
        for (const auto& snapshot : exit_paths) {
            MoveStateSnapshot current = m_lifetimes.save_move_states();
            m_lifetimes.merge_move_states(current, snapshot);
        }
        MoveStateSnapshot finally_entry = m_lifetimes.save_move_states();

        {
            ScopedValue finally_depth_guard(m_function_context.finally_depth);
            m_function_context.finally_depth++;
            m_lifetimes.set_branch_terminates(false);
            analyze_stmt(ts.finally_body);
        }

        // Rebuild the continuation: the normal-path merge with finally's own
        // effects applied. A variable finally actually changed (its state
        // differs from finally_entry — a move or a reassignment) takes finally's
        // result; one finally left untouched keeps its normal-path state, so a
        // move that happened only on a terminating path does not leak past the
        // try.
        MoveStateSnapshot finally_post = m_lifetimes.save_move_states();
        for (auto it = normal_continuation.begin(); it != normal_continuation.end(); ++it) {
            auto post_it = finally_post.find(it->first);
            auto entry_it = finally_entry.find(it->first);
            if (post_it != finally_post.end() && entry_it != finally_entry.end()
                && post_it->second != entry_it->second) {
                it.value() = post_it->second;
            }
        }
        m_lifetimes.restore_move_states(normal_continuation);

        // If finally terminates, so does the whole statement; otherwise
        // use the all_terminate result computed from try+catches.
        if (!m_lifetimes.branch_terminates()) {
            m_lifetimes.set_branch_terminates(all_terminate);
        }
    } else {
        m_lifetimes.set_branch_terminates(all_terminate);
    }
}

void SemanticAnalyzer::analyze_yield_stmt(Stmt* stmt) {
    YieldStmt& ys = stmt->yield_stmt;

    if (!m_function_context.in_coroutine) {
        error(stmt->loc, "'yield' can only appear inside a coroutine function (one returning Coro<T>)");
        return;
    }

    if (m_function_context.finally_depth > 0) {
        error(stmt->loc, "'yield' inside finally is not supported");
        return;
    }

    Type* actual = analyze_expr(ys.value);
    if (!actual || actual->is_error()) return;

    if (!m_checker.check_assignable(m_function_context.coro_yield_type, actual, stmt->loc)) {
        // Error already reported
    } else {
        m_checker.coerce_numeric_literal(ys.value, m_function_context.coro_yield_type);
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
        // Settle an unsuffixed literal on its default so it has a Printable
        // implementation — the trait is registered on concrete primitives.
        if (etype->is_numeric_literal()) {
            m_checker.coerce_numeric_literal(expression, default_literal_type(etype));
            etype = expression->resolved_type;
        }
        // Uniform trait check for ALL types (primitives and structs)
        if (!m_types.implements_trait(etype, m_type_env.printable_type())) {
            error_fmt(expression->loc,
                     "type '{}' does not implement Printable (no to_string method)",
                     m_checker.type_string(etype).data());
        }
    }
    return m_types.string_type();
}

Type* SemanticAnalyzer::default_literal_type(Type* type) {
    if (!type) return type;
    if (type->is_int_literal()) return m_types.i32_type();
    if (type->is_float_literal()) return m_types.f64_type();
    return type;
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
            // Unsuffixed: polymorphic until the context picks f32/f64, mirroring
            // the I32 case above. `1.0f` is LiteralKind::F32 and stays concrete.
            return m_types.float_literal_type();
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
            return m_generic_calls.resolve_explicit_generic_template_ref(expr);
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

    // Cache the resolved symbol for the call path (analyze_regular_fun_call reads
    // it to fetch the callee's FunDecl params without a second lookup). Safe even
    // if the capture path below rewrites the expr: that only happens for
    // capturable (non-Function) symbols, and the call path guards on the callee
    // still being an ExprIdentifier.
    id.resolved_sym = sym;

    // Closure-capture path: if this identifier resolves across a lambda
    // boundary, record the capture(s) and rewrite the expr in place.
    Type* captured_type = nullptr;
    if (try_capture_identifier(expr, sym, &captured_type)) {
        return captured_type;
    }

    // Check move state for owned variables (uniq refs and value structs with destructors)
    // Note: we check here but the actual move marking happens at call sites, return, delete
    if (sym->type && sym->type->noncopyable()) {
        m_lifetimes.check_not_moved(sym, id.name, expr->loc);
    }

    return sym->type;
}

Vector<u32> SemanticAnalyzer::collect_crossed_lambda_contexts(const Scope* stop_scope) {
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

Type* SemanticAnalyzer::analyze_lambda_expr(Expr* expr) {
    LambdaExpr& le = expr->lambda;

    // Resolve user-facing param and return types (the `Function<sig>` type).
    Vector<Type*> sig_param_types;
    for (auto& p : le.params) {
        Type* pt = resolve_type_expr(p.type);
        if (pt->is_error()) return m_types.error_type();
        sig_param_types.push_back(pt);
    }
    Type* ret_type = le.return_type ? resolve_type_expr(le.return_type) : m_types.void_type();
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
                error_fmt(expr->loc,
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
            if (!outer_sym->type || outer_sym->type->is_copy()) {
                error_fmt(entry.loc,
                    "move captures only apply to noncopyable types; '{}' is copyable, capture it implicitly",
                    entry.name);
                return false;
            }
            if (context.by_symbol.find(outer_sym) != context.by_symbol.end()) {
                error_fmt(entry.loc, "duplicate capture entry for '{}'", entry.name);
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
        if (entry.name != "self"_sv) {
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

            Expr* env_id = make_identifier_expr("__env"_sv, outer_env_ref, loc);
            return make_get_expr(env_id, "__self"_sv, ref_self, loc);
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
            Type* ptype = resolve_type_expr(p.type);
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
                error_fmt(p.loc, "duplicate parameter name '{}'", p.name);
            } else if (is_synthesized_env || check_no_local_shadowing(p.name, p.loc)) {
                m_symbols.define_parameter(p.name, ptype, p.loc, i,
                                       p.modifier != ParamModifier::None);
            }
            if (ptype && ptype->noncopyable()) {
                m_lifetimes.track_live(m_symbols.lookup(p.name));
            }
        }

        analyze_stmt(fd.body);

        // All-paths-return: a non-void lambda body must terminate (return/throw
        // or an unreachable fall-through) on every path, exactly like a free
        // function (analyze_fun_body). Lambdas are never coroutines, so there is
        // no yield exemption. The `=> expr` short body desugars to a `return`, so
        // only block-bodied lambdas that fall off the end are flagged here.
        if (ret_type && !ret_type->is_void() && !m_lifetimes.branch_terminates()) {
            error_fmt(expr->loc, "not all code paths return a value in lambda");
        }

        m_lifetimes.check_scope_exit_uniq_destructors(m_symbols.current_scope(), expr->loc);
        m_symbols.pop_scope();  // function scope
        // context_scope restores the outer per-function context at block end.
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
                  m_checker.type_string(operand_type).data());
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

    // Coerce unsuffixed literals: match the concrete side, or stay polymorphic
    // if both sides are literals. `is_numeric` and not `is_integer` on the
    // concrete side so an int literal adapts to a float operand too (`1 + 2.0`).
    // A float literal only matches a float operand — `1.0 + 2l` stays an error.
    if (left_type->is_numeric_literal() && right_type->is_numeric()) {
        m_checker.coerce_numeric_literal(binary_expr.left, right_type);
        left_type = binary_expr.left->resolved_type;
    } else if (right_type->is_numeric_literal() && left_type->is_numeric()) {
        m_checker.coerce_numeric_literal(binary_expr.right, left_type);
        right_type = binary_expr.right->resolved_type;
    } else if (left_type->is_numeric_literal() && right_type->is_numeric_literal()) {
        // Arithmetic on two unsuffixed literals is itself an unsuffixed literal.
        // If either side is a float literal the pair defaults to f64, otherwise
        // i32 — mixing them (`1 + 2.0`) makes the result a float literal, which
        // keeps `var x: f32 = 1 + 2.0;` open to the annotation.
        bool is_float = left_type->is_float_literal() || right_type->is_float_literal();
        Type* fallback = is_float ? m_types.f64_type() : m_types.i32_type();
        Type* literal_type = is_float ? m_types.float_literal_type()
                                      : m_types.int_literal_type();

        // Resolve against the default to validate the operator and learn the
        // result shape, but hand back the literal type so an enclosing context
        // can still choose — `var x: i64 = 1 + 2;`. coerce_numeric_literal
        // recurses into both operands when that context arrives; with no such
        // context everything defaults exactly as before.
        Type* result = get_binary_result_type(binary_expr.op, fallback, fallback, expr->loc);
        if (result && result->is_numeric()) {
            return literal_type;
        }
        // Comparison (bool) or an error: nothing downstream can pick a type for
        // the operands, so settle them on the default now.
        m_checker.coerce_numeric_literal(binary_expr.left, fallback);
        m_checker.coerce_numeric_literal(binary_expr.right, fallback);
        return result;
    } else if (right_type->is_int_literal() && !left_type->is_int_literal()) {
        // Right is IntLiteral, left is non-integer (e.g., struct) — coerce to method's param type
        StringView name = binary_op_to_trait_method(binary_expr.op);
        if (!name.empty()) {
            const MethodInfo* mi = m_types.lookup_method(left_type, name);
            if (mi && mi->param_types.size() == 1 && mi->param_types[0]->is_integer()) {
                m_checker.coerce_numeric_literal(binary_expr.right, mi->param_types[0]);
                right_type = mi->param_types[0];
            }
        }
    } else if (left_type->is_int_literal() && !right_type->is_int_literal()) {
        // Left is IntLiteral, right is non-integer — coerce to method's param type
        StringView name = binary_op_to_trait_method(binary_expr.op);
        if (!name.empty()) {
            const MethodInfo* mi = m_types.lookup_method(right_type, name);
            if (mi && mi->param_types.size() == 1 && mi->param_types[0]->is_integer()) {
                m_checker.coerce_numeric_literal(binary_expr.left, mi->param_types[0]);
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
    MoveStateSnapshot pre_branch_states = m_lifetimes.save_move_states();

    Type* then_type = analyze_expr(ternary_expr.then_expr);
    MoveStateSnapshot then_states = m_lifetimes.save_move_states();

    m_lifetimes.restore_move_states(pre_branch_states);
    Type* else_type = analyze_expr(ternary_expr.else_expr);
    MoveStateSnapshot else_states = m_lifetimes.save_move_states();

    m_lifetimes.restore_move_states(pre_branch_states);
    m_lifetimes.merge_move_states(then_states, else_states);

    if (then_type->is_error()) return else_type;
    if (else_type->is_error()) return then_type;

    // Coerce unsuffixed literals in ternary branches, mirroring analyze_binary_expr:
    // match the concrete branch, or keep both polymorphic so an enclosing context
    // can still choose (`var x: i64 = c ? 1 : 2;`).
    if (then_type->is_numeric_literal() && else_type->is_numeric()) {
        m_checker.coerce_numeric_literal(ternary_expr.then_expr, else_type);
        then_type = ternary_expr.then_expr->resolved_type;
    } else if (else_type->is_numeric_literal() && then_type->is_numeric()) {
        m_checker.coerce_numeric_literal(ternary_expr.else_expr, then_type);
        else_type = ternary_expr.else_expr->resolved_type;
    } else if (then_type->is_numeric_literal() && else_type->is_numeric_literal()) {
        if (then_type == else_type) {
            return then_type;  // both int literals, or both float literals
        }
        // One of each (`c ? 1 : 2.0`): the int side adapts to float.
        m_checker.coerce_numeric_literal(ternary_expr.then_expr, m_types.f64_type());
        m_checker.coerce_numeric_literal(ternary_expr.else_expr, m_types.f64_type());
        then_type = ternary_expr.then_expr->resolved_type;
        else_type = ternary_expr.else_expr->resolved_type;
    }

    // A ternary that selects a noncopyable value is unsupported. gen_ternary_expr
    // merges each branch's value through a phi, but the per-branch noncopyable
    // temporaries are never reconciled with that phi: the taken branch's value is
    // freed once by its consumer and again by statement-end cleanup (double-free),
    // and selecting an owned *variable* never nullifies it the way a real move
    // does. The cleanup model can't conditionally null a ternary operand, so
    // reject the construct rather than miscompile it.
    if ((then_type && then_type->noncopyable()) ||
        (else_type && else_type->noncopyable())) {
        error(expr->loc,
            "conditional expression cannot select a noncopyable value; it would move "
            "an operand without nullifying it (double-free). Use an if/else statement "
            "with an explicit move in each branch instead");
        return m_types.error_type();
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

        // An `out`/`inout` container subscript is an lvalue on the element *slot*,
        // not a read: re-type it to the raw element/value type (the `index`
        // method returns the `borrowed` view — `ref T` for an owning `uniq T`
        // element — which is right for reads but wrong here, since `inout` gives
        // reassignable access to the owning slot). Copyable elements are
        // unaffected (`borrowed T` == `T`). See lifetimes.md "Container element lvalues".
        if ((arg.modifier == ParamModifier::Inout || arg.modifier == ParamModifier::Out)
            && arg.expr->kind == AstKind::ExprIndex) {
            Type* cont = arg.expr->index.object->resolved_type;
            Type* base = cont ? cont->base_type() : nullptr;
            Type* elem = nullptr;
            if (base && base->is_list()) elem = base->list_info.element_type;
            else if (base && base->is_map()) elem = base->map_info.value_type;
            if (elem && !elem->is_error()) {
                arg.expr->resolved_type = elem;
                arg_type = elem;
            }
        }

        // Resolve generic-template-ref arg against param type before
        // assignability checking. Updates arg_type via resolved_type.
        if (param_types[i] && m_generic_calls.coerce_generic_template_ref(arg.expr, param_types[i])) {
            arg_type = arg.expr->resolved_type;
        }

        // Type check (skip for 'out' since it's write-only)
        if (arg.modifier != ParamModifier::Out) {
            m_checker.check_assignable(param_types[i], arg_type, arg.expr->loc);
            m_checker.coerce_numeric_literal(arg.expr, param_types[i]);
        }

        // Move semantics: passing owned arg to owned param transfers ownership —
        // but only for by-value arguments. `inout` and `out` borrow through a
        // pointer; the callee sees the same slot the caller still owns, so
        // treating them as a move falsely rejects loops like
        // `while (...) { fn(inout xs); }` with "moved in loop body without
        // reassignment", even though `xs` stays live across iterations.
        if (param_types[i] && param_types[i]->noncopyable()
            && arg.modifier == ParamModifier::None) {
            m_lifetimes.consume_noncopyable(arg.expr, arg.expr->loc);
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

void SemanticAnalyzer::populate_enum_methods(Type* type) {
    assert(type && type->is_enum());

    // eq(other: Self): bool, ne(other: Self): bool
    Span<Type*> self_param(m_allocator.emplace<Type*>(type), 1);
    Vector<MethodInfo> methods;
    methods.push_back(make_method("eq"_sv, self_param, m_types.bool_type()));
    methods.push_back(make_method("ne"_sv, self_param, m_types.bool_type()));
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
    methods.push_back(make_method("resume"_sv, Span<Type*>(), yield_type, resume_native));
    methods.push_back(make_method("done"_sv, Span<Type*>(), m_types.bool_type(), done_native));
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
    if (elem_type->is_error()) return m_types.error_type();

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
        error_fmt(expr->loc, "List constructor expects {} to {} argument(s) but got {}",
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
    if (key_type->is_error()) return m_types.error_type();
    if (val_type->is_error()) return m_types.error_type();

    if (!is_hashable_key_type(key_type)) {
        error(expr->loc, "Map key type must be a primitive, enum, or struct");
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
        error_fmt(expr->loc, "Map constructor expects 0 to 1 argument(s) but got {}",
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
        if (arg_type->is_error()) return m_types.error_type();
        type_arg_types.push_back(arg_type);
    }

    // Check trait bounds on type args
    Span<Type*> type_args = m_allocator.alloc_span(type_arg_types);
    const ResolvedTypeParams* bounds = m_type_env.generics().get_struct_bounds(func_name);
    if (!m_generic_calls.check_type_arg_bounds(func_name, type_args, bounds, expr->loc)) {
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

void SemanticAnalyzer::check_super_call_arg_types(CallExpr& ce, Span<Type*> param_types) {
    for (u32 i = 0; i < ce.arguments.size(); i++) {
        CallArg& arg = ce.arguments[i];
        Type* arg_type = analyze_expr(arg.expr);
        if (!m_checker.check_assignable(param_types[i], arg_type, arg.expr->loc)) {
            // Error already reported
        } else {
            m_checker.coerce_numeric_literal(arg.expr, param_types[i]);
        }
    }
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
                error_fmt(expr->loc, "parent constructor expects {} argument(s) but got {}",
                         ctor->param_types.size(), ce.arguments.size());
                return m_types.void_type();
            }
            check_super_call_arg_types(ce, ctor->param_types);
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
            error_fmt(expr->loc, "parent constructor expects {} argument(s) but got {}",
                     ctor->param_types.size(), ce.arguments.size());
            return m_types.void_type();
        }
        check_super_call_arg_types(ce, ctor->param_types);

        // Annotate that this super call resolved to a named CONSTRUCTOR — the
        // IR builder must not infer ctor-vs-method from the void result type
        // (a super *method* returning void would be misclassified).
        ce.constructor_name = super_expr.method_name;
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
            error_fmt(expr->loc, "method '{}' expects {} argument(s) but got {}",
                     super_expr.method_name, mi->param_types.size(), ce.arguments.size());
            return mi->return_type;
        }
        check_super_call_arg_types(ce, mi->param_types);

        // Store found_in_type for IR builder to mangle correctly
        ce.callee->resolved_type = m_types.ref_type(found_in_type);
        return mi->return_type;
    }

    // Neither constructor nor method
    error_fmt(expr->loc, "parent struct '{}' has no constructor or method '{}'",
             parent_struct_type_info.name, super_expr.method_name);
    return m_types.error_type();
}

bool SemanticAnalyzer::check_container_copy_method(Expr* expr, Type* base_type,
                                                   StringView method_name) {
    if (method_name != "copy"_sv || !base_type) return true;

    if (base_type->is_list()) {
        Type* et = base_type->list_info.element_type;
        if (et && !et->is_copy()) {
            error_fmt(expr->loc,
                "cannot '.copy()' a List with a non-copyable element type '{}' "
                "(its elements own resources that can't be duplicated)",
                m_checker.type_string(et).data());
            return false;
        }
    } else if (base_type->is_map()) {
        Type* kt = base_type->map_info.key_type;
        Type* vt = base_type->map_info.value_type;
        if ((kt && !kt->is_copy()) || (vt && !vt->is_copy())) {
            error_fmt(expr->loc,
                "cannot '.copy()' a Map with a non-copyable key or value type "
                "(its entries own resources that can't be duplicated)");
            return false;
        }
    }
    return true;
}

Type* SemanticAnalyzer::analyze_builtin_method_call(Expr* expr, CallExpr& ce, GetExpr& ge,
                                                     Type* obj_type, const MethodInfo* mi) {
    Type* base_type = obj_type->base_type();
    if (!check_container_copy_method(expr, base_type, mi->name)) {
        return m_types.error_type();
    }

    if (ce.arguments.size() != mi->param_types.size()) {
        error_fmt(expr->loc, "{}() expects {} argument(s) but got {}",
                 mi->name, mi->param_types.size(),
                 ce.arguments.size());
        return mi->return_type;
    }
    check_call_args(ce.arguments, mi->param_types, Span<Param>(), expr->loc);
    ce.mangled_name = mi->native_name;
    ge.object->resolved_type = obj_type;

    // Set callee's resolved_type to a function type for IR builder move tracking
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
        error_fmt(expr->loc, "method expects {} argument(s) but got {}",
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

    // A borrowed function value (`ref fun` / `weak fun`) is the same env-pointer
    // representation as a `fun` value, so it is callable too — unwrap the borrow.
    Type* fn_type = callee_type->base_type();
    if (!fn_type->is_function()) {
        error(ce.callee->loc, "expression is not callable");
        return m_types.error_type();
    }

    FunctionTypeInfo& fti = fn_type->func_info;

    // Check argument count
    if (ce.arguments.size() != fti.param_types.size()) {
        error_fmt(expr->loc, "call expects {} argument(s) but got {}",
                 fti.param_types.size(), ce.arguments.size());
        return fti.return_type;
    }

    // Try to get the FunDecl to access parameter modifiers. analyze_expr(ce.callee)
    // above already resolved the identifier and cached its symbol, so reuse that
    // instead of a second SymbolTable lookup (§3.4).
    Span<Param> params;
    if (ce.callee->kind == AstKind::ExprIdentifier) {
        Symbol* sym = ce.callee->identifier.resolved_sym;
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
            return m_generic_calls.analyze_generic_fun_call(expr, call_expr, func_name);
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
            return m_generic_calls.analyze_generic_fun_call_inferred(expr, call_expr, func_name);
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
            if (base_type && base_type->is_type_param() && m_generic_calls.has_active_bounds()) {
                Type* found_in_trait = nullptr;
                const TraitMethodInfo* trait_method = m_generic_calls.lookup_type_param_method(base_type, get_expr.name, &found_in_trait);
                if (trait_method) {
                    return m_generic_calls.analyze_type_param_method_call(expr, call_expr, get_expr, obj_type, base_type,
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

    // Concretize an unsuffixed literal argument to i32 before casting: can_cast
    // doesn't treat the polymorphic IntLiteral kind as numeric (so `u8(200)` would
    // wrongly fail), and lowering's cast path can't take an IntLiteral source. We
    // pin to i32 (its canonical default), NOT the target — so an out-of-range cast
    // like `u8(300)` still truncates via TRUNC_U rather than becoming a no-op.
    if (source_type->is_numeric_literal()) {
        source_type = default_literal_type(source_type);
        m_checker.coerce_numeric_literal(call_expr.arguments[0].expr, source_type);
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
            error_fmt(expr->loc, "constructor expects {} argument(s) but got {}",
                     ctor->param_types.size(), call_expr.arguments.size());
            return result_type();
        }

        // Check argument types and modifiers. A ConstructorInfo may carry a
        // null decl (a synthetic constructor, or an LSP-recovered one whose AST
        // never fully formed) — fall back to empty params, matching the
        // destructor path in analyze_delete_stmt and the method/free-fun paths.
        Span<Param> ctor_params = ctor->decl ? ctor->decl->constructor_decl.params
                                             : Span<Param>();
        check_call_args(call_expr.arguments, ctor->param_types, ctor_params, expr->loc);
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
            m_checker.coerce_numeric_literal(index_expr.index, method_info->param_types[0]);
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

    if (base_type->is_type_param() && m_generic_calls.has_active_bounds()) {
        // Type parameters have no fields, but may have methods via bounds
        Type* found_in_trait = nullptr;
        const TraitMethodInfo* trait_method = m_generic_calls.lookup_type_param_method(base_type, get_expr.name, &found_in_trait);
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
        // Resolve through the enum's own variant table, NOT the flat symbol
        // namespace — a same-named variant of another enum would shadow this
        // one there and make a perfectly valid Enum::Variant unresolvable.
        if (type->enum_info.find_variant(sge.member_name)) {
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
        if (target_sym && m_lifetimes.lookup_state(target_sym, saved_state) &&
            saved_state != MoveState::Live) {
            m_lifetimes.set_state(target_sym, MoveState::Live);
            restored_for_assign = true;
        }
    }

    Type* target_type = analyze_expr(assign_expr.target);

    // Restore the state so auto-delete logic in IR builder knows the old value was moved
    if (restored_for_assign) {
        Symbol* target_sym = m_symbols.lookup(assign_expr.target->identifier.name);
        if (target_sym) {
            m_lifetimes.set_state(target_sym, saved_state);
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
            m_checker.coerce_numeric_literal(assign_expr.value, target_type);
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
        m_checker.coerce_numeric_literal(assign_expr.value, target_type);
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
        m_lifetimes.mark_live(assign_expr.target->identifier.name);
    }

    // Consume noncopyable source (field-move check + mark source as moved).
    // For a plain target this is target_type->noncopyable(). For an index target
    // into a container the target type is the *borrowed* element (e.g. `ref T`
    // for List<uniq T>), which isn't noncopyable — but storing a noncopyable
    // value into the slot is still a move, so consult the container's element /
    // value type instead. Without this the moved-in value stays double-owned
    // (container slot + caller scope) and double-frees.
    bool moves_noncopyable = target_type && target_type->noncopyable();
    if (!moves_noncopyable && assign_expr.target->kind == AstKind::ExprIndex) {
        Type* container_type = assign_expr.target->index.object->resolved_type;
        if (container_type) container_type = container_type->base_type();
        Type* elem_type = nullptr;
        if (container_type && container_type->is_list()) {
            elem_type = container_type->list_info.element_type;
        } else if (container_type && container_type->is_map()) {
            elem_type = container_type->map_info.value_type;
        }
        moves_noncopyable = elem_type && elem_type->noncopyable();
    }
    if (assign_expr.op == AssignOp::Assign && moves_noncopyable) {
        m_lifetimes.consume_noncopyable(assign_expr.value, assign_expr.value->loc);
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
        // A lambda boundary crossed before reaching the enclosing struct scope
        // means this `self` must be captured into the closure's env.
        bool crossed_lambda =
            !collect_crossed_lambda_contexts(m_symbols.current_struct_scope()).empty();

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
            Expr* env_id = make_identifier_expr("__env"_sv, env_ref, expr->loc);

            expr->kind = AstKind::ExprGet;
            expr->get.object = env_id;
            expr->get.name = "__self"_sv;
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
            if (arg_type->is_error()) return m_types.error_type();
            type_arg_types.push_back(arg_type);
        }

        Span<Type*> type_args = m_allocator.alloc_span(type_arg_types);

        // Check trait bounds on type args
        const ResolvedTypeParams* bounds = m_type_env.generics().get_struct_bounds(sl.type_name);
        if (!m_generic_calls.check_type_arg_bounds(sl.type_name, type_args, bounds, expr->loc)) {
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

        auto inferred = m_generic_calls.infer_type_args_from_fields(
            template_struct_decl.type_params, template_struct_decl.fields,
            sl.fields, expr->loc);

        if (inferred.success) {
            Span<Type*> type_args = m_allocator.alloc_span(inferred.type_args);

            // Check trait bounds on inferred type args
            const ResolvedTypeParams* bounds = m_type_env.generics().get_struct_bounds(sl.type_name);
            if (!m_generic_calls.check_type_arg_bounds(sl.type_name, type_args, bounds, expr->loc)) {
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
            if (field_type && m_generic_calls.coerce_generic_template_ref(fi.value, field_type)) {
                value_type = fi.value->resolved_type;
            }
            m_checker.check_assignable(field_type, value_type, fi.loc);
            m_checker.coerce_numeric_literal(fi.value, field_type);

            // Consume noncopyable source (field-move check + mark source as moved)
            if (field_type && field_type->noncopyable()) {
                m_lifetimes.consume_noncopyable(fi.value, fi.loc);
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
            m_checker.coerce_numeric_literal(fi.value, variant_field_info->type);

            // Consume noncopyable source (field-move check + mark source as moved)
            if (variant_field_info->type && variant_field_info->type->noncopyable()) {
                m_lifetimes.consume_noncopyable(fi.value, fi.loc);
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

Type* SemanticAnalyzer::get_binary_result_type(BinaryOp op, Type* left, Type* right, SourceLocation loc) {
    // Java/C#-style numeric promotion: narrow integer types (i8/i16/u8/u16) have no
    // native arithmetic — they widen to i32 for the operation, and the result is i32.
    // Resolution then finds i32's registered operator methods. u32/u64 are NOT narrow
    // (they get native unsigned arithmetic separately and stay unsupported here);
    // string/struct/float/bool operands are never narrow, so this leaves them untouched.
    if (left && left->is_narrow_integer()) left = m_types.i32_type();
    if (right && right->is_narrow_integer()) right = m_types.i32_type();

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
                if (m_checker.require_types_match(left, right, loc, "arithmetic operator")) {
                    // Same numeric type, but no registered operator: the type
                    // has no backend support for this op (unsigned/small-int
                    // arithmetic, `%` on floats). Report it — silently
                    // returning error_type let the expression compile as
                    // Error-typed and misbehave downstream.
                    error_fmt(loc, "operator '{}' is not supported for type '{}'",
                              binary_op_to_symbol(op), m_checker.type_string(left).data());
                }
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
                if (m_checker.require_types_match(left, right, loc, "comparison operator")) {
                    error_fmt(loc, "operator '{}' is not supported for type '{}'",
                              binary_op_to_symbol(op), m_checker.type_string(left).data());
                }
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
                if (m_checker.require_types_match(left, right, loc, "bitwise operator")) {
                    error_fmt(loc, "operator '{}' is not supported for type '{}'",
                              binary_op_to_symbol(op), m_checker.type_string(left).data());
                }
                return m_types.error_type();
            }
            error(loc, "bitwise operators require integer operands");
            return m_types.error_type();
        }
    }

    return m_types.error_type();
}

Type* SemanticAnalyzer::get_unary_result_type(UnaryOp op, Type* operand, SourceLocation loc) {
    // Numeric promotion (see get_binary_result_type): a narrow integer operand widens
    // to i32 for '-' / '~', yielding an i32 result. int_literal / bool operands are not
    // narrow, so the int-literal short-circuit and '!' below are unaffected.
    if (operand && operand->is_narrow_integer()) operand = m_types.i32_type();

    switch (op) {
        case UnaryOp::Negate:
            // `-1` / `-1.0` stay polymorphic; coerce_numeric_literal recurses
            // through the unary to concretize the operand with the expression.
            if (operand->is_numeric_literal())
                return operand;
            if (Type* result = try_resolve_unary_op(op, operand))
                return result;
            if (operand->is_numeric()) {
                error_fmt(loc, "operator '-' is not supported for type '{}'",
                          m_checker.type_string(operand).data());
            } else {
                error(loc, "unary '-' requires numeric operand");
            }
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
            if (operand->is_integer()) {
                error_fmt(loc, "operator '~' is not supported for type '{}'",
                          m_checker.type_string(operand).data());
            } else {
                error(loc, "unary '~' requires integer operand");
            }
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
