#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/span.hpp"
#include "roxy/core/string_view.hpp"
#include "roxy/core/vector.hpp"
#include "roxy/core/bump_allocator.hpp"
#include "roxy/shared/token.hpp"
#include "roxy/compiler/types.hpp"

#include "roxy/core/tsl/robin_map.h"

namespace rx {

// Forward declarations
struct Decl;

enum class SymbolKind : u8 {
    Variable,
    Parameter,
    Function,
    Struct,
    Enum,
    Field,
    EnumVariant,
    Module,              // Imported module namespace
    ImportedFunction,    // Function imported from another module
};

// A symbol represents a named entity in the program
struct Symbol {
    SymbolKind kind;
    StringView name;
    Type* type;
    SourceLocation loc;
    Decl* decl;           // AST node that declared this symbol (may be null for built-ins)
    bool is_pub;          // Public visibility

    Symbol()
        : kind(SymbolKind::Variable)
        , name(nullptr, 0)  // Explicitly initialize StringView
        , type(nullptr)
        , loc{0, 0, 0}
        , decl(nullptr)
        , is_pub(false)
    {
        // Zero-initialize the union
        param.index = 0;
    }

    // Kind-specific data
    union {
        struct {
            u32 index;    // Parameter index in function signature
        } param;

        struct {
            u32 index;    // Field index in struct layout
        } field;

        struct {
            i64 value;    // Enum variant value
        } enum_variant;

        struct {
            void* module_info;  // ModuleInfo* (avoid circular include)
        } module;

        struct {
            StringView module_name;  // Source module name
            StringView original_name;  // Original function name in the module
            u32 native_index;        // Index in module's native_functions
            bool is_native;          // True if from native module
        } imported_func;
    };
};

// Scope types for semantic analysis
enum class ScopeKind : u8 {
    Global,       // Top-level scope
    Function,     // Function body scope
    Block,        // Block scope (if, while, for, etc.)
    Loop,         // Loop scope (while, for) - for break/continue validation
    Struct,       // Struct scope - for 'this' validation
};

// A scope contains symbols and tracks context
struct Scope {
    ScopeKind kind;
    Scope* parent;
    Vector<Symbol*> symbols;

    // Scope-specific data
    union {
        struct {
            Type* return_type;    // Expected return type
            bool has_return;      // Whether all paths return
        } function;

        struct {
            Type* struct_type;    // The struct type for 'this'
        } struct_scope;
    };
};

// Symbol table manages scopes and symbol lookup
class SymbolTable {
public:
    explicit SymbolTable(BumpAllocator& allocator);

    // Scope management
    void push_scope(ScopeKind kind);
    void push_function_scope(Type* return_type);
    void push_loop_scope();
    void push_struct_scope(Type* struct_type);
    void pop_scope();

    // Symbol definition
    Symbol* define(SymbolKind kind, StringView name, Type* type, SourceLocation loc, Decl* decl = nullptr);
    Symbol* define_parameter(StringView name, Type* type, SourceLocation loc, u32 index);
    Symbol* define_field(StringView name, Type* type, SourceLocation loc, u32 index, bool is_pub);
    Symbol* define_enum_variant(StringView name, Type* type, SourceLocation loc, i64 value);
    Symbol* define_module(StringView name, void* module_info, SourceLocation loc);
    Symbol* define_imported_function(StringView name, Type* type, SourceLocation loc,
                                     StringView module_name, StringView original_name,
                                     u32 native_index, bool is_native);

    // Symbol lookup
    Symbol* lookup(StringView name) const;           // Look up in all scopes
    Symbol* lookup_local(StringView name) const;     // Look up in current scope only

    // Scope queries
    bool is_in_loop() const;
    bool is_in_function() const;
    bool is_in_struct() const;
    Type* current_return_type() const;
    Type* current_struct_type() const;
    Scope* current_scope() const { return m_current; }
    Scope* global_scope() const { return m_global; }

    // Mark current function as having a return statement
    void mark_return();

private:
    BumpAllocator& m_allocator;
    Scope* m_global;
    Scope* m_current;

    // Fast lookup map for current scope chain
    tsl::robin_map<StringView, Symbol*, StringViewHash, StringViewEqual> m_lookup_cache;

    Scope* create_scope(ScopeKind kind);
    void rebuild_lookup_cache();
};

}
