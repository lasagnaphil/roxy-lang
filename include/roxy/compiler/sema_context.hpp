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

// Shared context for the semantic analyzer's collaborators (LifetimeChecker,
// TraitSystem, ...): the analyzer-owned state everything needs, bundled so
// each extracted collaborator doesn't grow a seven-reference constructor.
// All members are borrowed — the analyzer (and its owners) outlive the
// collaborators, which are analyzer members themselves.
//
// resolve_type_expr is the one analyzer *operation* collaborators need (trait
// method signatures, `for Trait<Args>` args, impl method param/return types):
// full TypeExpr resolution requires generic instantiation, builtin-container
// method population, and the active-type-param stack, all of which live on
// the analyzer. The analyzer installs a thunk to its private resolver at
// construction time (a plain function pointer — this codebase uses no virtual
// dispatch); collaborators get exactly that operation and nothing else, so
// there is still no back-reference to the rest of the analyzer.
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

    Type* resolve_type_expr(TypeExpr* type_expr) const {
        return resolve_type_expr_fn(analyzer, type_expr);
    }
};

}
