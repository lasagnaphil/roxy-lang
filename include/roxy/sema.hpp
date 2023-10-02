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
    UndefinedVar,
    WrongType,
    InvalidInitializerType,
    InvalidAssignedType,
    IncompatibleTypes,
    CannotInferType,
    Misc,
};

// TODO: better SemaResult ADT...

struct SemaResult {
    SemaResultType res_type;

    Expr* cur_expr = nullptr;
    Stmt* cur_stmt = nullptr;
    Expr* other_expr = nullptr;
    Type* expected_type = nullptr;

    static SemaResult ok() {
        return {
            .res_type = SemaResultType::Ok,
        };
    }
    static SemaResult undefined_var(Expr* expr) {
        return {
            .res_type = SemaResultType::UndefinedVar,
            .cur_expr = expr
        };
    }
    static SemaResult wrong_type(Expr* expr, Type* expected_type) {
        return {
            .res_type = SemaResultType::WrongType,
            .cur_expr = expr,
            .expected_type = expected_type
        };
    }
    static SemaResult invalid_initializer_type(Expr* initializer_expr) {
        return {
            .res_type = SemaResultType::InvalidInitializerType,
            .cur_expr = initializer_expr
        };
    }
    static SemaResult invalid_assigned_type(Stmt* var_stmt, Expr* assigned_expr) {
        return {
            .res_type = SemaResultType::InvalidAssignedType,
            .cur_expr = assigned_expr,
            .cur_stmt = var_stmt,
        };
    }
    static SemaResult incompatible_types(Expr* left, Expr* right) {
        return {
            .res_type = SemaResultType::IncompatibleTypes,
            .cur_expr = left,
            .other_expr = right,
        };
    }
    static SemaResult cannot_infer_type(Expr* expr) {
        return {
            .res_type = SemaResultType::CannotInferType,
            .cur_expr = expr
        };
    }
    static SemaResult misc(Expr* expr) {
        return {
            .res_type = SemaResultType::Misc,
            .cur_expr = expr
        };
    }

    bool is_ok() const { return res_type == SemaResultType::Ok; }

