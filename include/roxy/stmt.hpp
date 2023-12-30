#pragma once

#include "roxy/expr.hpp"
#include "roxy/token.hpp"
#include "roxy/type.hpp"

namespace rx {

enum class StmtKind {
    Error,
    Block,
    Module,
    Expression,
    Struct,
    Function,
    If,
    Var,
    While,
    Return,
    Break,
    Continue,
    Import,
};

struct Stmt {
#ifndef NDEBUG
    virtual ~Stmt() = default;
#endif

    StmtKind kind;

    Stmt(StmtKind kind) : kind(kind) {}

    template <typename StmtT, typename = std::enable_if_t<std::is_base_of_v<Stmt, StmtT>>>
    const StmtT& cast() const {
        assert(kind == StmtT::s_kind);
        return static_cast<const StmtT&>(*this);
    }

    template <typename StmtT, typename = std::enable_if_t<std::is_base_of_v<Stmt, StmtT>>>
    StmtT& cast() {
        assert(kind == StmtT::s_kind);
        return static_cast<StmtT&>(*this);
    }

    template <typename StmtT, typename = std::enable_if_t<std::is_base_of_v<Stmt, StmtT>>>
    const StmtT* try_cast() const {
        if (kind == StmtT::s_kind) return static_cast<const StmtT*>(this);
        else return nullptr;
    }

    template <typename StmtT, typename = std::enable_if_t<std::is_base_of_v<Stmt, StmtT>>>
    StmtT* try_cast() {
        if (kind == StmtT::s_kind) return static_cast<StmtT*>(this);
        else return nullptr;
    }
};

struct ErrorStmt : public Stmt {
    static constexpr StmtKind s_kind = StmtKind::Error;

    std::string message;

    ErrorStmt(std::string message) : Stmt(s_kind), message(std::move(message)) {}
    ErrorStmt(std::string_view message) : Stmt(s_kind), message(message) {}
};

struct BlockStmt : public Stmt {
    static constexpr StmtKind s_kind = StmtKind::Block;

    RelSpan<RelPtr<Stmt>> statements;

    BlockStmt(Span<RelPtr<Stmt>> statements) : Stmt(s_kind), statements(statements) {}
};

struct ImportStmt;

struct ModuleStmt : public Stmt {
    static constexpr StmtKind s_kind = StmtKind::Module;

    RelSpan<RelPtr<Stmt>> statements;
    RelSpan<RelPtr<AstVarDecl>> locals;
    RelSpan<RelPtr<AstFunDecl>> functions;

    RelSpan<RelPtr<AstFunDecl>> exports;
    RelSpan<RelPtr<ImportStmt>> imports;

    ModuleStmt(Span<RelPtr<Stmt>> statements)
        : Stmt(s_kind), statements(statements) {}

    void set_locals(Span<RelPtr<AstVarDecl>> locals) {
        new (&this->locals) RelSpan<RelPtr<AstVarDecl>>(locals);
    }

    void set_functions(Span<RelPtr<AstFunDecl>> functions) {
        new (&this->functions) RelSpan<RelPtr<AstFunDecl>>(functions);
    }

    void set_exports(Span<RelPtr<AstFunDecl>> exports) {
        new (&this->exports) RelSpan<RelPtr<AstFunDecl>>(exports);
    }

    void set_imports(Span<RelPtr<ImportStmt>> imports) {
        new (&this->imports) RelSpan<RelPtr<ImportStmt>>(imports);
    }
};

struct ExpressionStmt : public Stmt {
    static constexpr StmtKind s_kind = StmtKind::Expression;

    RelPtr<Expr> expr;

    ExpressionStmt(Expr* expr) : Stmt(s_kind), expr(expr) {}
};

struct StructStmt : public Stmt {
    static constexpr StmtKind s_kind = StmtKind::Struct;

    Token name;
    RelSpan<AstVarDecl> fields;
    RelPtr<Type> type = nullptr; // created later in sema analyzer

    StructStmt(Token name, Span<AstVarDecl> fields) : Stmt(s_kind), name(name), fields(fields) {}
};

struct FunctionStmt : public Stmt {
public:
    static constexpr StmtKind s_kind = StmtKind::Function;

    AstFunDecl fun_decl;
    RelSpan<RelPtr<Stmt>> body;
    bool is_public;
    bool is_native;

    // created later in sema analyzer
    RelSpan<RelPtr<AstVarDecl>> locals;

    FunctionStmt(FunDecl fun_decl, Span<RelPtr<Stmt>> body, bool is_public, bool is_native) :
        Stmt(s_kind), fun_decl(fun_decl), body(body), is_public(is_public), is_native(is_native) {
    }

    void set_locals(Span<RelPtr<AstVarDecl>> locals) {
        new (&this->locals) RelSpan<RelPtr<AstVarDecl>>(locals);
    }
};

struct IfStmt : public Stmt {
    static constexpr StmtKind s_kind = StmtKind::If;

    RelPtr<Expr> condition;
    RelPtr<Stmt> then_branch;
    RelPtr<Stmt> else_branch; // can be null

    IfStmt(Expr* condition, Stmt* then_branch, Stmt* else_branch) :
        Stmt(s_kind), condition(condition), then_branch(then_branch), else_branch(else_branch) {}
};

struct VarStmt : public Stmt {
    static constexpr StmtKind s_kind = StmtKind::Var;

    AstVarDecl var;
    RelPtr<Expr> initializer; // can be null

    VarStmt(VarDecl var, Expr* initializer) : Stmt(s_kind), var(var), initializer(initializer) {}
};

struct WhileStmt : public Stmt {
    static constexpr StmtKind s_kind = StmtKind::While;

    RelPtr<Expr> condition;
    RelPtr<Stmt> body;

    WhileStmt(Expr* condition, Stmt* body) : Stmt(s_kind), condition(condition), body(body) {}
};

struct ReturnStmt : public Stmt {
    static constexpr StmtKind s_kind = StmtKind::Return;

    RelPtr<Expr> expr; // can be null

    ReturnStmt(Expr* expr) : Stmt(s_kind), expr(expr) {}
};

struct BreakStmt : public Stmt {
    static constexpr StmtKind s_kind = StmtKind::Break;

    BreakStmt() : Stmt(s_kind) {}
};

struct ContinueStmt : public Stmt {
    static constexpr StmtKind s_kind = StmtKind::Continue;

    ContinueStmt() : Stmt(s_kind) {}
};

struct ImportStmt : public Stmt {
    static constexpr StmtKind s_kind = StmtKind::Import;

    RelSpan<Token> package_path;
    RelSpan<Token> import_symbols;

    bool is_wildcard() const {
        return import_symbols.size() == 1 && import_symbols[0].type == TokenType::Star;
    }

    ImportStmt(Span<Token> package_path, Span<Token> import_symbols) :
            Stmt(s_kind), package_path(package_path), import_symbols(import_symbols) {}
};

}