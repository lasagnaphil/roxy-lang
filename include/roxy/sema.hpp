#pragma once

#include "roxy/core/vector.hpp"
#include "roxy/core/bump_allocator.hpp"
#include "roxy/tsl/robin_map.h"
#include "roxy/ast_allocator.hpp"
#include "roxy/ast_visitor.hpp"
#include "roxy/ast_printer.hpp"

#include "roxy/fmt/format.h"

namespace rx {

using ImportMap = tsl::robin_map<std::string_view, AstFunDecl*>;

class SemaEnv {
private:
    SemaEnv* m_outer;

    FunctionStmt* m_function;

    tsl::robin_map<std::string_view, AstVarDecl*> m_var_map;
    tsl::robin_map<std::string_view, AstFunDecl*> m_function_map;
    tsl::robin_map<std::string_view, StructStmt*> m_struct_map;

    Vector<AstVarDecl*> m_locals;
    Vector<AstFunDecl*> m_functions;
    Vector<AstFunDecl*> m_native_functions;

    SemaEnv* m_closest_fun_env; // closest parent function
    SemaEnv* m_closest_module_env; // closest parent module

public:
    // Inside an ordinary scope
    SemaEnv(SemaEnv* outer)
        : m_outer(outer), m_function(m_outer->m_function) {
        SemaEnv* env = m_outer;
        while (!env->is_function_scope()) {
            env = env->m_outer;
        }
        m_closest_fun_env = env;
        while (!env->is_module_scope()) {
            env = env->m_outer;
        }
        m_closest_module_env = env;
    }

    // Inside a function
    SemaEnv(SemaEnv* outer, FunctionStmt* function) :
        m_outer(outer), m_function(function), m_closest_fun_env(this), m_closest_module_env(nullptr) {
        SemaEnv* env = m_outer;
        while (!env->is_module_scope()) {
            env = env->m_outer;
        }
        m_closest_module_env = env;
    }

    // Inside a module
    SemaEnv(SemaEnv* outer, ModuleStmt* module) :
        m_outer(outer), m_function(nullptr), m_closest_fun_env(this), m_closest_module_env(this) {}

    bool is_function_scope() const { return m_closest_fun_env == this; }
    bool is_module_scope() const { return m_closest_module_env == this; }

    SemaEnv* get_outer_env() const { return m_outer; }
    FunctionStmt* get_outer_function() const { return m_function; }
    Vector<AstVarDecl*> get_locals() const { return m_locals; }

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
        if (inserted) {
            var_decl->local_index = m_closest_fun_env->m_locals.size();
            m_closest_fun_env->m_locals.push_back(var_decl);
        }
        return inserted;
    }

