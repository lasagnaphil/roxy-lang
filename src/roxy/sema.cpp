#include "roxy/sema.hpp"
#include "roxy/ast_allocator.hpp"

namespace rx {

static bool is_type_same(Type* lhs, Type* rhs) {
    if (lhs == rhs) return true;
    if (lhs->kind != rhs->kind) return false;
    switch (lhs->kind) {
        case TypeKind::Primitive:
            return lhs->cast<PrimitiveType>().prim_kind == rhs->cast<PrimitiveType>().prim_kind;
        default:
            return false; // TODO
    }
}

static bool is_type_compatible(Type* lhs, Type* rhs) {
    return is_type_same(lhs, rhs);
}

Vector<SemaResult> SemaAnalyzer::check(BlockStmt* stmt) {
    SemaEnv top_env(nullptr);

    for (Stmt* stmt : stmt->statements) {
        auto res = check(stmt, &top_env);
        if (!res.is_ok()) m_errors.push_back(res);
    }

    auto errors = m_errors;
    m_errors.clear();
    return errors;
}

SemaResult SemaAnalyzer::check(Stmt* stmt, SemaEnv* env) {
    switch (stmt->type) {
        case StmtType::Block: {
            auto& block_stmt = stmt->cast<BlockStmt>();
            SemaEnv block_env(env);
            for (Stmt* inner_stmt : block_stmt.statements) {
                auto res = check(inner_stmt, &block_env);
                if (!res.is_ok()) return res;
            }
            return {SemaResultType::Ok, nullptr};
        }
        case StmtType::Expression: {
            auto& expr_stmt = stmt->cast<ExpressionStmt>();
            Type* type;
            auto res = get_type(expr_stmt.expr, env,type);
            if (!res.is_ok()) return res;
            return {SemaResultType::Ok, nullptr};
        }
        case StmtType::Struct: {
            // TODO
            return {SemaResultType::Ok, nullptr};
        }
        case StmtType::Function: {
            // TODO
            return {SemaResultType::Ok, nullptr};
        }
        case StmtType::If: {
            // TODO
            return {SemaResultType::Ok, nullptr};
        }
        case StmtType::Print: {
            return {SemaResultType::Ok, nullptr};
        }
        case StmtType::Var: {
            auto& var_stmt = stmt->cast<VarStmt>();
            Type* init_type;
            if (var_stmt.initializer) {
                auto res = get_type(var_stmt.initializer, env, init_type);
                if (!res.is_ok()) return res;
                if (var_stmt.var.type != nullptr) {
                    if (!is_type_compatible(var_stmt.var.type, init_type)) {
                        return {SemaResultType::IncompatibleTypes, var_stmt.initializer};
                    }
                }
                else {
                    // Basic local type inference
                    var_stmt.var.type = init_type;
                }
            }
            else {
                // insert default value
                if (var_stmt.var.type != nullptr) {
                    var_stmt.initializer = nullptr; // TODO
                }
                else {
                    return {SemaResultType::CannotInferType, nullptr};
                }
            }
            env->set_var(var_stmt.var.name.str(), var_stmt.var.type);

            return {SemaResultType::Ok, nullptr};
        }
        case StmtType::While: {
            auto& while_stmt = stmt->cast<WhileStmt>();
            Type* cond_type;
            auto res = get_type(while_stmt.condition, env, cond_type);
            if (!res.is_ok()) return res;
            if (!cond_type->is_bool()) {
                return {SemaResultType::IncompatibleTypes, nullptr};
            }
            return check(while_stmt.body, env);
        }
        case StmtType::Return: {
            auto& return_stmt = stmt->cast<ReturnStmt>();
            // TODO: Need to look at current function scope and compare return type
            return {SemaResultType::Ok, nullptr};
        }
        default: // none
            return {SemaResultType::Ok, nullptr};
    }
}

SemaResult SemaAnalyzer::get_type(Expr* expr, SemaEnv* env, Type*& type) {
    switch (expr->type) {
        case ExprType::Assign: {
            auto& assign_expr = expr->cast<AssignExpr>();
            Type* var_type;
            if (env->get_var_type(assign_expr.name.str(), var_type)) {
                // If cannot find type of assigned expression, then error.
                Type* assigned_type;
                auto res = get_type(assign_expr.value, env, assigned_type);
                if (!res.is_ok()) return res;

                // If assigned type is not compatible with variable, then error.
                if (!is_type_compatible(var_type, assigned_type)) {
                    return {SemaResultType::IncompatibleTypes, expr};
                }
            }
            else {
                // If cannot get type of assigned variable, then error.
                return {SemaResultType::UndefinedVariable, expr};
            }
            return {SemaResultType::Ok, expr};
        }
        case ExprType::Binary: {
            auto& binary_expr = expr->cast<BinaryExpr>();
            SemaResult res;

            Type* lhs_type;
            res = get_type(binary_expr.left, env, lhs_type);
            if (!res.is_ok()) return res;

            Type* rhs_type;
            res = get_type(binary_expr.right, env, rhs_type);
            if (!res.is_ok()) return res;

            switch (binary_expr.op.type) {
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
                    if (lhs_type->is_number() && rhs_type->is_number()) {
                        type = lhs_type;
                        return {SemaResultType::Ok, nullptr};
                    }
                    else {
                        return {SemaResultType::IncompatibleTypes, expr};
                    }
                }
                // The logical operators &&, || can only be done on bools
                case TokenType::AmpAmp:
                case TokenType::BarBar:
                {
                    if (lhs_type->is_bool() && rhs_type->is_bool()) {
                        type = lhs_type;
                        return {SemaResultType::Ok, nullptr};
                    }
                    else {
                        return {SemaResultType::IncompatibleTypes, expr};
                    }
                }
                // The equality comparison operators ==, != can only be done on same types
                case TokenType::BangEqual:
                case TokenType::EqualEqual:
                {
                    if (is_type_same(lhs_type, rhs_type)) {
                        type = lhs_type;
                    }
                    else {
                        return {SemaResultType::IncompatibleTypes, expr};
                    }
                }
                default:
                    return {SemaResultType::Misc, expr}; // unreachable
            }
        }
        case ExprType::Ternary: {
            auto& ternary_expr = expr->cast<TernaryExpr>();
            SemaResult res;

            Type* res_type;
            res = get_type(ternary_expr.cond, env, res_type);
            if (!res.is_ok()) return res;
            if (!res_type->is_bool()) {
                return {SemaResultType::IncompatibleTypes, expr};
            }

            Type* left_type;
            res = get_type(ternary_expr.left, env, left_type);
            if (!res.is_ok()) return res;

            Type* right_type;
            res = get_type(ternary_expr.right, env, right_type);
            if (!res.is_ok()) return res;

            if (is_type_same(left_type, right_type)) {
                type = left_type;
                return {SemaResultType::Ok, nullptr};
            }
            else {
                return {SemaResultType::IncompatibleTypes, expr};
            }
        }
        case ExprType::Grouping: {
            auto& grouping_expr = expr->cast<GroupingExpr>();
            return get_type(grouping_expr.expression, env, type);
        }
        case ExprType::Literal: {
            auto& literal_expr = expr->cast<LiteralExpr>();
            if (literal_expr.value.is_bool()) {
                type = m_allocator->alloc<PrimitiveType>(PrimTypeKind::Bool);
            }
            else if (literal_expr.value.is_number()) {
                type = m_allocator->alloc<PrimitiveType>(PrimTypeKind::Number);
            }
            else if (literal_expr.value.is_string()) {
                type = m_allocator->alloc<PrimitiveType>(PrimTypeKind::String);
            }
            else {
                return {SemaResultType::Misc, expr}; // unreachable
            }
            return {SemaResultType::Ok, nullptr};
        }
        case ExprType::Unary: {
            auto& unary_expr = expr->cast<UnaryExpr>();
            Type* rhs_type;
            auto res = get_type(unary_expr.right, env, rhs_type);
            if (!res.is_ok()) return res;
            switch (unary_expr.op.type) {
                case TokenType::Minus: {
                    if (rhs_type->is_number()) {
                        type = m_allocator->alloc<PrimitiveType>(PrimTypeKind::Bool);
                        return {SemaResultType::Ok, nullptr};
                    }
                    else {
                        return {SemaResultType::IncompatibleTypes, expr};
                    }
                }
                case TokenType::Bang: {
                    if (rhs_type->is_bool()) {
                        type = m_allocator->alloc<PrimitiveType>(PrimTypeKind::Bool);
                        return {SemaResultType::Ok, nullptr};
                    }
                    else {
                        return {SemaResultType::IncompatibleTypes, expr};
                    }
                }
                default: {
                    return {SemaResultType::Misc, expr}; // unreachable
                }
            }
        }
        case ExprType::Variable: {
            auto& var_expr = expr->cast<VariableExpr>();
            if (env->get_var_type(var_expr.name.str(), type) && type != nullptr) {
                return {SemaResultType::Ok, nullptr};
            }
            else {
                return {SemaResultType::UndefinedVariable, expr};
            }
        }
        case ExprType::Call: {
            auto& call_expr = expr->cast<CallExpr>();
            // TODO
            return {SemaResultType::Ok, nullptr};
        }
        default: // none
            return {SemaResultType::Ok, nullptr};
    }
}

}