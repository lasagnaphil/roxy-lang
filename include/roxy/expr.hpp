#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/span.hpp"
#include "roxy/core/rel_ptr.hpp"
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


struct Expr {
#ifndef NDEBUG
    virtual ~Expr() = default;
#endif

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

struct ErrorExpr : public Expr {
    static constexpr ExprType s_type = ExprType::Error;

    ErrorExpr() : Expr(s_type) {}
};

struct AssignExpr : public Expr {
    static constexpr ExprType s_type = ExprType::Assign;

    Token name;
    RelPtr<Expr> value;

    AssignExpr(Token name, Expr* value) :
        Expr(s_type), name(name), value(value) {}
};

struct BinaryExpr : public Expr {
    static constexpr ExprType s_type = ExprType::Binary;

    RelPtr<Expr> left;
    RelPtr<Expr> right;
    Token op;

    BinaryExpr(Expr* left, Token op, Expr* right) :
        Expr(s_type), left(left), right(right), op(op) {}
};

struct TernaryExpr : public Expr {
    static constexpr ExprType s_type = ExprType::Ternary;

    RelPtr<Expr> cond;
    RelPtr<Expr> left;
    RelPtr<Expr> right;

    TernaryExpr(Expr* cond, Expr* left, Expr* right) :
        Expr(s_type), cond(cond), left(left), right(right) {}
};

struct GroupingExpr : public Expr {
    static constexpr ExprType s_type = ExprType::Grouping;

    RelPtr<Expr> expression;

    GroupingExpr(Expr* expression) :
            Expr(s_type),
            expression(expression) {}
};

struct LiteralExpr : public Expr {
public:
    static constexpr ExprType s_type = ExprType::Literal;

    AnyValue value;

    LiteralExpr(AnyValue value) :
            Expr(s_type),
            value(value) {}
};

struct UnaryExpr : public Expr {
public:
    static constexpr ExprType s_type = ExprType::Unary;

    Token op;
    RelPtr<Expr> right;

    UnaryExpr(Token op, Expr* right) : Expr(s_type), op(op), right(right) {}
};

struct VariableExpr : public Expr {
public:
    static constexpr ExprType s_type = ExprType::Variable;

    Token name;

    VariableExpr(Token name) : Expr(s_type), name(name) {}
};

struct CallExpr : public Expr {
public:
    static constexpr ExprType s_type = ExprType::Call;

    RelPtr<Expr> callee;
    Token paren;
    RelSpan<RelPtr<Expr>> arguments;

    CallExpr(Expr* callee, Token paren, Span<RelPtr<Expr>> arguments) :
            Expr(s_type), callee(callee), paren(paren), arguments(arguments) {}
};

}