    AstFunDecl* get_function(std::string_view name) {
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

    bool set_function(std::string_view name, AstFunDecl* fun_decl) {
        auto [it, inserted] = m_function_map.insert({name, fun_decl});
        if (inserted) {
            if (fun_decl->is_native) {
                fun_decl->local_index = m_closest_module_env->m_native_functions.size();
                m_closest_module_env->m_native_functions.push_back(fun_decl);
            }
            else {
                fun_decl->local_index = m_closest_module_env->m_functions.size();
                m_closest_module_env->m_functions.push_back(fun_decl);
            }
        }
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
    InvalidParamType,
    InvalidReturnType,
    UncallableType,
    IncompatibleTypes,
    CannotInferType,
    CannotFindType,
    CannotDotAccessOnType,
    CannotCallOnType,
    CannotFindField,
    IncompatibleFieldType,
    InvalidNativeFunDeclLocation,
    InvalidImportDeclaration,
    Misc,
};

// TODO: better SemaResult ADT...

struct ErrorMessage {
    SourceLocation loc;
    std::string message;
};

struct SemaResult {
    SemaResultType res_type = SemaResultType::Ok;

    Expr* cur_expr = nullptr;
    Stmt* cur_stmt = nullptr;
    Token cur_name = {};
    AstVarDecl* cur_var_decl = nullptr;
    AstFunDecl* cur_fun_decl = nullptr;
    Expr* other_expr = nullptr;
    Type* expected_type = nullptr;

    bool is_ok() const { return res_type == SemaResultType::Ok; }

    ErrorMessage to_error_msg(const u8* source) const {
        switch (res_type) {
        case SemaResultType::UndefinedVar: return {
                .loc = cur_expr->get_source_loc(),
                .message = "Undefined variable."
            };
        case SemaResultType::WrongType: return {
                .loc = cur_expr->get_source_loc(),
                .message = fmt::format("Wrong type: expected {}.",
                                       AstPrinter(source).to_string(*expected_type))
            };
        case SemaResultType::InvalidInitializerType: return {
                .loc = cur_expr->get_source_loc(),
                .message = fmt::format("Invalid initializer type: expected {}.",
                                       AstPrinter(source).to_string(*expected_type))
            };
        case SemaResultType::InvalidAssignedType: return {
                .loc = cur_expr->get_source_loc(),
                .message = fmt::format("Invalid assignment type: expected {}.",
                                       AstPrinter(source).to_string(*cur_var_decl->type))
            };
        case SemaResultType::InvalidReturnType: return {
                .loc = cur_expr->get_source_loc(),
                .message = fmt::format("Invalid return type: expected {}.",
                                       AstPrinter(source).to_string(*expected_type))
            };
        case SemaResultType::InvalidParamType: return {
                .loc = cur_expr->get_source_loc(),
                .message = fmt::format("Invalid param type: cannot be void.")
            };
        case SemaResultType::IncompatibleTypes: return {
                .loc = cur_expr->get_source_loc(),
                .message = fmt::format("Incompatible types between {} and {}.",
                                       AstPrinter(source).to_string(*cur_expr->type),
                                       AstPrinter(source).to_string(*other_expr->type))
            };
        case SemaResultType::CannotInferType: return {
                .loc = cur_expr->get_source_loc(),
                .message = fmt::format("Cannot infer type.",
                                       AstPrinter(source).to_string(*expected_type))
            };
        case SemaResultType::CannotFindType: {
            auto unassigned_type = expected_type->cast<UnassignedType>();
            return {
                .loc = unassigned_type.name.get_source_loc(),
                .message = fmt::format("Cannot find type {}.", unassigned_type.name.str(source))
            };
        };
        case SemaResultType::CannotDotAccessOnType: return {
                .loc = cur_expr->get_source_loc(),
                .message = fmt::format("Cannot dot access on type {}",
                                       AstPrinter(source).to_string(*expected_type))

            };
        case SemaResultType::CannotCallOnType: return {
                    .loc = cur_expr->get_source_loc(),
                    .message = fmt::format("Cannot call on type {}",
                                           AstPrinter(source).to_string(*expected_type))
            };
        case SemaResultType::CannotFindField: return {
                .loc = cur_expr->get_source_loc(),
                .message = fmt::format("Cannot find field.")
            };
        case SemaResultType::IncompatibleFieldType: return {
                .loc = cur_expr->get_source_loc(),
                .message = fmt::format("Incompatible field type.")
            };
        case SemaResultType::InvalidNativeFunDeclLocation: return {
                .loc = cur_fun_decl->name.get_source_loc(),
                .message = fmt::format("Invalid native function declaration: only allowed at the module-level.")
            };
        case SemaResultType::InvalidImportDeclaration: return {
                .loc = cur_stmt->cast<ImportStmt>().package_path[0].get_source_loc(),
                .message = fmt::format("Invalid import declaration: only allowed at the module-level.")
            };
        case SemaResultType::Misc: return {
                .loc = cur_expr->get_source_loc(),
                .message = "Misc."
            };
        default: return {};
        }
    }
};

class AstAllocator;

class SemaAnalyzer :
    public StmtVisitorBase<SemaAnalyzer, SemaResult>,
    public ExprVisitorBase<SemaAnalyzer, SemaResult> {
private:
    AstAllocator* m_allocator;
    const u8* m_source;

    Vector<SemaResult> m_errors;
    SemaEnv* m_cur_env = nullptr;
    ImportMap* m_import_map = nullptr;

    void revert_parent_env() {}

    SemaResult report_error(SemaResult res) {
        m_errors.push_back(res);
        return res;
    }

public:
    using StmtVisitorBase<SemaAnalyzer, SemaResult>::visit;
    using ExprVisitorBase<SemaAnalyzer, SemaResult>::visit;

    SemaAnalyzer(AstAllocator* allocator, const u8* source) : m_allocator(allocator), m_source(source) {}

    Vector<SemaResult> scan_dependencies(std::string_view module_name, ModuleStmt* module) {
        Vector<ImportStmt*> imports;
        Vector<AstFunDecl*> exports;

        Vector<SemaResult> errors;

        SemaEnv module_env(m_cur_env, module);
        m_cur_env = &module_env;

        for (RelPtr<Stmt>& inner_stmt : module->statements) {
            if (auto import_stmt = inner_stmt->try_cast<ImportStmt>()) {
                imports.push_back(import_stmt);
            }
            else if (auto fun_stmt = inner_stmt->try_cast<FunctionStmt>()) {
                if (fun_stmt->is_public) {
                    auto res = typecheck_fun_decl(*fun_stmt);
                    if (!res.is_ok()) errors.push_back(res);
                    fun_stmt->fun_decl.module = module_name;
                    exports.push_back(&fun_stmt->fun_decl);
                }
            }
        }

        m_cur_env = nullptr;

        module->set_imports(m_allocator->alloc_vector<RelPtr<ImportStmt>, ImportStmt*>(std::move(imports)));
        module->set_exports(m_allocator->alloc_vector<RelPtr<AstFunDecl>, AstFunDecl*>(std::move(exports)));

        return errors;
    }

    Vector<SemaResult> typecheck(ModuleStmt* module_stmt, ImportMap& import_map) {
        SemaEnv module_env(m_cur_env, module_stmt);
        m_cur_env = &module_env;
        m_import_map = &import_map;

        // Add imported functions to the environment
        for (auto [name, import_fun_stmt] : import_map) {
            m_cur_env->set_function(name, import_fun_stmt);
        }

        // Visit the AST for semantic analysis
        visit_impl(*module_stmt);

        m_cur_env = nullptr;
        m_import_map = nullptr;
        module_stmt->set_locals(m_allocator->alloc_vector<RelPtr<AstVarDecl>, AstVarDecl*>(module_env.get_locals()));

        auto errors = m_errors;
        m_errors.clear();
        return errors;
    }

#define SEMA_TRY(EXPR) do { auto _res = EXPR; if (!_res.is_ok()) return _res; } while (false)

    SemaResult visit_impl(ErrorStmt& stmt) { return ok(); } // unreachable
    SemaResult visit_impl(BlockStmt& stmt) {
        SemaEnv block_env(m_cur_env);
        m_cur_env = &block_env;

        // For the first pass, scan all function definitions and add them to the env first
        for (RelPtr<Stmt>& inner_stmt : stmt.statements) {
            if (auto fun_stmt = inner_stmt->try_cast<FunctionStmt>()) {
                if (fun_stmt->is_native) {
                    // Native function declarations should be only at the module level!
                    return error_invalid_native_fun_declaration(&fun_stmt->fun_decl);
                }
                auto fun_name = fun_stmt->fun_decl.name.str(m_source);
                m_cur_env->set_function(fun_name, &fun_stmt->fun_decl);
            }
        }

        // Inside the second pass, do a proper sema visit
        for (RelPtr<Stmt>& inner_stmt : stmt.statements) {
            // Do not return on error result, since we want to scan all statements inside the block.
            auto _ = visit(*inner_stmt.get());
        }
        m_cur_env = block_env.get_outer_env();
        return ok();
    }

    SemaResult visit_impl(ModuleStmt& stmt) {
        // For the first pass, scan all function definitions and add them to the env first
        for (RelPtr<Stmt>& inner_stmt : stmt.statements) {
            if (auto fun_stmt = inner_stmt->try_cast<FunctionStmt>()) {
                auto fun_name = fun_stmt->fun_decl.name.str(m_source);
                m_cur_env->set_function(fun_name, &fun_stmt->fun_decl);
            }
        }
        // Inside the second pass, do a proper sema visit
        for (RelPtr<Stmt>& inner_stmt : stmt.statements) {
            // Do not return on error result, since we want to scan all statements inside the block.
            auto _ = visit(*inner_stmt.get());
        }

        return ok();
    }

    SemaResult visit_impl(ExpressionStmt& stmt) {
        return visit(*stmt.expr.get());
    }

    SemaResult visit_impl(StructStmt& stmt) {
        auto fields = stmt.fields.to_span();
        auto struct_type = m_allocator->alloc<StructType>(stmt.name, fields);

        // Calculate size and alignment of newly made struct type
        for (auto& field : fields) {
            auto field_type = field.type.get();
            auto unassigned_type = field_type->try_cast<UnassignedType>();
            if (unassigned_type) {
                auto field_type_name = unassigned_type->name.str(m_source);
                auto found_struct = m_cur_env->get_struct(field_type_name);
                if (found_struct) {
                    field.type = found_struct->type.get();
                }
                else {
                    auto _ = error_cannot_find_type(unassigned_type);
                    continue;
                }
            }
        }
        struct_type->calc_size_and_alignment();

        stmt.type = struct_type;
        m_cur_env->set_struct(stmt.name.str(m_source), &stmt);

        return ok();
    }

    // Helper function that only registers the function stmt declaration, but doesn't analyze the inner contents.
    SemaResult typecheck_fun_decl(FunctionStmt& stmt) {
        // Find return type
        if (auto unassigned_type = stmt.fun_decl.ret_type->try_cast<UnassignedType>()) {
            auto param_type_name = unassigned_type->name.str(m_source);
            auto struct_stmt = m_cur_env->get_struct(param_type_name);
            if (struct_stmt) {
                stmt.fun_decl.ret_type = struct_stmt->type.get();
            }
            else {
                auto _ = error_cannot_find_type(unassigned_type);
            }
        }

        Vector<Type*> param_types;
        for (auto& param : stmt.fun_decl.params) {
            if (auto unassigned_param_type = param.type->try_cast<UnassignedType>()) {
                auto struct_stmt = m_cur_env->get_struct(unassigned_param_type->name.str(m_source));
                if (struct_stmt) {
                    param.type = struct_stmt->type.get();
                }
                else {
                    auto _ = error_cannot_find_type(unassigned_param_type);
                }
            }
            param_types.push_back(param.type.get());
        }

        stmt.fun_decl.type = m_allocator->alloc<FunctionType>(
                m_allocator->alloc_vector<RelPtr<Type>, Type*>(std::move(param_types)),
                stmt.fun_decl.ret_type.get());

        return ok();
    }

    SemaResult visit_impl(FunctionStmt& stmt) {
        // Type-check the function, if it isn't already done so.
        if (!stmt.fun_decl.type.get()) {
            SEMA_TRY(typecheck_fun_decl(stmt));
        }

        // For native functions there is no body to typecheck.
        if (stmt.is_native) {
            return ok();
        }

        SemaEnv fn_env(m_cur_env, &stmt);
        m_cur_env = &fn_env;

        // Add all function parameters to the scope.
        for (auto& param : stmt.fun_decl.params) {
            // Assign type to each param
            auto param_name = param.name.str(m_source);
            if (auto prim_type = param.type->try_cast<PrimitiveType>()) {
                if (prim_type->prim_kind == PrimTypeKind::Void) {
                    return error_invalid_param_type(&param);
                }
            }
            auto unassigned_type = param.type->try_cast<UnassignedType>();
            if (unassigned_type) {
                auto param_type_name = unassigned_type->name.str(m_source);
                auto struct_stmt = m_cur_env->get_struct(param_type_name);
                if (struct_stmt) {
                    param.type = struct_stmt->type.get();
                }
                else {
                    auto _ = error_cannot_find_type(unassigned_type);
                }
            }
            m_cur_env->set_var(param_name, &param);
        }

        // Do a sema pass on the statements inside the body.
        for (auto& body_stmt : stmt.body) {
            auto _ = visit(*body_stmt);
        }

        stmt.set_locals(m_allocator->alloc_vector<RelPtr<AstVarDecl>, AstVarDecl*>(fn_env.get_locals()));

        m_cur_env = fn_env.get_outer_env();
        return ok();
    }

    SemaResult visit_impl(IfStmt& stmt) {
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

    SemaResult visit_impl(VarStmt& stmt) {
        if (Expr* init_expr = stmt.initializer.get()) {
            SEMA_TRY(visit(*init_expr));
            auto inferred_type = stmt.var.type->try_cast<InferredType>();
            if (inferred_type) {
                // Basic local type inference
                stmt.var.type = init_expr->type.get();
            }
            else {
                Type* var_type = stmt.var.type.get();
                if (!is_type_compatible(var_type, init_expr->type.get())) {
                    return error_invalid_initializer_type(init_expr, var_type);
                }
                else {
                    stmt.var.type = init_expr->type.get();
                }
            }
        }
        else {
            assert(stmt.var.type != nullptr); // The parser filters out this case beforehand

            auto unassigned_type = stmt.var.type->try_cast<UnassignedType>();
            if (unassigned_type) {
                auto var_type_name = unassigned_type->name.str(m_source);
                auto struct_stmt = m_cur_env->get_struct(var_type_name);
                if (struct_stmt) {
                    stmt.var.type = struct_stmt->type.get();
                }
                else {
                    return error_cannot_find_type(unassigned_type);
                }
            }

            // insert default value
            stmt.initializer = nullptr; // TODO
        }
        m_cur_env->set_var(stmt.var.name.str(m_source), &stmt.var);

        return {SemaResultType::Ok, nullptr};
    }

    SemaResult visit_impl(WhileStmt& stmt) {
        auto cond_expr = stmt.condition.get();
        SEMA_TRY(visit(*cond_expr));
        if (!cond_expr->type->is_bool()) {
            return error_wrong_type(cond_expr, m_allocator->get_bool_type());
        }
        return visit(*stmt.body.get());
    }

    SemaResult visit_impl(ReturnStmt& stmt) {
        auto return_expr = stmt.expr.get();
        SEMA_TRY(visit(*return_expr));
        Type* return_type = return_expr->type.get();
        auto cur_fn = m_cur_env->get_outer_function();
        Type* fn_return_type = cur_fn->fun_decl.ret_type.get();
        if (fn_return_type) {
            if (!is_type_same(return_type, fn_return_type)) {
                return error_invalid_return_type(return_expr, fn_return_type);
            }
        }
        else {
            cur_fn->fun_decl.ret_type = return_type;
        }
        return ok();
    }

    SemaResult visit_impl(BreakStmt& stmt) { return ok(); } // nothing to do
    SemaResult visit_impl(ContinueStmt& stmt) { return ok(); } // nothing to do

    SemaResult visit_impl(ImportStmt& stmt) {
        if (!m_cur_env->is_module_scope()) {
            return error_invalid_import_declaration(&stmt);
        }
        return ok();
    }

    SemaResult visit_impl(ErrorExpr& expr) { return error_misc(&expr); } // unreachable???
    SemaResult visit_impl(AssignExpr& expr) {
        if (auto var_decl = m_cur_env->get_var(expr.name.str(m_source))) {
            Expr* assigned_expr = expr.value.get();
            SEMA_TRY(visit(*assigned_expr));

            Type* var_type = var_decl->type.get();
            expr.type = var_type;
            expr.origin = var_decl;

            // If assigned kind is not compatible with variable, then error.
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

    SemaResult visit_impl(BinaryExpr& expr) {
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
        case TokenType::Slash: {
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
        case TokenType::LessEqual: {
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
        case TokenType::BarBar: {
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
        case TokenType::EqualEqual: {
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

    SemaResult visit_impl(TernaryExpr& expr) {
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

    SemaResult visit_impl(GroupingExpr& expr) {
        auto& inner_expr = *expr.expression;
        SEMA_TRY(visit(inner_expr));
        expr.type = inner_expr.type.get();
        return ok();
    }

    SemaResult visit_impl(LiteralExpr& expr) {
        expr.type = m_allocator->alloc<PrimitiveType>(expr.value.kind);
        return ok();
    }

    SemaResult visit_impl(UnaryExpr& expr) {
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

    SemaResult visit_impl(VariableExpr& expr) {
        auto expr_name = expr.name.str(m_source);

        auto var_decl = m_cur_env->get_var(expr_name);
        if (var_decl) {
            // Loading variables
            expr.type = var_decl->type.get();
            expr.var_origin = var_decl;
            return ok();
        }

        auto it = m_import_map->find(expr_name);
        if (it != m_import_map->end()) {
            AstFunDecl* fun_decl = it->second;
            expr.type = fun_decl->type.get();
            expr.fun_origin = fun_decl;
            return ok();
        }

        auto fun_decl = m_cur_env->get_function(expr_name);
        if (fun_decl) {
            // Loading functions
            expr.type = fun_decl->type.get();
            expr.fun_origin = fun_decl;
            return ok();
        }

        return error_undefined_var(&expr);
    }

    SemaResult visit_impl(CallExpr& expr) {
        auto callee_expr = expr.callee.get();
        SEMA_TRY(visit(*callee_expr));
        auto callee_type = callee_expr->type.get();
        if (auto function_type = callee_type->try_cast<FunctionType>()) {
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

    SemaResult visit_impl(GetExpr& expr) {
        auto obj_expr = expr.object.get();

        SEMA_TRY(visit(*obj_expr));
        auto obj_type = obj_expr->type.get();
        if (auto obj_struct_type = obj_type->try_cast<StructType>()) {
            Type* type = nullptr;
            for (auto& var_decl : obj_struct_type->declarations) {
                auto var_name = var_decl.name.str(m_source);
                if (var_name == expr.name.str(m_source)) {
                    type = var_decl.type.get();
                    break;
                }
            }
            if (type) {
                expr.type = type;
                return ok();
            }
            else {
                return error_cannot_find_field(&expr);
            }
        }
        else {
            return error_cannot_dot_access_on_type(obj_expr, obj_type);
        }
    }

    SemaResult visit_impl(SetExpr& expr) {
        auto obj_expr = expr.object.get();
        SEMA_TRY(visit(*obj_expr));
        auto obj_type = obj_expr->type.get();
        if (auto obj_struct_type = obj_type->try_cast<StructType>()) {
            Type* type = nullptr;
            for (auto& var_decl : obj_struct_type->declarations) {
                auto var_name = var_decl.name.str(m_source);
                if (var_name == expr.name.str(m_source)) {
                    type = var_decl.type.get();
                    break;
                }
            }
            if (type) {
                expr.type = type;
                auto value_expr = expr.value.get();
                SEMA_TRY(visit(*value_expr));
                Type* value_type = value_expr->type.get();
                if (is_type_compatible(type, value_type)) {
                    return ok();
                }
                else {
                    return error_incompatible_field_type(&expr, expr.name, type);
                }
            }
            else {
                return error_cannot_find_field(&expr);
            }
        }
        else {
            return error_cannot_dot_access_on_type(obj_expr, obj_type);
        }
    }

    inline bool is_type_same(Type* lhs, Type* rhs) {
        if (lhs == rhs) return true;
        if (lhs->kind != rhs->kind) return false;
        switch (lhs->kind) {
        case TypeKind::Primitive:
            return lhs->cast<PrimitiveType>().prim_kind == rhs->cast<PrimitiveType>().prim_kind;
        case TypeKind::Struct: {
            auto& lhs_struct = lhs->cast<StructType>();
            auto& rhs_struct = rhs->cast<StructType>();
            return lhs_struct.name.str(m_source) == rhs_struct.name.str(m_source);
        }
        default:
            return false; // TODO
        }
    }

    inline bool is_type_compatible(Type* lhs, Type* rhs) {
        if (lhs == rhs) return true;
        if (lhs->kind != rhs->kind) return false;
        switch (lhs->kind) {
        case TypeKind::Primitive: {
            auto& lhs_type = lhs->cast<PrimitiveType>();
            auto& rhs_type = rhs->cast<PrimitiveType>();

            // implicit numeric type conversions (only to larger number type)
            if (lhs_type.is_signed_integer() && rhs_type.is_signed_integer()) {
                return lhs_type.prim_kind >= rhs_type.prim_kind;
            }
            else if (lhs_type.is_unsigned_integer() && rhs_type.is_unsigned_integer()) {
                return lhs_type.prim_kind >= rhs_type.prim_kind;
            }
            else if (lhs_type.is_floating_point_num() && rhs_type.is_floating_point_num()) {
                return lhs_type.prim_kind >= rhs_type.prim_kind;
            }
            else {
                return lhs_type.prim_kind == rhs_type.prim_kind;
            }
        }
        case TypeKind::Struct: {
            auto& lhs_struct = lhs->cast<StructType>();
            auto& rhs_struct = rhs->cast<StructType>();
            // TODO: add subtyping
            return lhs_struct.name.str(m_source) == rhs_struct.name.str(m_source);
        }
        default:
            return false; // TODO
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

    SemaResult error_invalid_initializer_type(Expr* initializer_expr, Type* expected_type) {
        return report_error({
            .res_type = SemaResultType::InvalidInitializerType,
            .cur_expr = initializer_expr,
            .expected_type = expected_type
        });
    }

    SemaResult error_invalid_assigned_type(AstVarDecl* var_decl, Expr* assigned_expr) {
        return report_error({
            .res_type = SemaResultType::InvalidAssignedType,
            .cur_expr = assigned_expr,
            .cur_var_decl = var_decl,
        });
    }

    SemaResult error_invalid_param_type(AstVarDecl* param_var_decl) {
        return report_error({
            .res_type = SemaResultType::InvalidParamType,
            .cur_var_decl = param_var_decl,
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

    SemaResult error_cannot_find_type(Type* type) {
        return report_error({
            .res_type = SemaResultType::CannotFindType,
            .expected_type = type
        });
    }

    SemaResult error_cannot_dot_access_on_type(Expr* expr, Type* type) {
        return report_error({
            .res_type = SemaResultType::CannotDotAccessOnType,
            .cur_expr = expr,
            .expected_type = type
        });
    }

    SemaResult error_cannot_call_on_type(Expr* expr, Type* type) {
        return report_error({
            .res_type = SemaResultType::CannotCallOnType,
            .cur_expr = expr,
            .expected_type = type
        });
    }

    SemaResult error_cannot_find_field(Expr* expr) {
        return report_error({
            .res_type = SemaResultType::CannotFindField,
            .cur_expr = expr,
        });
    }

    SemaResult error_incompatible_field_type(Expr* expr, Token name, Type* type) {
        return report_error({
            .res_type = SemaResultType::IncompatibleFieldType,
            .cur_expr = expr,
            .cur_name = name,
            .expected_type = type
        });
    }

    SemaResult error_invalid_native_fun_declaration(AstFunDecl* fun_decl) {
        return report_error({
            .res_type = SemaResultType::InvalidNativeFunDeclLocation,
            .cur_fun_decl = fun_decl
        });
    }

    SemaResult error_invalid_import_declaration(Stmt* stmt) {
        return report_error({
            .res_type = SemaResultType::InvalidImportDeclaration,
            .cur_stmt = stmt
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
