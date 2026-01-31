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
    scope->function.has_return = false;
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
        // Remove symbols from lookup cache
        for (Symbol* sym : m_current->symbols) {
            m_lookup_cache.erase(sym->name);
        }
        m_current = m_current->parent;
        // Rebuild cache to ensure shadowed symbols are accessible again
        rebuild_lookup_cache();
    }
}

void SymbolTable::rebuild_lookup_cache() {
    m_lookup_cache.clear();
    // Walk from global to current, so inner scopes shadow outer
    Vector<Scope*> scopes;
    for (Scope* s = m_current; s; s = s->parent) {
        scopes.push_back(s);
    }
    // Iterate in reverse (global first)
    for (i32 i = static_cast<i32>(scopes.size()) - 1; i >= 0; i--) {
        for (Symbol* sym : scopes[i]->symbols) {
            m_lookup_cache[sym->name] = sym;
        }
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

    m_current->symbols.push_back(sym);
    m_lookup_cache[name] = sym;
    return sym;
}

Symbol* SymbolTable::define_parameter(StringView name, Type* type, SourceLocation loc, u32 index) {
    Symbol* sym = define(SymbolKind::Parameter, name, type, loc, nullptr);
    sym->param.index = index;
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
    for (Symbol* sym : m_current->symbols) {
        if (sym->name == name) {
            return sym;
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

void SymbolTable::mark_return() {
    for (Scope* s = m_current; s; s = s->parent) {
        if (s->kind == ScopeKind::Function) {
            s->function.has_return = true;
            return;
        }
    }
}

}
