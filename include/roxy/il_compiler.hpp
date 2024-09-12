#pragma once

#include <il.hpp>
#include <type.hpp>

#include "roxy/il.hpp"
#include "roxy/compiler.hpp"

namespace rx {

class ILCompiler : public CompilerBase {
    ILCompiler(Scanner* scanner) : CompilerBase(scanner) {}

private:
    CompileResult visit_impl(ErrorStmt& stmt) { return ok(); }
    CompileResult visit_impl(BlockStmt& stmt) {
        for (auto& inner_stmt : stmt.statements) {
            COMP_TRY(visit(*inner_stmt.get()));
        }
        return ok();
    }

    CompileResult visit_impl(ModuleStmt& stmt) {
        FnLocalEnv module_env(stmt, m_scanner->source());
        m_cur_fn_env = &module_env;

        for (auto& inner_stmt : stmt.statements) {
            COMP_TRY(visit(*inner_stmt.get()));
        }

        emit_ret(-1);

        m_cur_fn_env->move_locals_to_chunk(m_cur_chunk);
        m_cur_fn_env = nullptr;
        m_cur_chunk = nullptr;
        return ok();
    }

    CompileResult visit_impl(ExpressionStmt& stmt) {
        COMP_TRY(visit(*stmt.expr.get()));
        return ok();
    }

    CompileResult visit_impl(StructStmt& stmt) { return ok(); }

    CompileResult visit_impl(FunctionStmt& stmt) {
        auto fn_name = std::string(stmt.fun_decl.name.str(m_scanner->source()));
        auto fn_type = stmt.fun_decl.type.get();

        // If this is a native function declaration, there is no implementation yet
        // So just add the definition to the native table and return.
        if (stmt.is_native) {
            m_cur_module->m_native_function_table.push_back({
                fn_name,
                std::string(m_cur_module->name()),
                FunctionTypeData(*fn_type, m_scanner->source()),
                NativeFunctionRef {} // We do not know the exact function pointer yet... just set it to null for now
            });
            return ok();
        }

        // Set current chunk to newly created chunk of function
        auto fn_chunk = UniquePtr<Chunk>(new Chunk(fn_name, m_cur_module));
        Chunk* parent_chunk = m_cur_chunk;
        m_cur_chunk = fn_chunk.get();

        // Create new local env
        FnLocalEnv fn_env(m_cur_fn_env, stmt, m_scanner->source());
        m_cur_fn_env = &fn_env;

        for (auto& body_stmt : stmt.body) {
            COMP_TRY(visit(*body_stmt));
        }

        // Build function chunk and insert it to parent module
        m_cur_module->m_function_table.push_back({
            fn_name,
            std::string(m_cur_module->name()),
            FunctionTypeData(*fn_type, m_scanner->source()),
            std::move(fn_chunk)});

        // Revert to parent env and chunk
        m_cur_fn_env->move_locals_to_chunk(m_cur_chunk);
        m_cur_fn_env = fn_env.get_outer_env();
        m_cur_chunk = parent_chunk;

        return ok();
    }

    CompileResult visit_impl(AssignExpr& expr) {
        COMP_TRY(visit(*expr.value));

        u32 cur_line = m_scanner->get_line(expr.name.get_source_loc());
        AstVarDecl* var_decl = expr.origin.get();
        u16 offset = m_cur_fn_env->get_local_offset(var_decl->local_index);
        auto expr_type = expr.type.get();
        if (auto prim_type = expr_type->try_cast<PrimitiveType>()) {
            if (prim_type->is_within_4_bytes_integer()) {
                emit_code({ILOperator::AssignD})
                assign_value = ILAddress::make_const_int(0);
            }
            else if (prim_type->prim_kind == PrimTypeKind::U64 || prim_type->prim_kind == PrimTypeKind::I64) {
                assign_value = ILAddress::make_const_long(0);
            }
            else if (prim_type->prim_kind == PrimTypeKind::F32) {
                assign_value = ILAddress::make_const_float(0);
            }
            else if (prim_type->prim_kind == PrimTypeKind::F64) {
                assign_value = ILAddress::make_const_double(0);
            }
            else if (prim_type->prim_kind == PrimTypeKind::String) {
                assign_value = ILAddress::make_addr(0); // TODO: how to get reference to empty string?
            }
            else {
                return unreachable();
            }
            if (prim_type->is_within_4_bytes()) {
                emit_store_u32(offset, cur_line);
            }
            else {
                if (prim_type->is_string()) {
                    emit_store_ref(offset, cur_line);
                }
                else {
                    emit_store_u64(offset, cur_line);
                }
            }
        }
        else {
            return unimplemented();
        }

        return ok();
    }

