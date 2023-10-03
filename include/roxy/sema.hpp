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
    FunctionStmt* m_function;
    tsl::robin_map<std::string_view, AstVarDecl*> m_var_map;
    tsl::robin_map<std::string_view, FunctionStmt*> m_function_map;
    tsl::robin_map<std::string_view, StructStmt*> m_struct_map;

public:
    SemaEnv(SemaEnv* outer) : m_outer(outer), m_function(m_outer? m_outer->m_function : nullptr) {}
    SemaEnv(SemaEnv* outer, FunctionStmt* function) : m_outer(outer), m_function(function) {}

    SemaEnv* get_outer_env() { return m_outer; }
    FunctionStmt* get_outer_function() { return m_function; }

    AstVarDecl* get_var(std::string_view name) {
        auto it = m_var_map.find(name);
        if (it != m_var_map.end()) {
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

    bool set_var(std::string_view name, AstVarDecl* var_decl) {
        auto [it, inserted] = m_var_map.insert({name, var_decl});
        return inserted;
    }

    FunctionStmt* get_function(std::string_view name) {
        auto it = m_function_map.find(name);
        if (it != m_function_map.end()) {
            return it->second;
        }
        else {
            if (m_outer) {
                return m_outer->get_function(name);
            }
            else {
                return nullptr;
            }
        }
    }

    bool set_function(std::string_view name, FunctionStmt* fun_stmt) {
        auto [it, inserted] = m_function_map.insert({name, fun_stmt});
        return inserted;
    }

    StructStmt* get_struct(std::string_view name) {
        auto it = m_struct_map.find(name);
        if (it != m_struct_map.end()) {
            return it->second;
        }
        else {
            if (m_outer) {
                return m_outer->get_struct(name);
            }
            else {
                return nullptr;
            }
        }
    }

    bool set_struct(std::string_view name, StructStmt* struct_stmt) {
        auto [it, inserted] = m_struct_map.insert({name, struct_stmt});
        return inserted;
    }
};

enum class SemaResultType {
    Ok,
    UndefinedVar,
    WrongType,
    InvalidInitializerType,
    InvalidAssignedType,
    InvalidReturnType,
    UncallableType,
    IncompatibleTypes,
    CannotInferType,
    CannotFindType,
    Misc,
};

// TODO: better SemaResult ADT...

struct SemaResult {
    SemaResultType res_type = SemaResultType::Ok;

    Expr* cur_expr = nullptr;
    AstVarDecl* cur_var_decl = nullptr;
    Expr* other_expr = nullptr;
    Type* expected_type = nullptr;

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
            case SemaResultType::InvalidReturnType:
                return "Invalid return type.";
            case SemaResultType::IncompatibleTypes:
                return "Incompatible types.";
            case SemaResultType::CannotInferType:
                return "Cannot infer type.";
            case SemaResultType::CannotFindType:
                return "Cannot find type.";
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

    SemaEnv* m_cur_env = nullptr;

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

    SemaResult visit_impl(ErrorStmt& stmt)         { return ok(); } // unreachable
    SemaResult visit_impl(BlockStmt& stmt)         {
        SemaEnv block_env(m_cur_env);
        m_cur_env = &block_env;
        for (RelPtr<Stmt>& inner_stmt : stmt.statements) {
            // Do not return on error result, since we want to scan all statements inside the block.
            auto _ = visit(*inner_stmt.get());
        }
        m_cur_env = block_env.get_outer_env();
        return ok();
    }
    SemaResult visit_impl(ExpressionStmt& stmt)    {
        return visit(*stmt.expr.get());
    }
    SemaResult visit_impl(StructStmt& stmt)        {
        auto fields = stmt.fields.to_span();
        auto struct_type = m_allocator->alloc<StructType>(stmt.name, fields);

        // Calculate size and alignment of newly made struct type
        u16 size = 0;
        u16 alignment = 0;
        for (auto& field : fields) {
            auto field_type = field.type.get();
            auto unassigned_type = field_type->try_cast<UnassignedType>();
            if (unassigned_type) {
                auto field_type_name = unassigned_type->name.str(m_source);
                auto found_struct= m_cur_env->get_struct(field_type_name);
                if (found_struct) {
                    field.type = found_struct->type.get();
                }
                else {
                    auto _ = error_cannot_find_type(&field);
                    continue;
                }
            }
            u32 aligned_size = (size + field_type->alignment - 1) & ~(field_type->alignment - 1);
            size = aligned_size + field_type->size;
            alignment = field_type->alignment > alignment? field_type->alignment : alignment;
        }
        struct_type->size = size;
        struct_type->alignment = alignment;

        stmt.type = struct_type;

        m_cur_env->set_struct(stmt.name.str(m_source), &stmt);

        return ok();
    }
    SemaResult visit_impl(FunctionStmt& stmt)      {
        m_cur_env->set_function(stmt.name.str(m_source), &stmt);

        SemaEnv fn_env(m_cur_env, &stmt);
        m_cur_env = &fn_env;

        // Add all function parameters to the scope.
        Vector<Type*> param_types;
        for (auto& param : stmt.params) {
            // Assign type to each param
            auto param_name = param.name.str(m_source);
            auto unassigned_type = param.type->try_cast<UnassignedType>();
            if (unassigned_type) {
                auto param_type_name = unassigned_type->name.str(m_source);
                auto struct_stmt = m_cur_env->get_struct(param_type_name);
                if (struct_stmt) {
                    param.type = struct_stmt->type.get();
                }
                else {
                    auto _ = error_cannot_find_type(&param);
                }
            }
            param_types.push_back(param.type.get());
            m_cur_env->set_var(param_name, &param);
        }

        // Add the created function to the TypeEnv.
        stmt.function_type = m_allocator->alloc<FunctionType>(
                m_allocator->alloc_vector<RelPtr<Type>, Type*>(std::move(param_types)),
                stmt.ret_type.get());

        // Do a sema pass on the statements inside the body.
        for (auto& body_stmt : stmt.body) {
            // Do not return on error result, since we want to scan all statements.
            auto _ = visit(*body_stmt);
        }

        m_cur_env = fn_env.get_outer_env();
        return ok();
    }
    SemaResult visit_impl(IfStmt& stmt)            {
        Expr* cond_expr = stmt.condition.get();
        SEMA_TRY(visit(*cond_expr));
        if (!cond_expr->type->is_bool()) {
            return error_wrong_type(stmt.condition.get(), m_allocator->get_bool_type());
        }
        SEMA_TRY(visit(*stmt.then_branch.get()));
        if (auto else_branch = stmt.else_branch.get()) {
            SEMA_TRY(visit(*else_branch));
        }
        return ok();
    }
    SemaResult visit_impl(PrintStmt& stmt)         {
        return visit(*stmt.expr.get());
    }
    SemaResult visit_impl(VarStmt& stmt)           {
        if (Expr* init_expr = stmt.initializer.get()) {
            SEMA_TRY(visit(*init_expr));
            auto unassigned_type = stmt.var.type->try_cast<UnassignedType>();
            if (unassigned_type) {
                // Basic local kind inference
                stmt.var.type = init_expr->type.get();
            }
            else {
                Type* var_type = stmt.var.type.get();
                if (!is_type_compatible(var_type, init_expr->type.get())) {
                    return error_invalid_initializer_type(init_expr);
                }
            }
        }
        else {
            assert(stmt.var.type != nullptr); // The parser filters out this case beforehand

            auto unassigned_type = stmt.var.type->try_cast<UnassignedType>();
            if (unassigned_type) {
                auto var_type_name = unassigned_type->name.str(m_source);
                auto struct_stmt = m_cur_env->get_struct(var_type_name);
                stmt.var.type = struct_stmt->type.get();
            }

            // insert default value
            stmt.initializer = nullptr; // TODO
        }
        m_cur_env->set_var(stmt.var.name.str(m_source), &stmt.var);

        return {SemaResultType::Ok, nullptr};
    }
    SemaResult visit_impl(WhileStmt& stmt)         {
        auto cond_expr = stmt.condition.get();
        SEMA_TRY(visit(*cond_expr));
        if (!cond_expr->type->is_bool()) {
            return error_wrong_type(cond_expr, m_allocator->get_bool_type());
        }
        return visit(*stmt.body.get());
    }
    SemaResult visit_impl(ReturnStmt& stmt)        {
        auto return_expr = stmt.expr.get();
        SEMA_TRY(visit(*return_expr));
        Type* return_type = return_expr->type.get();
        Type* fn_return_type = m_cur_env->get_outer_function()->ret_type.get();
        if (fn_return_type) {
            if (!is_type_same(return_type, fn_return_type)) {
                return error_invalid_return_type(return_expr, fn_return_type);
            }
        }
        else {
            m_cur_env->get_outer_function()->ret_type = return_type;
        }
        return ok();
    }
    SemaResult visit_impl(BreakStmt& stmt)         { return ok(); } // nothing to do
    SemaResult visit_impl(ContinueStmt& stmt)      { return ok(); } // nothing to do

    SemaResult visit_impl(ErrorExpr& expr)         { return error_misc(&expr); } // unreachable???
    SemaResult visit_impl(AssignExpr& expr)        {
        SEMA_TRY(visit(*expr.value.get()));
        if (auto var_decl = m_cur_env->get_var(expr.name.str(m_source))) {
            // If cannot find kind of assigned expression, then error.
            Expr* assigned_expr = expr.value.get();
            SEMA_TRY(visit(*assigned_expr));

            // If assigned kind is not compatible with variable, then error.
            Type* var_type = var_decl->type.get();
            expr.type = var_type;
            if (is_type_compatible(var_type, assigned_expr->type.get())) {
                return ok();
            }
            else {
                return error_invalid_assigned_type(var_decl, assigned_expr);
            }
        }
        else {
            // If cannot get kind of assigned variable, then error.
            return error_undefined_var(&expr);
        }
    }

    SemaResult visit_impl(BinaryExpr& expr)        {
        Expr* left_expr = expr.left.get();
        Expr* right_expr = expr.right.get();
        SEMA_TRY(visit(*left_expr));
        SEMA_TRY(visit(*right_expr));

        switch (expr.op.type) {
            // The numeric operators +, -, *, / and the inequality comparison operators >, >=, <, <=
            //  can only be done on same types of numbers
            case TokenType::Minus:
            case TokenType::Plus:
            case TokenType::Star:
            case TokenType::Slash:
            {
                expr.type = left_expr->type.get();
                if (is_type_same(left_expr->type.get(), right_expr->type.get())) {
                    return ok();
                }
                else {
                    // When incompatible type error, just use the left type for the placeholder.
                    return error_incompatible_types(left_expr, right_expr);
                }
            }
            case TokenType::Greater:
            case TokenType::GreaterEqual:
            case TokenType::Less:
            case TokenType::LessEqual:
            {
                expr.type = m_allocator->get_bool_type();
                if (left_expr->type->is_number() && right_expr->type->is_number()) {
                    return ok();
                }
                else {
                    // When incompatible type error, just use the left type for the placeholder.
                    return error_incompatible_types(left_expr, right_expr);
                }
            }
                // The logical operators &&, || can only be done on bools
            case TokenType::AmpAmp:
            case TokenType::BarBar:
            {
                expr.type = m_allocator->get_bool_type();
                if (left_expr->type->is_bool() && right_expr->type->is_bool()) {
                    return ok();
                }
                else {
                    return error_incompatible_types(left_expr, right_expr);
                }
            }
                // The equality comparison operators ==, != can only be done on same types
            case TokenType::BangEqual:
            case TokenType::EqualEqual:
            {
                expr.type = m_allocator->get_bool_type();
                if (is_type_same(left_expr->type.get(), right_expr->type.get())) {
                    return ok();
                }
                else {
                    return error_incompatible_types(left_expr, right_expr);
                }
            }
            default:
                return error_misc(&expr);
        }
    }
    SemaResult visit_impl(TernaryExpr& expr)       {
        Expr* cond_expr = expr.cond.get();
        SEMA_TRY(visit(*cond_expr));
        if (!cond_expr->type->is_bool()) {
            auto bool_type = m_allocator->get_bool_type();
            expr.type = bool_type;
            return error_wrong_type(cond_expr, bool_type);
        }

        Expr* left_expr = expr.left.get();
        Expr* right_expr = expr.right.get();

        SEMA_TRY(visit(*left_expr));
        SEMA_TRY(visit(*right_expr));

        expr.type = left_expr->type.get();
        if (is_type_same(left_expr->type.get(), right_expr->type.get())) {
            return ok();
        }
        else {
            return error_incompatible_types(left_expr, right_expr);
        }
    }
    SemaResult visit_impl(GroupingExpr& expr)      {
        return visit(*expr.expression.get());
    }
    SemaResult visit_impl(LiteralExpr& expr)       {
        expr.type = m_allocator->alloc<PrimitiveType>(expr.value.kind);
        return ok();
    }
    SemaResult visit_impl(UnaryExpr& expr)         {
        auto right_expr = expr.right.get();
        SEMA_TRY(visit(*right_expr));
        switch (expr.op.type) {
            case TokenType::Minus: {
                if (right_expr->type->is_number()) {
                    expr.type = right_expr->type.get();
                    return ok();
                }
                else {
                    expr.type = m_allocator->alloc<PrimitiveType>(PrimTypeKind::I32); // Just pick any integer
                    return error_wrong_type(right_expr, expr.type.get());
                }
            }
            case TokenType::Bang: {
                expr.type = m_allocator->get_bool_type();
                if (right_expr->type->is_bool()) {
                    return ok();
                }
                else {
                    return error_wrong_type(right_expr, expr.type.get());
                }
            }
            default: {
                return error_misc(&expr); // unreachable
            }
        }
    }
    SemaResult visit_impl(VariableExpr& expr)      {
        auto expr_name = expr.name.str(m_source);
        auto var_stmt = m_cur_env->get_var(expr_name);
        if (var_stmt) {
            expr.type = var_stmt->type.get();
            return ok();
        }
        auto function_stmt = m_cur_env->get_function(expr_name);
        if (function_stmt) {
            expr.type = function_stmt->function_type.get();
            return ok();
        }
        return error_undefined_var(&expr);
    }
    SemaResult visit_impl(CallExpr& expr)          {
        auto callee_expr = expr.callee.get();
        SEMA_TRY(visit(*callee_expr));
        auto function_type = callee_expr->type->try_cast<FunctionType>();
        if (function_type) {
            expr.type = function_type->ret.get();
            for (u32 i = 0; i < expr.arguments.size(); i++) {
                auto arg_expr = expr.arguments[i].get();
                SEMA_TRY(visit(*arg_expr));

                auto fun_param_type = function_type->params[i].get();
                if (!is_type_compatible(fun_param_type, arg_expr->type.get())) {
                    return error_wrong_type(arg_expr, fun_param_type);
                }
            }
            return ok();
        }
        else {
            return error_uncallable_type(callee_expr);
        }
    }

    SemaResult ok() {
        return {
                .res_type = SemaResultType::Ok,
        };
    }
    SemaResult error_undefined_var(Expr* expr) {
        return report_error({
                .res_type = SemaResultType::UndefinedVar,
                .cur_expr = expr
        });
    }
    SemaResult error_wrong_type(Expr* expr, Type* expected_type) {
        return report_error({
                .res_type = SemaResultType::WrongType,
                .cur_expr = expr,
                .expected_type = expected_type
        });
    }
    SemaResult error_invalid_initializer_type(Expr* initializer_expr) {
        return report_error({
                .res_type = SemaResultType::InvalidInitializerType,
                .cur_expr = initializer_expr
        });
    }
    SemaResult error_invalid_assigned_type(AstVarDecl* var_decl, Expr* assigned_expr) {
        return report_error({
                .res_type = SemaResultType::InvalidAssignedType,
                .cur_expr = assigned_expr,
                .cur_var_decl = var_decl,
        });
    }
    SemaResult error_invalid_return_type(Expr* return_expr, Type* expected_type) {
        return report_error({
                .res_type = SemaResultType::InvalidReturnType,
                .cur_expr = return_expr,
                .expected_type = expected_type
        });
    }
    SemaResult error_uncallable_type(Expr* uncallable_expr) {
        return report_error({
                .res_type = SemaResultType::UncallableType,
                .cur_expr = uncallable_expr
        });
    }
    SemaResult error_incompatible_types(Expr* left, Expr* right) {
        return report_error({
                .res_type = SemaResultType::IncompatibleTypes,
                .cur_expr = left,
                .other_expr = right,
        });
    }
    SemaResult error_cannot_infer_type(Expr* expr) {
        return report_error({
                .res_type = SemaResultType::CannotInferType,
                .cur_expr = expr
        });
    }
    SemaResult error_cannot_find_type(AstVarDecl* var_decl) {
        return report_error({
                .res_type = SemaResultType::CannotFindType,
                .cur_var_decl = var_decl
        });
    }
    SemaResult error_misc(Expr* expr) {
        return report_error({
                .res_type = SemaResultType::Misc,
                .cur_expr = expr
        });
    }

};

}
