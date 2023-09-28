#pragma once

#include "roxy/core/types.hpp"

namespace rx {

enum class ExprType : u32 {
    Error,
    Binary,
    Grouping,
    Literal,
    Unary
};

class Expr {
public:
    u32 source_loc;
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

class BinaryExpr : public Expr {
public:
    static constexpr ExprType s_type = ExprType::Binary;

    Expr* left;
    Expr* right;
    Token op;

    BinaryExpr(Expr* left, Token op, Expr* right) :
            Expr(s_type),
            left(left), right(right), op(op) {}
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

    Expr* right;
    Token op;

    UnaryExpr(Token op, Expr* right) :
            Expr(s_type),
            right(right), op(op) {}
};

}