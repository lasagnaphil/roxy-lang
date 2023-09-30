#pragma once

#include "roxy/core/types.hpp"
#include "roxy/token.hpp"
#include "roxy/value.hpp"

#include <cassert>

namespace rx {

enum class ExprType : u32 {
    Error,
    Assign,
    Binary,
    Ternary,
    Grouping,
    Literal,
    Unary,
    Variable,
    Call,
};

class Expr {
public:
    ExprType type;

    Expr(ExprType type) : type(type) {}

    template <typename ExprT, typename = std::enable_if_t<std::is_base_of_v<Expr, ExprT>>>
    const ExprT& cast() const {
        assert(type == ExprT::s_type);
        return static_cast<const ExprT&>(*this);
    }

    template <typename ExprT, typename = std::enable_if_t<std::is_base_of_v<Expr, ExprT>>>
    ExprT& cast() {
        assert(type == ExprT::s_type);
        return static_cast<ExprT&>(*this);
    }

    template <typename ExprT, typename = std::enable_if_t<std::is_base_of_v<Expr, ExprT>>>
    const ExprT* try_cast() const {
        if (type == ExprT::s_type) return static_cast<const ExprT*>(this);
        else return nullptr;
    }

    template <typename ExprT, typename = std::enable_if_t<std::is_base_of_v<Expr, ExprT>>>
    ExprT* try_cast() {
        if (type == ExprT::s_type) return static_cast<ExprT*>(this);
        else return nullptr;
    }
};

class ErrorExpr : public Expr {
public:
    static constexpr ExprType s_type = ExprType::Error;

    ErrorExpr() : Expr(s_type) {}
};

class AssignExpr : public Expr {
public:
    static constexpr ExprType s_type = ExprType::Assign;

    Token name;
    Expr* value;

    AssignExpr(Token name, Expr* value) :
        Expr(s_type), name(name), value(value) {}
};

class BinaryExpr : public Expr {
public:
    static constexpr ExprType s_type = ExprType::Binary;

    Expr* left;
    Expr* right;
    Token op;

    BinaryExpr(Expr* left, Token op, Expr* right) :
        Expr(s_type), left(left), right(right), op(op) {}
};

class TernaryExpr : public Expr {
public:
    static constexpr ExprType s_type = ExprType::Ternary;

    Expr* cond;
    Expr* left;
    Expr* right;

    TernaryExpr(Expr* cond, Expr* left, Expr* right) :
        Expr(s_type), cond(cond), left(left), right(right) {}
};

class GroupingExpr : public Expr {
public:
    static constexpr ExprType s_type = ExprType::Grouping;

    Expr* expression;

    GroupingExpr(Expr* expression) :
            Expr(s_type),
            expression(expression) {}
};

class LiteralExpr : public Expr {
public:
    static constexpr ExprType s_type = ExprType::Literal;

    Value value;

    LiteralExpr(Value value) :
            Expr(s_type),
            value(value) {}
};

class UnaryExpr : public Expr {
public:
    static constexpr ExprType s_type = ExprType::Unary;

    Token op;
    Expr* right;

    UnaryExpr(Token op, Expr* right) : Expr(s_type), op(op), right(right) {}
};

class VariableExpr : public Expr {
public:
    static constexpr ExprType s_type = ExprType::Variable;

    Token name;

    VariableExpr(Token name) : Expr(s_type), name(name) {}
};

class CallExpr : public Expr {
public:
    static constexpr ExprType s_type = ExprType::Call;

    Expr* callee;
    Token paren;
    Vector<Expr*> arguments;

    CallExpr(Expr* callee, Token paren, Vector<Expr*>&& arguments) :
            Expr(s_type), callee(callee), paren(paren), arguments(std::move(arguments)) {}
};

}