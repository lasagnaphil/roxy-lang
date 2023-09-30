#pragma once

#include "roxy/expr.hpp"
#include "roxy/token.hpp"
#include "roxy/type.hpp"

namespace rx {

enum class StmtType {
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

class Stmt {
public:
    StmtType type;

    Stmt(StmtType type) : type(type) {}

    template <typename StmtT, typename = std::enable_if_t<std::is_base_of_v<Stmt, StmtT>>>
    const StmtT& cast() const {
        assert(type == StmtT::s_type);
        return static_cast<const StmtT&>(*this);
    }

    template <typename StmtT, typename = std::enable_if_t<std::is_base_of_v<Stmt, StmtT>>>
    StmtT& cast() {
        assert(type == StmtT::s_type);
        return static_cast<StmtT&>(*this);
    }

    template <typename StmtT, typename = std::enable_if_t<std::is_base_of_v<Stmt, StmtT>>>
    const StmtT* try_cast() const {
        if (type == StmtT::s_type) return static_cast<const StmtT*>(this);
        else return nullptr;
    }

    template <typename StmtT, typename = std::enable_if_t<std::is_base_of_v<Stmt, StmtT>>>
    StmtT* try_cast() {
        if (type == StmtT::s_type) return static_cast<StmtT*>(this);
        else return nullptr;
    }
};

class ErrorStmt : public Stmt {
public:
    static constexpr StmtType s_type = StmtType::Error;

    ErrorStmt() : Stmt(s_type) {}
};

class BlockStmt : public Stmt {
public:
    static constexpr StmtType s_type = StmtType::Block;

    Vector<Stmt*> statements;

    BlockStmt(Vector<Stmt*>&& statements) : Stmt(s_type), statements(statements) {}
};

class ExpressionStmt : public Stmt {
public:
    static constexpr StmtType s_type = StmtType::Expression;

    Expr* expr;

    ExpressionStmt(Expr* expr) : Stmt(s_type), expr(expr) {}
};

class StructStmt : public Stmt {
public:
    static constexpr StmtType s_type = StmtType::Struct;

    Token name;
    Vector<VarDecl> fields;

    StructStmt(Token name, Vector<VarDecl>&& fields) : Stmt(s_type), name(name), fields(std::move(fields)) {}
};

class FunctionStmt : public Stmt {
public:
    static constexpr StmtType s_type = StmtType::Function;

    Token name;
    Vector<VarDecl> params;
    Vector<Stmt*> body;

    FunctionStmt(Token name, Vector<VarDecl>&& params, Vector<Stmt*>&& body) :
        Stmt(s_type), name(name), params(std::move(params)), body(std::move(body)) {}
};

class IfStmt : public Stmt {
public:
    static constexpr StmtType s_type = StmtType::If;

    Expr* condition;
    Stmt* then_branch;
    Stmt* else_branch; // can be null

    IfStmt(Expr* condition, Stmt* then_branch, Stmt* else_branch) :
        Stmt(s_type), condition(condition), then_branch(then_branch), else_branch(else_branch) {}
};

class PrintStmt : public Stmt {
public:
    static constexpr StmtType s_type = StmtType::Print;

    Expr* expr;

    PrintStmt(Expr* expr) : Stmt(s_type), expr(expr) {}
};

class VarStmt : public Stmt {
public:
    static constexpr StmtType s_type = StmtType::Var;

    VarDecl var;
    Expr* initializer; // can be null

    VarStmt(VarDecl var, Expr* initializer) : Stmt(s_type), var(var), initializer(initializer) {}
};

class WhileStmt : public Stmt {
public:
    static constexpr StmtType s_type = StmtType::While;

    Expr* condition;
    Stmt* body;

    WhileStmt(Expr* condition, Stmt* body) : Stmt(s_type), condition(condition), body(body) {}
};

class ReturnStmt : public Stmt {
public:
    static constexpr StmtType s_type = StmtType::Return;

    Expr* expr; // can be null

    ReturnStmt(Expr* expr) : Stmt(s_type), expr(expr) {}
};

class BreakStmt : public Stmt {
public:
    static constexpr StmtType s_type = StmtType::Break;

    BreakStmt() : Stmt(s_type) {}
};

class ContinueStmt : public Stmt {
public:
    static constexpr StmtType s_type = StmtType::Continue;

    ContinueStmt() : Stmt(s_type) {}
};

}