    std::string to_error_msg() const {
        switch (res_type) {
            case SemaResultType::UndefinedVar:
                return "Undefined variable.";
            case SemaResultType::WrongType:
                return "Wrong type.";
            case SemaResultType::InvalidInitializerType:
                return "Invalid initializer type.";
            case SemaResultType::InvalidAssignedType:
                return "Invalid assignment type.";
            case SemaResultType::IncompatibleTypes:
                return "Incompatible types.";
            case SemaResultType::CannotInferType:
                return "Cannot infer kind.";
            case SemaResultType::Misc:
                return "Misc.";
            default:
                return "";
        }
    }
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
        public ExprVisitorBase<SemaAnalyzer, SemaResult> {

private:
    AstAllocator* m_allocator;
    const u8* m_source;

    Vector<SemaResult> m_errors;

    SemaEnv* m_cur_env;

    void set_env(SemaEnv* env) {
        m_cur_env = env;
    }

    SemaResult report_error(const SemaResult& res) {
        m_errors.push_back(res);
        return res;
    }

public:
    using StmtVisitorBase<SemaAnalyzer, SemaResult>::visit;
    using ExprVisitorBase<SemaAnalyzer, SemaResult>::visit;

    SemaAnalyzer(AstAllocator* allocator, const u8* source) : m_allocator(allocator), m_source(source) {}

    Vector<SemaResult> check(BlockStmt* stmt) {
        visit_impl(*stmt);

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
            // Do not return on error result, since we want to scan all statements inside the block.
            auto _ = visit(*inner_stmt.get());
        }
        m_cur_env = block_env.get_outer_env();
        return {};
    }
    SemaResult visit_impl(ExpressionStmt& stmt)    {
        return visit(*stmt.expr.get());
    }
    SemaResult visit_impl(StructStmt& stmt)        {
        return {}; // TODO: add struct to env
    }
    SemaResult visit_impl(FunctionStmt& stmt)      {
        return {}; // TODO
    }
    SemaResult visit_impl(IfStmt& stmt)            {
        Expr* cond_expr = stmt.condition.get();
        SEMA_TRY(visit(*cond_expr));
        if (!cond_expr->type->is_bool()) {
            return report_error(SemaResult::wrong_type(stmt.condition.get(), m_allocator->get_bool_type()));
        }
        SEMA_TRY(visit(*stmt.then_branch.get()));
        if (auto else_branch = stmt.else_branch.get()) {
            SEMA_TRY(visit(*else_branch));
        }
        return {};
    }
    SemaResult visit_impl(PrintStmt& stmt)         {
        return visit(*stmt.expr.get());
    }
    SemaResult visit_impl(VarStmt& stmt)           {
        if (Expr* init_expr = stmt.initializer.get()) {
            SEMA_TRY(visit(*init_expr));
            if (stmt.var.type != nullptr) {
                Type* var_type = stmt.var.type.get();
                if (!is_type_compatible(var_type, init_expr->type.get())) {
                    return report_error(SemaResult::invalid_initializer_type(init_expr));
                }
            }
            else {
                // Basic local kind inference
                stmt.var.type = init_expr->type.get();
            }
        }
        else {
            assert(stmt.var.type != nullptr); // The parser filters out this case beforehand

            // insert default value
            stmt.initializer = nullptr; // TODO
        }
        m_cur_env->set_var(stmt.var.name.str(m_source), &stmt);

        return {SemaResultType::Ok, nullptr};
    }
    SemaResult visit_impl(WhileStmt& stmt)         {
        auto cond_expr = stmt.condition.get();
        SEMA_TRY(visit(*cond_expr));
        if (!cond_expr->type->is_bool()) {
            return report_error(SemaResult::wrong_type(cond_expr, m_allocator->get_bool_type()));
        }
        return visit(*stmt.body.get());
    }
    SemaResult visit_impl(ReturnStmt& stmt)        {
        // TODO: Need to look at current function scope and compare return kind
        return {};
    }
    SemaResult visit_impl(BreakStmt& stmt)         { return SemaResult::ok(); } // nothing to do
    SemaResult visit_impl(ContinueStmt& stmt)      { return SemaResult::ok(); } // nothing to do

    SemaResult visit_impl(ErrorExpr& expr)         { return SemaResult::misc(&expr); } // unreachable???
    SemaResult visit_impl(AssignExpr& expr)        {
        SEMA_TRY(visit(*expr.value.get()));
        if (VarStmt* var_stmt = m_cur_env->get_var(expr.name.str(m_source))) {
            // If cannot find kind of assigned expression, then error.
            Expr* assigned_expr = expr.value.get();
            SEMA_TRY(visit(*assigned_expr));

            // If assigned kind is not compatible with variable, then error.
            Type* var_type = var_stmt->var.type.get();
            expr.type = var_type;
            if (is_type_compatible(var_type, assigned_expr->type.get())) {
                return SemaResult::ok();
            }
            else {
                return report_error(SemaResult::invalid_assigned_type(var_stmt, assigned_expr));
            }
        }
        else {
            // If cannot get kind of assigned variable, then error.
            return SemaResult::undefined_var(&expr);
        }
    }