    CompileResult visit_impl(IfStmt& stmt) {
        Expr* cond_expr = stmt.condition.get();
        u32 cond_line = m_scanner->get_line(cond_expr->get_source_loc());

        COMP_TRY(visit(*cond_expr));
        u32 then_jump = emit_ifz_with_patch(cond_expr->il_address, cond_line);

        Stmt* then_stmt = stmt.then_branch.get();
        COMP_TRY(visit(*then_stmt));

        u32 else_jump = emit_jmp_with_patch(cond_line);

        patch_ifz(then_jump);

        Stmt* else_stmt = stmt.else_branch.get();
        if (else_stmt) {
            COMP_TRY(visit(*else_stmt));
        }
        patch_jmp(else_jump);

        return ok();
    }

    CompileResult visit_impl(VarStmt& stmt) {
        u32 cur_line = m_scanner->get_line(stmt.var.name.get_source_loc());
        ILAddress assign_value;
        if (Expr* init_expr = stmt.initializer.get()) {
            COMP_TRY(visit(*init_expr));
            assign_value = init_expr->il_address;
        }
        else {
            // synthesize default initializer
            auto type = stmt.var.type.get();
            if (auto prim_type = type->try_cast<PrimitiveType>()) {
                assign_value = make_const_value(prim_type->prim_kind);
            }
            else {
                return unimplemented();
            }
        }

        auto type = stmt.var.type.get();
        if (auto prim_type = type->try_cast<PrimitiveType>()) {
            stmt.var.il_address = ILAddress::make_reg(make_virtual_reg());
            emit_code({il_op_assign(prim_type->prim_kind), stmt.var.il_address, assign_value}, cur_line);
        }
        else if (auto struct_type = type->try_cast<StructType>()) {
            return unimplemented();
        }
        else {
            return error("Cannot compile expression with unassigned type!");
        }

        return ok();
    }

    CompileResult visit_impl(WhileStmt& stmt) {
        u32 loop_start = m_cur_chunk->m_bytecode.size();

        auto cond_expr = stmt.condition.get();
        u32 cond_line = m_scanner->get_line(cond_expr->get_source_loc());

        COMP_TRY(visit(*cond_expr));
        u32 exit_jump = emit_ifz_with_patch(cond_expr->il_address, cond_line);

        COMP_TRY(visit(*stmt.body));
        emit_jmp(loop_start, cond_line);

        patch_ifz(exit_jump);
        return ok();
    }

    CompileResult visit_impl(ReturnStmt& stmt) {
        auto inner_expr = stmt.expr.get();
        if (inner_expr) {
            u32 cond_line = m_scanner->get_line(inner_expr->get_source_loc());
            COMP_TRY(visit(*inner_expr));
            Type* ret_type = inner_expr->type.get();
            if (auto prim_ret_type = ret_type->try_cast<PrimitiveType>()) {
                if (prim_ret_type->is_within_4_bytes()) {
                    emit_code({ILOperator::RetI}, cond_line);
                }
                else {
                    emit_code({ILOperator::RetL}, cond_line);
                }
            }
            else {
                unimplemented();
            }
        }
        else {
            // TODO: find a way to retrieve line information
            emit_ret(-1);
        }

        return ok();
    }

