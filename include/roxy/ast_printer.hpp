#pragma once

#include "roxy/expr.hpp"

#include <string>

namespace rx {

class AstPrinter {
private:
    template <typename ... ExprT>
    static std::string parenthesize(std::string_view name, ExprT&&... exprs) {
        std::string str;
        str += "(";
        str += name;
        (((str += ' ', str += to_string(std::forward<Expr>(exprs)))),...);
        str += ")";
        return str;
    }
public:
    static std::string to_string(const Expr& expr) {
        switch (expr.type) {
            case ExprType::Error: {
                return "error";
            }
            case ExprType::Binary: {
                auto& binary_expr = expr.cast<BinaryExpr>();
                return parenthesize(binary_expr.op.str(), *binary_expr.left, *binary_expr.right);
            }
            case ExprType::Grouping: {
                auto& grouping_expr = expr.cast<GroupingExpr>();
                return parenthesize("group", *grouping_expr.expression);
            }
            case ExprType::Literal: {
                auto& literal_expr = expr.cast<LiteralExpr>();
                return literal_expr.value.to_std_string();
            }
            case ExprType::Unary: {
                auto& unary_expr = expr.cast<UnaryExpr>();
                return parenthesize(unary_expr.op.str(), *unary_expr.right);
            }
        }
        return "";
    }
};

}