#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/span.hpp"
#include "roxy/core/rel_ptr.hpp"
#include "roxy/token.hpp"
#include "roxy/value.hpp"

#include <cassert>

namespace rx {

enum class ExprKind : u8 {
    Error,
    Assign,
    Binary,
    Ternary,
    Grouping,
    Literal,
    Unary,
    Variable,
    Call,
    Get,
    Set
};

struct Type;

struct Expr {
#ifndef NDEBUG
    virtual ~Expr() = default;
#endif

    u32 source_loc;
    u16 length;
    ExprKind kind;
    RelPtr<Type> type;

    Expr(ExprKind kind, SourceLocation loc) :
        source_loc(loc.source_loc), length(loc.length),
        kind(kind), type(nullptr) {}

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

    SourceLocation get_source_loc() const {
        return {source_loc, length};
    }
};

struct ErrorExpr : public Expr {
    static constexpr ExprKind s_kind = ExprKind::Error;

    std::string message;

    ErrorExpr(SourceLocation loc, std::string message) : Expr(s_kind, loc), message(std::move(message)) {}
    ErrorExpr(SourceLocation loc, std::string_view message) : Expr(s_kind, loc), message(message) {}
};

struct AssignExpr : public Expr {
    static constexpr ExprKind s_kind = ExprKind::Assign;

    Token name;
    RelPtr<Expr> value;

    AssignExpr(SourceLocation loc, Token name, Expr* value) :
        Expr(s_kind, loc), name(name), value(value) {}
};

struct BinaryExpr : public Expr {
    static constexpr ExprKind s_kind = ExprKind::Binary;

    RelPtr<Expr> left;
    RelPtr<Expr> right;
    Token op;

    BinaryExpr(SourceLocation loc, Expr* left, Token op, Expr* right) :
        Expr(s_kind, loc), left(left), right(right), op(op) {}
};

struct TernaryExpr : public Expr {
    static constexpr ExprKind s_kind = ExprKind::Ternary;

    RelPtr<Expr> cond;
    RelPtr<Expr> left;
    RelPtr<Expr> right;

    TernaryExpr(SourceLocation loc, Expr* cond, Expr* left, Expr* right) :
        Expr(s_kind, loc), cond(cond), left(left), right(right) {}
};

struct GroupingExpr : public Expr {
    static constexpr ExprKind s_kind = ExprKind::Grouping;

    RelPtr<Expr> expression;

    GroupingExpr(SourceLocation loc, Expr* expression) :
            Expr(s_kind, loc),
            expression(expression) {}
};

struct LiteralExpr : public Expr {
public:
    static constexpr ExprKind s_kind = ExprKind::Literal;

    AnyValue value;

    LiteralExpr(SourceLocation loc, AnyValue value) :
            Expr(s_kind, loc),
            value(value) {}
};

struct UnaryExpr : public Expr {
public:
    static constexpr ExprKind s_kind = ExprKind::Unary;

    Token op;
    RelPtr<Expr> right;

    UnaryExpr(SourceLocation loc, Token op, Expr* right) : Expr(s_kind, loc), op(op), right(right) {}
};

struct VariableExpr : public Expr {
public:
    static constexpr ExprKind s_kind = ExprKind::Variable;

    Token name;

    VariableExpr(SourceLocation loc, Token name) : Expr(s_kind, loc), name(name) {}
};

struct CallExpr : public Expr {
public:
    static constexpr ExprKind s_kind = ExprKind::Call;

    RelPtr<Expr> callee;
    Token paren;
    RelSpan<RelPtr<Expr>> arguments;

    CallExpr(SourceLocation loc, Expr* callee, Token paren, Span<RelPtr<Expr>> arguments) :
            Expr(s_kind, loc), callee(callee), paren(paren), arguments(arguments) {}
};

struct GetExpr : public Expr {
public:
    static constexpr ExprKind s_kind = ExprKind::Get;

    RelPtr<Expr> object;
    Token name;

    GetExpr(SourceLocation loc, Expr* object, Token name) :
            Expr(s_kind, loc), object(object), name(name) {}
};

struct SetExpr : public Expr {
public:
    static constexpr ExprKind s_kind = ExprKind::Set;

    RelPtr<Expr> object;
    Token name;
    RelPtr<Expr> value;

    SetExpr(SourceLocation loc, Expr* object, Token name, Expr* value) :
            Expr(s_kind, loc), object(object), name(name), value(value) {}
};

}