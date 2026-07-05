#include "roxy/compiler/symbol_table.hpp"

namespace rx {

SymbolTable::SymbolTable(BumpAllocator& allocator)
    : m_allocator(allocator)
    , m_global(nullptr)
    , m_current(nullptr)
{
    // Create global scope
    m_global = create_scope(ScopeKind::Global);
    m_current = m_global;
}

Scope* SymbolTable::create_scope(ScopeKind kind) {
    Scope* scope = m_allocator.emplace<Scope>();
    scope->kind = kind;
    scope->parent = nullptr;
    scope->symbols = Vector<Symbol*>();
    return scope;
}

void SymbolTable::push_scope(ScopeKind kind) {
    Scope* scope = create_scope(kind);
    scope->parent = m_current;
    m_current = scope;
}

void SymbolTable::push_function_scope(Type* return_type) {
    Scope* scope = create_scope(ScopeKind::Function);
    scope->parent = m_current;
    scope->function.return_type = return_type;
    m_current = scope;
}

void SymbolTable::push_loop_scope() {
    Scope* scope = create_scope(ScopeKind::Loop);
    scope->parent = m_current;
    m_current = scope;
}

void SymbolTable::push_struct_scope(Type* struct_type) {
    Scope* scope = create_scope(ScopeKind::Struct);
    scope->parent = m_current;
    scope->struct_scope.struct_type = struct_type;
    m_current = scope;
}

void SymbolTable::pop_scope() {
    if (m_current && m_current->parent) {
        // Undo this scope's cache entries in reverse definition order: each
        // symbol restores the entry it displaced (an outer symbol, an earlier
        // same-scope definition, or nothing). Reverse order makes same-name
        // redefinitions within one scope unwind correctly. O(symbols in this
        // scope) — replacing the old full-chain cache rebuild that made every
        // block exit cost O(total visible symbols).
        Vector<Symbol*>& symbols = m_current->symbols;
        for (u32 i = symbols.size(); i > 0; i--) {
            Symbol* sym = symbols[i - 1];
            if (sym->shadowed) {
                m_lookup_cache[sym->name] = sym->shadowed;
            } else {
                m_lookup_cache.erase(sym->name);
            }
        }
        m_current = m_current->parent;
    }
}

Symbol* SymbolTable::define(SymbolKind kind, StringView name, Type* type, SourceLocation loc, Decl* decl) {
    Symbol* sym = m_allocator.emplace<Symbol>();
    sym->kind = kind;
    sym->name = name;
    sym->type = type;
    sym->loc = loc;
    sym->decl = decl;
    sym->is_pub = false;
    sym->defining_scope = m_current;

    m_current->symbols.push_back(sym);
    // Record the displaced cache entry so pop_scope can restore it.
    auto it = m_lookup_cache.find(name);
    sym->shadowed = (it != m_lookup_cache.end()) ? it->second : nullptr;
    m_lookup_cache[name] = sym;
    return sym;
}

Symbol* SymbolTable::define_parameter(StringView name, Type* type, SourceLocation loc, u32 index,
                                     bool is_out_inout) {
    Symbol* sym = define(SymbolKind::Parameter, name, type, loc, nullptr);
    sym->param.index = index;
    sym->is_out_inout = is_out_inout;
    return sym;
}

Symbol* SymbolTable::define_field(StringView name, Type* type, SourceLocation loc, u32 index, bool is_pub) {
    Symbol* sym = define(SymbolKind::Field, name, type, loc, nullptr);
    sym->field.index = index;
    sym->is_pub = is_pub;
    return sym;
}

Symbol* SymbolTable::define_enum_variant(StringView name, Type* type, SourceLocation loc, i64 value) {
    Symbol* sym = define(SymbolKind::EnumVariant, name, type, loc, nullptr);
    sym->enum_variant.value = value;
    return sym;
}

Symbol* SymbolTable::define_module(StringView name, void* module_info, SourceLocation loc) {
    Symbol* sym = define(SymbolKind::Module, name, nullptr, loc, nullptr);
    sym->module.module_info = module_info;
    return sym;
}

Symbol* SymbolTable::define_imported_function(StringView name, Type* type, SourceLocation loc,
                                              StringView module_name, StringView original_name,
                                              u32 native_index, bool is_native) {
    Symbol* sym = define(SymbolKind::ImportedFunction, name, type, loc, nullptr);
    sym->imported_func.module_name = module_name;
    sym->imported_func.original_name = original_name;
    sym->imported_func.native_index = native_index;
    sym->imported_func.is_native = is_native;
    return sym;
}

Symbol* SymbolTable::lookup(StringView name) const {
    auto it = m_lookup_cache.find(name);
    if (it != m_lookup_cache.end()) {
        return it->second;
    }
    return nullptr;
}

Symbol* SymbolTable::lookup_local(StringView name) const {
    // The cache maps every visible name to its innermost symbol, so a local
    // definition — if one exists — is exactly the cached entry (nothing inside
    // the current scope can displace it except a same-scope redefinition,
    // which is also local). O(1), replacing a linear scan of the current scope
    // that made the per-export prelude import quadratic on the global scope.
    auto it = m_lookup_cache.find(name);
    if (it != m_lookup_cache.end() && it->second->defining_scope == m_current) {
        return it->second;
    }
    return nullptr;
}

Symbol* SymbolTable::lookup_function_local(StringView name) const {
    Symbol* sym = lookup(name);
    if (!sym) return nullptr;
    if (sym->kind != SymbolKind::Variable && sym->kind != SymbolKind::Parameter) {
        return nullptr;
    }
    // Walk the scope chain of the function currently being analyzed. The walk
    // stops at the Global scope (module-level names are shadowable) and after
    // the outermost Function scope — but a Function scope parented by a Lambda
    // boundary scope belongs to a lambda, and the shadowing ban crosses lambda
    // boundaries (like C#), so the walk continues through it.
    for (Scope* scope = m_current; scope; scope = scope->parent) {
        if (scope->kind == ScopeKind::Global) break;
        if (scope == sym->defining_scope) return sym;
        if (scope->kind == ScopeKind::Function &&
            (!scope->parent || scope->parent->kind != ScopeKind::Lambda)) {
            break;
        }
    }
    return nullptr;
}

bool SymbolTable::is_in_loop() const {
    for (Scope* s = m_current; s; s = s->parent) {
        if (s->kind == ScopeKind::Loop) {
            return true;
        }
    }
    return false;
}

bool SymbolTable::is_in_function() const {
    for (Scope* s = m_current; s; s = s->parent) {
        if (s->kind == ScopeKind::Function) {
            return true;
        }
    }
    return false;
}

bool SymbolTable::is_in_struct() const {
    for (Scope* s = m_current; s; s = s->parent) {
        if (s->kind == ScopeKind::Struct) {
            return true;
        }
    }
    return false;
}

Type* SymbolTable::current_return_type() const {
    for (Scope* s = m_current; s; s = s->parent) {
        if (s->kind == ScopeKind::Function) {
            return s->function.return_type;
        }
    }
    return nullptr;
}

Type* SymbolTable::current_struct_type() const {
    for (Scope* s = m_current; s; s = s->parent) {
        if (s->kind == ScopeKind::Struct) {
            return s->struct_scope.struct_type;
        }
    }
    return nullptr;
}

Scope* SymbolTable::current_struct_scope() const {
    for (Scope* s = m_current; s; s = s->parent) {
        if (s->kind == ScopeKind::Struct) {
            return s;
        }
    }
    return nullptr;
}

}
