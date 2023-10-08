#pragma once

#include "roxy/expr.hpp"
#include "roxy/token.hpp"
#include "roxy/type.hpp"

namespace rx {

enum class StmtKind {
    Error,
    Block,
    Expression,
    Struct,
    Function,
    If,
    Print,
    Var,
    While,
    Return,
    Break,
    Continue,
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

    Token name;
    RelSpan<AstVarDecl> params;
    RelSpan<RelPtr<Stmt>> body;
    RelPtr<Type> ret_type;
    RelPtr<FunctionType> function_type = nullptr; // created later in sema analyzer

    FunctionStmt(Token name, Span<AstVarDecl> params, Span<RelPtr<Stmt>> body, Type* ret_type) :
        Stmt(s_kind), name(name), params(params), body(body), ret_type(ret_type) {}
};

struct IfStmt : public Stmt {
    static constexpr StmtKind s_kind = StmtKind::If;

    RelPtr<Expr> condition;
    RelPtr<Stmt> then_branch;
    RelPtr<Stmt> else_branch; // can be null

    IfStmt(Expr* condition, Stmt* then_branch, Stmt* else_branch) :
        Stmt(s_kind), condition(condition), then_branch(then_branch), else_branch(else_branch) {}
};

struct PrintStmt : public Stmt {
    static constexpr StmtKind s_kind = StmtKind::Print;

    RelPtr<Expr> expr;

    PrintStmt(Expr* expr) : Stmt(s_kind), expr(expr) {}
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

}