    SemaResult visit_impl(BinaryExpr& expr)        {
        Expr* left_expr = expr.left.get();
        Expr* right_expr = expr.right.get();
        SEMA_TRY(visit(*left_expr));
        SEMA_TRY(visit(*right_expr));

        switch (expr.op.type) {
            // The numeric operators +, -, *, / and the inequality comparison operators >, >=, <, <=
            //  can only be done on numbers
            case TokenType::Minus:
            case TokenType::Plus:
            case TokenType::Star:
            case TokenType::Slash:
            {
                expr.type = m_allocator->get_number_type();
                if (left_expr->type->is_number() && right_expr->type->is_number()) {
                    return SemaResult::ok();
                }
                else {
                    // When incompatible type error, just use the left type for the placeholder.
                    return report_error(SemaResult::incompatible_types(left_expr, right_expr));
                }
            }
            case TokenType::Greater:
            case TokenType::GreaterEqual:
            case TokenType::Less:
            case TokenType::LessEqual:
            {
                expr.type = m_allocator->get_bool_type();
                if (left_expr->type->is_number() && right_expr->type->is_number()) {
                    return SemaResult::ok();
                }
                else {
                    // When incompatible type error, just use the left type for the placeholder.
                    return report_error(SemaResult::incompatible_types(left_expr, right_expr));
                }
            }
                // The logical operators &&, || can only be done on bools
            case TokenType::AmpAmp:
            case TokenType::BarBar:
            {
                expr.type = m_allocator->get_bool_type();
                if (left_expr->type->is_bool() && right_expr->type->is_bool()) {
                    return SemaResult::ok();
                }
                else {
                    return report_error(SemaResult::incompatible_types(left_expr, right_expr));
                }
            }
                // The equality comparison operators ==, != can only be done on same types
            case TokenType::BangEqual:
            case TokenType::EqualEqual:
            {
                expr.type = m_allocator->get_bool_type();
                if (is_type_same(left_expr->type.get(), right_expr->type.get())) {
                    return SemaResult::ok();
                }
                else {
                    return report_error(SemaResult::incompatible_types(left_expr, right_expr));
                }
            }
            default:
                return SemaResult::misc(&expr);
        }
    }
    SemaResult visit_impl(TernaryExpr& expr)       {
        Expr* cond_expr = expr.cond.get();
        SEMA_TRY(visit(*cond_expr));
        if (!cond_expr->type->is_bool()) {
            auto bool_type = m_allocator->get_bool_type();
            expr.type = bool_type;
            return report_error(SemaResult::wrong_type(cond_expr, bool_type));
        }

        Expr* left_expr = expr.left.get();
        Expr* right_expr = expr.right.get();

        SEMA_TRY(visit(*left_expr));
        SEMA_TRY(visit(*right_expr));

        expr.type = left_expr->type.get();
        if (is_type_same(left_expr->type.get(), right_expr->type.get())) {
            return SemaResult::ok();
        }
        else {
            return report_error(SemaResult::incompatible_types(left_expr, right_expr));
        }
    }
    SemaResult visit_impl(GroupingExpr& expr)      {
        return visit(*expr.expression.get());
    }
    SemaResult visit_impl(LiteralExpr& expr)       {
        if (expr.value.is_bool()) {
            expr.type = m_allocator->get_bool_type();
            return SemaResult::ok();
        }
        else if (expr.value.is_number()) {
            expr.type = m_allocator->get_number_type();
            return SemaResult::ok();
        }
        else if (expr.value.is_string()) {
            expr.type = m_allocator->get_string_type();
            return SemaResult::ok();
        }
        else {
            return report_error(SemaResult::misc(&expr)); // TODO
        }
    }
    SemaResult visit_impl(UnaryExpr& expr)         {
        auto right_expr = expr.right.get();
        SEMA_TRY(visit(*right_expr));
        switch (expr.op.type) {
            case TokenType::Minus: {
                expr.type = m_allocator->get_number_type();
                if (right_expr->type->is_number()) {
                    return SemaResult::ok();
                }
                else {
                    return report_error(SemaResult::wrong_type(right_expr, expr.type.get()));
                }
            }
            case TokenType::Bang: {
                expr.type = m_allocator->get_bool_type();
                if (right_expr->type->is_bool()) {
                    return SemaResult::ok();
                }
                else {
                    return report_error(SemaResult::wrong_type(right_expr, expr.type.get()));
                }
            }
            default: {
                return SemaResult::misc(&expr); // unreachable
            }
        }
    }
    SemaResult visit_impl(VariableExpr& expr)      {
        auto var_stmt = m_cur_env->get_var(expr.name.str(m_source));
        if (var_stmt) {
            expr.type = var_stmt->var.type.get();
            return SemaResult::ok();
        }
        else {
            return report_error(SemaResult::undefined_var(&expr));
        }
    }
    SemaResult visit_impl(CallExpr& expr)          {
        return report_error(SemaResult::misc(&expr)); // TODO
    }
};

}
