#pragma once

namespace rx {

class BumpAllocator;
class TypeEnv;
class TypeCache;
class ModuleRegistry;
class SymbolTable;
class ErrorReporter;
class TypeChecker;
class SemanticAnalyzer;
struct Type;
struct TypeExpr;
struct Expr;
struct Stmt;

// Shared context for the semantic analyzer's collaborators (LifetimeChecker,
// TraitSystem, GenericCallResolver, ...): the analyzer-owned state everything
// needs, bundled so each extracted collaborator doesn't grow a
// seven-reference constructor. All members are borrowed — the analyzer (and
// its owners) outlive the collaborators, which are analyzer members
// themselves.
//
// The function pointers expose the analyzer *operations* collaborators need,
// without virtual dispatch (this codebase uses none) and without a
// back-reference to the rest of the analyzer:
// - resolve_type_expr: full TypeExpr resolution (generic instantiation,
//   builtin-container method population, active-type-param lookup) — used by
//   trait method signatures, `for Trait<Args>` args, impl method types,
//   generic type args and bounds. Never returns null (null input and every
//   failure yield error_type).
// - analyze_expr / analyze_stmt: re-entry into the expression/statement
//   walkers — used by generic type-arg inference (arguments are analyzed to
//   obtain their concrete types) and Phase B generic-template-body checking.
// The analyzer installs thunks to its private methods at construction time.
struct SemaContext {
    BumpAllocator& allocator;
    TypeEnv& type_env;
    TypeCache& types;
    ModuleRegistry& modules;
    SymbolTable& symbols;
    ErrorReporter& reporter;
    TypeChecker& checker;

    SemanticAnalyzer* analyzer;
    Type* (*resolve_type_expr_fn)(SemanticAnalyzer* analyzer, TypeExpr* type_expr);
    Type* (*analyze_expr_fn)(SemanticAnalyzer* analyzer, Expr* expr);
    void (*analyze_stmt_fn)(SemanticAnalyzer* analyzer, Stmt* stmt);

    Type* resolve_type_expr(TypeExpr* type_expr) const {
        return resolve_type_expr_fn(analyzer, type_expr);
    }
    Type* analyze_expr(Expr* expr) const {
        return analyze_expr_fn(analyzer, expr);
    }
    void analyze_stmt(Stmt* stmt) const {
        analyze_stmt_fn(analyzer, stmt);
    }
};

}
