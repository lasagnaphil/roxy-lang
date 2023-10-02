#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/span.hpp"
#include "roxy/core/rel_ptr.hpp"
#include "roxy/token.hpp"
#include "roxy/value.hpp"

#include <cassert>

namespace rx {

enum class ExprKind : u32 {
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

struct Type;

struct Expr {
#ifndef NDEBUG
    virtual ~Expr() = default;
#endif

    ExprKind kind;
    RelPtr<Type> type;

    Expr(ExprKind kind) : kind(kind), type(nullptr) {}

    template <typename ExprT, typename = std::enable_if_t<std::is_base_of_v<Expr, ExprT>>>
    const ExprT& cast() const {
        assert(kind == ExprT::s_kind);
        return static_cast<const ExprT&>(*this);
    }

    template <typename ExprT, typename = std::enable_if_t<std::is_base_of_v<Expr, ExprT>>>
    ExprT& cast() {
        assert(kind == ExprT::s_kind);
        return static_cast<ExprT&>(*this);
    }

    template <typename ExprT, typename = std::enable_if_t<std::is_base_of_v<Expr, ExprT>>>
    const ExprT* try_cast() const {
        if (kind == ExprT::s_kind) return static_cast<const ExprT*>(this);
        else return nullptr;
    }

    template <typename ExprT, typename = std::enable_if_t<std::is_base_of_v<Expr, ExprT>>>
    ExprT* try_cast() {
        if (kind == ExprT::s_kind) return static_cast<ExprT*>(this);
        else return nullptr;
    }
};

struct ErrorExpr : public Expr {
    static constexpr ExprKind s_kind = ExprKind::Error;

    ErrorExpr() : Expr(s_kind) {}
};

struct AssignExpr : public Expr {
    static constexpr ExprKind s_kind = ExprKind::Assign;

    Token name;
    RelPtr<Expr> value;

    AssignExpr(Token name, Expr* value) :
        Expr(s_kind), name(name), value(value) {}
};

struct BinaryExpr : public Expr {
    static constexpr ExprKind s_kind = ExprKind::Binary;

    RelPtr<Expr> left;
    RelPtr<Expr> right;
    Token op;

    BinaryExpr(Expr* left, Token op, Expr* right) :
        Expr(s_kind), left(left), right(right), op(op) {}
};

struct TernaryExpr : public Expr {
    static constexpr ExprKind s_kind = ExprKind::Ternary;

    RelPtr<Expr> cond;
    RelPtr<Expr> left;
    RelPtr<Expr> right;

    TernaryExpr(Expr* cond, Expr* left, Expr* right) :
        Expr(s_kind), cond(cond), left(left), right(right) {}
};

struct GroupingExpr : public Expr {
    static constexpr ExprKind s_kind = ExprKind::Grouping;

    RelPtr<Expr> expression;

    GroupingExpr(Expr* expression) :
            Expr(s_kind),
            expression(expression) {}
};

struct LiteralExpr : public Expr {
public:
    static constexpr ExprKind s_kind = ExprKind::Literal;

    AnyValue value;

    LiteralExpr(AnyValue value) :
            Expr(s_kind),
            value(value) {}
};

struct UnaryExpr : public Expr {
public:
    static constexpr ExprKind s_kind = ExprKind::Unary;

    Token op;
    RelPtr<Expr> right;

    UnaryExpr(Token op, Expr* right) : Expr(s_kind), op(op), right(right) {}
};

struct VariableExpr : public Expr {
public:
    static constexpr ExprKind s_kind = ExprKind::Variable;

    Token name;

    VariableExpr(Token name) : Expr(s_kind), name(name) {}
};

struct CallExpr : public Expr {
public:
    static constexpr ExprKind s_kind = ExprKind::Call;

    RelPtr<Expr> callee;
    Token paren;
    RelSpan<RelPtr<Expr>> arguments;

    CallExpr(Expr* callee, Token paren, Span<RelPtr<Expr>> arguments) :
            Expr(s_kind), callee(callee), paren(paren), arguments(arguments) {}
};

}