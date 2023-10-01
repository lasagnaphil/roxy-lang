#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/vector.hpp"
#include "roxy/ast_visitor.hpp"
#include "roxy/token.hpp"
#include "roxy/expr.hpp"
#include "roxy/stmt.hpp"
#include "roxy/type.hpp"

namespace rx {

template <typename Derived, typename ReturnT>
class ExprVisitorBase {
public:
    ReturnT visit(Expr& expr) {
        switch (expr.type) {
            case ExprType::Error:       return static_cast<Derived*>(this)->visit_impl(expr.cast<ErrorExpr>());
            case ExprType::Assign:      return static_cast<Derived*>(this)->visit_impl(expr.cast<AssignExpr>());
            case ExprType::Binary:      return static_cast<Derived*>(this)->visit_impl(expr.cast<BinaryExpr>());
            case ExprType::Ternary:     return static_cast<Derived*>(this)->visit_impl(expr.cast<TernaryExpr>());
            case ExprType::Grouping:    return static_cast<Derived*>(this)->visit_impl(expr.cast<GroupingExpr>());
            case ExprType::Literal:     return static_cast<Derived*>(this)->visit_impl(expr.cast<LiteralExpr>());
            case ExprType::Unary:       return static_cast<Derived*>(this)->visit_impl(expr.cast<UnaryExpr>());
            case ExprType::Variable:    return static_cast<Derived*>(this)->visit_impl(expr.cast<VariableExpr>());
            case ExprType::Call:        return static_cast<Derived*>(this)->visit_impl(expr.cast<CallExpr>());
        }
    }

    // The functions you need to implement for each class derived from ExprVisitorBase!
    ReturnT visit_impl(ErrorExpr& expr)         { return ReturnT{}; }
    ReturnT visit_impl(AssignExpr& expr)        { return ReturnT{}; }
    ReturnT visit_impl(BinaryExpr& expr)        { return ReturnT{}; }
    ReturnT visit_impl(TernaryExpr& expr)       { return ReturnT{}; }
    ReturnT visit_impl(GroupingExpr& expr)      { return ReturnT{}; }
    ReturnT visit_impl(LiteralExpr& expr)       { return ReturnT{}; }
    ReturnT visit_impl(UnaryExpr& expr)         { return ReturnT{}; }
    ReturnT visit_impl(VariableExpr& expr)      { return ReturnT{}; }
    ReturnT visit_impl(CallExpr& expr)          { return ReturnT{}; }

};

template <typename Derived, typename ReturnT>
class StmtVisitorBase {
public:
    ReturnT visit(Stmt& stmt) {
        switch (stmt.type) {
            case StmtType::Error:       return static_cast<Derived*>(this)->visit_impl(stmt.cast<ErrorStmt>());
            case StmtType::Block:       return static_cast<Derived*>(this)->visit_impl(stmt.cast<BlockStmt>());
            case StmtType::Expression:  return static_cast<Derived*>(this)->visit_impl(stmt.cast<ExpressionStmt>());
            case StmtType::Struct:      return static_cast<Derived*>(this)->visit_impl(stmt.cast<StructStmt>());
            case StmtType::Function:    return static_cast<Derived*>(this)->visit_impl(stmt.cast<FunctionStmt>());
            case StmtType::If:          return static_cast<Derived*>(this)->visit_impl(stmt.cast<IfStmt>());
            case StmtType::Print:       return static_cast<Derived*>(this)->visit_impl(stmt.cast<PrintStmt>());
            case StmtType::Var:         return static_cast<Derived*>(this)->visit_impl(stmt.cast<VarStmt>());
            case StmtType::While:       return static_cast<Derived*>(this)->visit_impl(stmt.cast<WhileStmt>());
            case StmtType::Return:      return static_cast<Derived*>(this)->visit_impl(stmt.cast<ReturnStmt>());
            case StmtType::Break:       return static_cast<Derived*>(this)->visit_impl(stmt.cast<BreakStmt>());
            case StmtType::Continue:    return static_cast<Derived*>(this)->visit_impl(stmt.cast<ContinueStmt>());
        }
    }

    // The functions you need to implement for each class derived from StmtVisitorBase!
    ReturnT visit_impl(ErrorStmt& stmt)         { return ReturnT{}; }
    ReturnT visit_impl(BlockStmt& stmt)         { return ReturnT{}; }
    ReturnT visit_impl(ExpressionStmt& stmt)    { return ReturnT{}; }
    ReturnT visit_impl(StructStmt& stmt)        { return ReturnT{}; }
    ReturnT visit_impl(FunctionStmt& stmt)      { return ReturnT{}; }
    ReturnT visit_impl(IfStmt& stmt)            { return ReturnT{}; }
    ReturnT visit_impl(PrintStmt& stmt)         { return ReturnT{}; }
    ReturnT visit_impl(VarStmt& stmt)           { return ReturnT{}; }
    ReturnT visit_impl(WhileStmt& stmt)         { return ReturnT{}; }
    ReturnT visit_impl(ReturnStmt& stmt)        { return ReturnT{}; }
    ReturnT visit_impl(BreakStmt& stmt)         { return ReturnT{}; }
    ReturnT visit_impl(ContinueStmt& stmt)      { return ReturnT{}; }
};

template <typename Derived, typename ReturnT>
class TypeVisitorBase {
public:
    ReturnT visit(Type& type) {
        switch (type.kind) {
            case TypeKind::Primitive:   return static_cast<Derived*>(this)->visit_impl(type.cast<PrimitiveType>());
            case TypeKind::Struct:      return static_cast<Derived*>(this)->visit_impl(type.cast<StructType>());
            case TypeKind::Function:    return static_cast<Derived*>(this)->visit_impl(type.cast<FunctionType>());
        }
    }

    // The functions you need to implement for each class derived from TypeVisitorBase!
    ReturnT visit_impl(PrimitiveType& type)     { return ReturnT{}; }
    ReturnT visit_impl(StructType& type)        { return ReturnT{}; }
    ReturnT visit_impl(FunctionType& type)      { return ReturnT{}; }
};

}