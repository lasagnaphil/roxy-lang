#pragma once

#include "roxy/core/vector.hpp"
#include "roxy/core/bump_allocator.hpp"
#include "roxy/tsl/robin_map.h"
#include "roxy/ast_visitor.hpp"
#include "roxy/ast_allocator.hpp"

namespace rx {

class SemaEnv {
private:
    SemaEnv* m_outer;
    tsl::robin_map<std::string, VarStmt*> m_type_map;

public:
    SemaEnv(SemaEnv* outer) : m_outer(outer) {}

    SemaEnv* get_outer_env() { return m_outer; }

    VarStmt* get_var(std::string_view name) {
        // TODO: Use a better hash map that doesn't need the std::string allocation
        auto it = m_type_map.find(std::string(name));
        if (it != m_type_map.end()) {
            return it->second;
        }
        else {
            if (m_outer) {
                return m_outer->get_var(name);
            }
            else {
                return nullptr;
            }
        }
    }

    void set_var(std::string_view name, VarStmt* stmt) {
        // TODO: Use a better hash map that doesn't need the std::string allocation
        m_type_map.insert({std::string(name), stmt});
    }
};

enum class SemaResultType {
    Ok,
    UndefinedVariable,
    IncompatibleTypes,
    CannotInferType,
    Misc,
};

// TODO: better SemaResult ADT...

struct SemaResult {
    SemaResultType res_type;
    Expr* expr;

    SemaResult() : res_type(SemaResultType::Ok), expr(nullptr) {}
    SemaResult(SemaResultType res_type, Expr* expr) : res_type(res_type), expr(expr) {}

    static SemaResult ok() { return {}; }

    bool is_ok() const { return res_type == SemaResultType::Ok; }
};

struct TypeInferResult {
    SemaResult sema_res;
    Type* type;

    TypeInferResult(Type* type) : sema_res(), type(type) {}
    TypeInferResult(SemaResultType res_type, Expr* expr) : sema_res(res_type, expr), type(nullptr) {}