    CompileResult visit_impl(BinaryExpr& expr) {
        u32 cur_line = m_scanner->get_line(expr.get_source_loc());

        Expr* left_expr = expr.left.get();
        Expr* right_expr = expr.right.get();

        COMP_TRY(visit(*left_expr));

        // TODO: Support operator overloading for custom types
        if (auto prim_type = expr.type->try_cast<PrimitiveType>()) {
            if (expr.op.is_arithmetic()) {
                COMP_TRY(visit(*right_expr));
                if (prim_type->is_number()) {
                    ILOperator op = il_op_binary(prim_type->prim_kind, expr.op.type);
                    u16 new_reg = make_virtual_reg();
                    emit_code({op, new_reg, left_expr->il_address, right_expr->il_address}, cur_line);
                }
                else if (prim_type->is_string()) {
                    // Call concat() builtin
                    if (expr.op.type == TokenType::Plus) {
                        u16 native_fun_index = m_cur_module->find_native_function_index("concat");
                        emit_code({ILOperator::Call, ILAddress::make_const_int(native_fun_index)}, cur_line);
                    }
                    else {
                        unreachable();
                    }
                }
                else {
                    unreachable();
                }
            }
            else {
                switch (expr.op.type) {
                // logical and
                case TokenType::AmpAmp: {
                    u32 end_jump = emit_ifz_with_patch(left_expr->il_address, cur_line);
                    COMP_TRY(visit(*right_expr));
                    patch_ifz(end_jump);
                    break;
                }
                // logical or
                case TokenType::BarBar: {
                    u32 else_jump = emit_ifz_with_patch(left_expr->il_address, cur_line);
                    u32 end_jump = emit_jmp_with_patch(cur_line);

                    patch_ifz(else_jump);
                    COMP_TRY(visit(*right_expr));
                    patch_jmp(end_jump);
                    break;
                }
                default:
                    return unimplemented();
                }
            }
        }
        else {
            unimplemented();
        }

        return ok();
    }

    CompileResult visit_impl(BreakStmt& stmt) { return unimplemented(); }
    CompileResult visit_impl(ContinueStmt& stmt) { return unimplemented(); }

    CompileResult visit_impl(ImportStmt& stmt) {
        return ok();
    }

    CompileResult visit_impl(ErrorExpr& expr) { return error("Cannot compile expression with error!"); }

    CompileResult visit_impl(AssignExpr& expr) {
        COMP_TRY(visit(*expr.value));

        u32 cur_line = m_scanner->get_line(expr.name.get_source_loc());
        AstVarDecl* var_decl = expr.origin.get();
        u16 offset = m_cur_fn_env->get_local_offset(var_decl->local_index);
        auto expr_type = expr.type.get();
        if (auto prim_type = expr_type->try_cast<PrimitiveType>()) {
            ILOperator op = il_op_assign(prim_type->prim_kind);
            emit_code({op, }, cur_line);
            if (prim_type->is_within_4_bytes()) {
                emit_store_u32(offset, cur_line);
            }
            else {
                if (prim_type->is_string()) {
                    emit_store_ref(offset, cur_line);
                }
                else {
                    emit_store_u64(offset, cur_line);
                }
            }
        }
        else {
            return unimplemented();
        }

        return ok();
    }

    void emit_code(ILCode code, u32 line) {
        m_cur_chunk->m_ilcode.push_back(code);
    }

    void emit_ret(u32 line) {
        emit_code({ILOperator::Ret}, line);
    }

    u32 emit_ifz_with_patch(ILAddress operand, u32 line) {
        emit_code({ILOperator::IfZ, operand, ILAddress::make_invalid()}, line);
        return m_cur_chunk->m_ilcode.size() - 1;
    }

    void patch_ifz(u32 offset) {
        u32 jump = m_cur_chunk->m_ilcode.size() - offset - 1;
        m_cur_chunk->m_ilcode[offset].a2 = ILAddress::make_const_int(jump);
    }

    u32 emit_jmp_with_patch(u32 line) {
        emit_code({ILOperator::Jmp, ILAddress::make_invalid()}, line);
        return m_cur_chunk->m_ilcode.size() - 1;
    }

    void patch_jmp(u32 offset) {
        u32 jump = m_cur_chunk->m_ilcode.size() - offset - 1;
        m_cur_chunk->m_ilcode[offset].a1 = ILAddress::make_const_int(jump);
    }

    void emit_jmp(u32 loop_start, u32 line) {
        u32 offset = m_cur_chunk->m_ilcode.size() - loop_start + 1;
        emit_code({ILOperator::Jmp, offset}, line);
    }

    u16 make_virtual_reg() {
        return m_num_virtual_regs++;
    }

    u16 m_num_virtual_regs = 0;
};

}