    bool is_ok() const { return sema_res.is_ok(); }
};

class AstAllocator;

inline bool is_type_same(Type* lhs, Type* rhs) {
    if (lhs == rhs) return true;
    if (lhs->kind != rhs->kind) return false;
    switch (lhs->kind) {
        case TypeKind::Primitive:
            return lhs->cast<PrimitiveType>().prim_kind == rhs->cast<PrimitiveType>().prim_kind;
        default:
            return false; // TODO
    }
}

inline bool is_type_compatible(Type* lhs, Type* rhs) {
    return is_type_same(lhs, rhs);
}

class SemaAnalyzer :
        public StmtVisitorBase<SemaAnalyzer, SemaResult>,
        public ExprVisitorBase<SemaAnalyzer, TypeInferResult> {

private:
    AstAllocator* m_allocator;
    Vector<SemaResult> m_errors;

    SemaEnv* m_cur_env;

    void set_env(SemaEnv* env) {
        m_cur_env = env;
    }

public:
    using StmtVisitorBase<SemaAnalyzer, SemaResult>::visit;
    using ExprVisitorBase<SemaAnalyzer, TypeInferResult>::visit;

    SemaAnalyzer(AstAllocator* allocator) : m_allocator(allocator) {}

    Vector<SemaResult> check(BlockStmt* stmt) {
        SemaEnv top_env(nullptr);
        m_cur_env = &top_env;

        for (auto& stmt_ptr : stmt->statements) {
            Stmt* stmt = stmt_ptr.get();
            auto res = visit(*stmt);
            if (!res.is_ok()) {
                m_errors.push_back(res);
            }
        }

        m_cur_env = nullptr;

        auto errors = m_errors;
        m_errors.clear();
        return errors;
    }

#define SEMA_TRY(EXPR) do { auto _res = EXPR; if (!_res.is_ok()) return _res; } while (false)

    SemaResult visit_impl(ErrorStmt& stmt)         { return {}; } // unreachable
    SemaResult visit_impl(BlockStmt& stmt)         {
        SemaEnv block_env(m_cur_env);
        m_cur_env = &block_env;
        for (RelPtr<Stmt>& inner_stmt : stmt.statements) {
            SEMA_TRY(visit(*inner_stmt.get()));
        }
        m_cur_env = block_env.get_outer_env();
        return {};
    }
    SemaResult visit_impl(ExpressionStmt& stmt)    {
        TypeInferResult res = visit(*stmt.expr.get());
        if (!res.is_ok()) return res.sema_res;
        return {};
    }
    SemaResult visit_impl(StructStmt& stmt)        {
        return {}; // TODO: add struct to env
    }
    SemaResult visit_impl(FunctionStmt& stmt)      {
        return {}; // TODO
    }
    SemaResult visit_impl(IfStmt& stmt)            {
        TypeInferResult res = visit(*stmt.condition.get());
        if (!res.is_ok()) return res.sema_res;
        if (!res.type->is_bool()) {
            return {SemaResultType::IncompatibleTypes, stmt.condition.get()};
        }
        SEMA_TRY(visit(*stmt.then_branch.get()));
        if (auto else_branch = stmt.else_branch.get()) {
            SEMA_TRY(visit(*else_branch));
        }
        return {};
    }
    SemaResult visit_impl(PrintStmt& stmt)         {
        TypeInferResult res = visit(*stmt.expr.get());
        if (!res.is_ok()) return res.sema_res;
        return {};
    }
    SemaResult visit_impl(VarStmt& stmt)           {
        if (Expr* init_expr = stmt.initializer.get()) {
            TypeInferResult res = visit(*init_expr);
            if (!res.is_ok()) return res.sema_res;
            if (stmt.var.type != nullptr) {
                if (!is_type_compatible(stmt.var.type.get(), res.type)) {
                    return {SemaResultType::IncompatibleTypes, stmt.initializer.get()};
                }
            }
            else {
                // Basic local type inference
                stmt.var.type = res.type;
            }
        }
        else {
            // insert default value
            if (stmt.var.type != nullptr) {
                stmt.initializer = nullptr; // TODO
            }
            else {
                // TODO: need to return statement as part of error...
                return {SemaResultType::CannotInferType, nullptr};
            }
        }
        m_cur_env->set_var(stmt.var.name.str(), &stmt);

        return {SemaResultType::Ok, nullptr};
    }
    SemaResult visit_impl(WhileStmt& stmt)         {
        auto cond_expr = stmt.condition.get();
        TypeInferResult res = visit(*cond_expr);
        if (!res.is_ok()) return res.sema_res;
        if (res.type->is_bool()) {
            return {SemaResultType::IncompatibleTypes, cond_expr};
        }
        return visit(*stmt.body.get());
    }
    SemaResult visit_impl(ReturnStmt& stmt)        {
        // TODO: Need to look at current function scope and compare return type
        return {};
    }
    SemaResult visit_impl(BreakStmt& stmt)         { return SemaResult{}; } // nothing to do
    SemaResult visit_impl(ContinueStmt& stmt)      { return SemaResult{}; } // nothing to do

    TypeInferResult visit_impl(ErrorExpr& expr)         { return {SemaResultType::Misc, &expr}; } // unreachable???
    TypeInferResult visit_impl(AssignExpr& expr)        {
        TypeInferResult res = visit(*expr.value.get());
        Type* type;
        if (VarStmt* var_stmt = m_cur_env->get_var(expr.name.str())) {
            // If cannot find type of assigned expression, then error.
            TypeInferResult assigned_res = visit(*expr.value.get());
            if (!assigned_res.is_ok()) return res;

            // If assigned type is not compatible with variable, then error.
            Type* var_type = var_stmt->var.type.get();
            if (!is_type_compatible(var_type, assigned_res.type)) {
                return {SemaResultType::IncompatibleTypes, &expr};
            }

            type = var_stmt->var.type.get();
        }
        else {
            // If cannot get type of assigned variable, then error.
            return {SemaResultType::UndefinedVariable, &expr};
        }
        return type;
    }

    TypeInferResult visit_impl(BinaryExpr& expr)        {
        TypeInferResult left_res = visit(*expr.left.get());
        if (!left_res.is_ok()) return left_res;

        TypeInferResult right_res = visit(*expr.right.get());
        if (!right_res.is_ok()) return right_res;

        switch (expr.op.type) {
            // The numeric operators +, -, *, / and the inequality comparison operators >, >=, <, <=
            //  can only be done on numbers
            case TokenType::Minus:
            case TokenType::Plus:
            case TokenType::Star:
            case TokenType::Slash:
            case TokenType::Greater:
            case TokenType::GreaterEqual:
            case TokenType::Less:
            case TokenType::LessEqual:
            {
                if (left_res.type->is_number() && right_res.type->is_number()) {
                    return left_res.type;
                }
                else {
                    return {SemaResultType::IncompatibleTypes, &expr};
                }
            }
                // The logical operators &&, || can only be done on bools
            case TokenType::AmpAmp:
            case TokenType::BarBar:
            {
                if (left_res.type->is_bool() && right_res.type->is_bool()) {
                    return left_res.type;
                }
                else {
                    return {SemaResultType::IncompatibleTypes, &expr};
                }
            }
                // The equality comparison operators ==, != can only be done on same types
            case TokenType::BangEqual:
            case TokenType::EqualEqual:
            {
                if (is_type_same(left_res.type, right_res.type)) {
                    return left_res.type;
                }
                else {
                    return {SemaResultType::IncompatibleTypes, &expr};
                }
            }
            default:
                return {SemaResultType::Misc, &expr}; // unreachable
        }
    }
    TypeInferResult visit_impl(TernaryExpr& expr)       {
        TypeInferResult cond_res = visit(*expr.cond.get());
        if (!cond_res.is_ok()) return cond_res;
        if (!cond_res.type->is_bool()) {
            return {SemaResultType::IncompatibleTypes, &expr};
        }

        TypeInferResult left_res = visit(*expr.left.get());
        SEMA_TRY(left_res);

        TypeInferResult right_res = visit(*expr.right.get());
        SEMA_TRY(right_res);

        if (is_type_same(left_res.type, right_res.type)) {
            return left_res.type;
        }
        else {
            return {SemaResultType::IncompatibleTypes, &expr};
        }
    }
    TypeInferResult visit_impl(GroupingExpr& expr)      {
        return visit(*expr.expression.get());
    }
    TypeInferResult visit_impl(LiteralExpr& expr)       {
        if (expr.value.is_bool()) {
            return m_allocator->alloc<PrimitiveType>(PrimTypeKind::Bool);
        }
        else if (expr.value.is_number()) {
            return m_allocator->alloc<PrimitiveType>(PrimTypeKind::Number);
        }
        else if (expr.value.is_string()) {
            return m_allocator->alloc<PrimitiveType>(PrimTypeKind::String);
        }
        else {
            return {SemaResultType::Misc, &expr}; // TODO
        }
    }
    TypeInferResult visit_impl(UnaryExpr& expr)         {
        auto res = visit(*expr.right.get());
        SEMA_TRY(res);
        switch (expr.op.type) {
            case TokenType::Minus: {
                if (res.type->is_number()) {
                    return m_allocator->alloc<PrimitiveType>(PrimTypeKind::Bool);
                }
                else {
                    return {SemaResultType::IncompatibleTypes, &expr};
                }
            }
            case TokenType::Bang: {
                if (res.type->is_bool()) {
                    return m_allocator->alloc<PrimitiveType>(PrimTypeKind::Bool);
                }
                else {
                    return {SemaResultType::IncompatibleTypes, &expr};
                }
            }
            default: {
                return {SemaResultType::Misc, &expr}; // unreachable
            }
        }
    }
    TypeInferResult visit_impl(VariableExpr& expr)      {
        auto var_stmt = m_cur_env->get_var(expr.name.str());
        if (var_stmt) {
            Type* type = var_stmt->var.type.get();
            if (type) return type;
            else return {SemaResultType::CannotInferType, &expr};
        }
        else {
            return {SemaResultType::UndefinedVariable, &expr};
        }
    }
    TypeInferResult visit_impl(CallExpr& expr)          {
        return {SemaResultType::Misc, &expr}; // TODO
    }

private:
    SemaResult check(Stmt* stmt, SemaEnv* env);
    SemaResult get_type(Expr* expr, SemaEnv* env, Type*& type);
};

}
