#pragma once

#include "roxy/compiler_common.hpp"

namespace rx {

class Compiler : public CompilerBase {
    Compiler(Scanner* scanner) : CompilerBase(scanner) {}

    Chunk* m_cur_chunk;

public:

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

        emit_byte(OpCode::ret, -1);

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

    // Emits a jump instruction from a conditional.
    // Used for implementing if / while / for statements.
    CompileResult emit_jump_from_cond_expr(Expr* cond_expr, bool shortened, bool opposite, u32& jump_loc) {
        u32 cond_line = m_scanner->get_line(cond_expr->get_source_loc());
        if (auto unary_expr = cond_expr->try_cast<UnaryExpr>()) {
            if (unary_expr->op.type == TokenType::Bang) {
                Expr* right_expr = unary_expr->right.get();
                COMP_TRY(visit(*right_expr));
                jump_loc = emit_jump(OpCode::br_true, cond_line);
            }
            else {
                COMP_TRY(visit(*cond_expr));
                jump_loc = emit_jump(OpCode::br_false, cond_line);
            }
        }
        else if (auto binary_expr = cond_expr->try_cast<BinaryExpr>()) {
            Expr* left_expr = binary_expr->left.get();
            Expr* right_expr = binary_expr->right.get();
            COMP_TRY(visit(*left_expr));
            COMP_TRY(visit(*right_expr));
            Type* binary_type = binary_expr->type.get();
            PrimitiveType* left_prim_type = left_expr->type->try_cast<PrimitiveType>();
            PrimitiveType* right_prim_type = right_expr->type->try_cast<PrimitiveType>();
            if (left_prim_type && right_prim_type && left_prim_type->prim_kind == right_prim_type->prim_kind) {
                // If lhs / rhs is primitive type
                if (left_prim_type->is_within_4_bytes_integer()) {
                    jump_loc = emit_jump(opcode_integer_br_cmp(binary_expr->op.type, shortened, opposite), cond_line);
                }
                else if (left_prim_type->is_integer()){
                    emit_byte(OpCode::lcmp, cond_line);
                    switch (binary_expr->op.type) {
                    case TokenType::EqualEqual:
                        jump_loc = emit_jump(OpCode::br_false, cond_line);
                        break;
                    case TokenType::BangEqual:
                        jump_loc = emit_jump(OpCode::br_true, cond_line);
                        break;
                    case TokenType::Less:
                        jump_loc = emit_jump(OpCode::br_ge, cond_line);
                        break;
                    case TokenType::LessEqual:
                        jump_loc = emit_jump(OpCode::br_gt, cond_line);
                        break;
                    case TokenType::Greater:
                        jump_loc = emit_jump(OpCode::br_le, cond_line);
                        break;
                    case TokenType::GreaterEqual:
                        jump_loc = emit_jump(OpCode::br_lt, cond_line);
                        break;
                    }
                }
                else if (left_prim_type->is_floating_point_num()) {
                    auto prim_kind = left_prim_type->prim_kind;
                    // TODO: test if this code is really right
                    switch (binary_expr->op.type) {
                    case TokenType::EqualEqual:
                        emit_byte(opcode_floating_cmp(prim_kind), cond_line);
                        jump_loc = emit_jump(OpCode::br_false, cond_line);
                        break;
                    case TokenType::BangEqual:
                        emit_byte(opcode_floating_cmp(prim_kind), cond_line);
                        jump_loc = emit_jump(OpCode::br_true, cond_line);
                        break;
                    case TokenType::Less:
                        emit_byte(opcode_floating_cmp(prim_kind), cond_line);
                        jump_loc = emit_jump(OpCode::br_ge, cond_line);
                        break;
                    case TokenType::LessEqual:
                        emit_byte(opcode_floating_cmp(prim_kind), cond_line);
                        jump_loc = emit_jump(OpCode::br_gt, cond_line);
                        break;
                    case TokenType::Greater:
                        emit_byte(opcode_floating_cmp(prim_kind), cond_line);
                        jump_loc = emit_jump(OpCode::br_le, cond_line);
                        break;
                    case TokenType::GreaterEqual:
                        emit_byte(opcode_floating_cmp(prim_kind), cond_line);
                        jump_loc = emit_jump(OpCode::br_lt, cond_line);
                        break;
                    default:
                        unreachable();
                    }
                }

                return ok();
            }
            StructType* left_struct_type = binary_type->try_cast<StructType>();
            StructType* right_struct_type = binary_type->try_cast<StructType>();
            if (left_struct_type && right_struct_type && left_struct_type == right_struct_type) {
                // TODO: If lhs / rhs are same struct types, find and call operator function
                unimplemented();
            }
        }
        else {
            COMP_TRY(visit(*cond_expr));
            jump_loc = emit_jump(OpCode::br_false, cond_line);
        }
        return ok();
    }

    CompileResult visit_impl(IfStmt& stmt) {
        Expr* cond_expr = stmt.condition.get();
        u32 cond_line = m_scanner->get_line(cond_expr->get_source_loc());

        u32 then_jump;
        COMP_TRY(emit_jump_from_cond_expr(cond_expr, false, true, then_jump));

        Stmt* then_stmt = stmt.then_branch.get();
        COMP_TRY(visit(*then_stmt));

        u32 else_jump = emit_jump(OpCode::jmp, cond_line);

        patch_jump(then_jump);

        Stmt* else_stmt = stmt.else_branch.get();
        if (else_stmt) {
            COMP_TRY(visit(*else_stmt));
        }
        patch_jump(else_jump);

        return ok();
    }

    CompileResult push_zero_initialized_value(Type* type, u32 cur_line) {
        if (auto prim_type = type->try_cast<PrimitiveType>()) {
            if (prim_type->is_within_4_bytes_integer()) {
                emit_byte(OpCode::iconst_0, cur_line);
            }
            else if (prim_type->is_integer()) {
                emit_byte(OpCode::lconst, cur_line);
                emit_u64(0);
            }
            else if (prim_type->prim_kind == PrimTypeKind::F32) {
                emit_byte(OpCode::fconst, cur_line);
                emit_u32(0);
            }
            else if (prim_type->prim_kind == PrimTypeKind::F64) {
                emit_byte(OpCode::dconst, cur_line);
                emit_u64(0);
            }
            else if (prim_type->prim_kind == PrimTypeKind::String) {
                emit_byte(OpCode::iconst_nil, cur_line);
            }
            return ok();
        }
        else if (auto struct_type = type->try_cast<StructType>()) {
            // TODO: need proper struct zero initializer
            u32 count_8bytes = (struct_type->size + 7) / 8;
            u32 count_4bytes = (struct_type->size - 8 * count_8bytes + 3) / 4;
            for (u32 i = 0; i < count_8bytes; i++) {
                emit_byte(OpCode::lconst, cur_line);
                emit_u64(0);
            }
            for (u32 i = 0; i < count_4bytes; i++) {
                emit_byte(OpCode::iconst_0, cur_line);
            }
            return ok();
        }
        else if (auto fun_type = type->try_cast<FunctionType>()) {
            return unimplemented();
        }
        else {
            return error("Cannot compile expression with unassigned type!");
        }
    }

    CompileResult visit_impl(VarStmt& stmt) {
        u32 cur_line = m_scanner->get_line(stmt.var.name.get_source_loc());
        if (Expr* init_expr = stmt.initializer.get()) {
            COMP_TRY(visit(*init_expr));
        }
        else {
            // synthesize default initializer
            auto type = stmt.var.type.get();
            COMP_TRY(push_zero_initialized_value(type, cur_line));
        }
        u16 offset = m_cur_fn_env->get_local_offset(stmt.var.local_index);

        auto type = stmt.var.type.get();
        if (auto prim_type = type->try_cast<PrimitiveType>()) {
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
        else if (auto struct_type = type->try_cast<StructType>()) {
            u32 count_8bytes = (struct_type->size + 7) / 8;
            u32 count_4bytes = (struct_type->size - 8 * count_8bytes + 3) / 4;
            for (u32 i = 0; i < count_8bytes; i++) {
                emit_store_u64(offset + 2 * i, cur_line);
            }
            for (u32 i = 0; i < count_4bytes; i++) {
                emit_store_u32(offset + 2 * count_8bytes + i, cur_line);
            }
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

        u32 exit_jump;
        COMP_TRY(emit_jump_from_cond_expr(cond_expr, false, true, exit_jump));
        COMP_TRY(visit(*stmt.body));
        emit_loop(loop_start, cond_line);;

        patch_jump(exit_jump);
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
                    emit_byte(OpCode::iret, cond_line);
                }
                else {
                    emit_byte(OpCode::lret, cond_line);
                }
            }
            else {
                unimplemented();
            }
        }
        else {
            // TODO: find a way to retrieve line information
            emit_byte(OpCode::ret, -1);
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
                    emit_byte(opcode_arithmetic(prim_type->prim_kind, expr.op.type), cur_line);
                }
                else if (prim_type->is_string()) {
                    // Call concat() builtin
                    if (expr.op.type == TokenType::Plus) {
                        u16 native_fun_index = m_cur_module->find_native_function_index("concat");
                        emit_byte(OpCode::callnative, cur_line);
                        emit_u16(native_fun_index);
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
                    u32 end_jump = emit_jump(OpCode::br_false, cur_line);
                    emit_byte(OpCode::ipop, cur_line);

                    COMP_TRY(visit(*right_expr));
                    patch_jump(end_jump);
                    break;
                }
                // logical or
                case TokenType::BarBar: {
                    u32 else_jump = emit_jump(OpCode::br_false, cur_line);
                    u32 end_jump = emit_jump(OpCode::jmp, cur_line);

                    patch_jump(else_jump);
                    emit_byte(OpCode::ipop, cur_line);

                    COMP_TRY(visit(*right_expr));
                    patch_jump(end_jump);
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

    CompileResult visit_impl(TernaryExpr& expr) { return unimplemented(); }

    CompileResult visit_impl(GroupingExpr& expr) {
        return visit(*expr.expression.get());
    }

    CompileResult visit_impl(LiteralExpr& expr) {
        u32 cur_line = m_scanner->get_line(expr.get_source_loc());
        AnyValue value = expr.value;
        auto type = expr.type.get();
        switch (type->kind) {
        case TypeKind::Primitive: {
            auto& prim_type = type->cast<PrimitiveType>();
            switch (prim_type.prim_kind) {
            case PrimTypeKind::Void: return unreachable();
            case PrimTypeKind::Bool: {
                emit_byte(value.value_bool ? OpCode::iconst_1 : OpCode::iconst_0, cur_line);
                break;
            }
            case PrimTypeKind::U8:
            case PrimTypeKind::I8: {
                if (prim_type.prim_kind == PrimTypeKind::I16 && value.value_i16 == -1) {
                    emit_byte(OpCode::iconst_m1, cur_line);
                }
                else if (value.value_u16 <= 8) {
                    emit_byte((OpCode)((u32)OpCode::iconst_0 + (u32)value.value_u16), cur_line);
                }
                else {
                    emit_byte(OpCode::iconst_s, cur_line);
                    emit_byte(value.value_u8);
                }
                break;
            }
            case PrimTypeKind::U16:
            case PrimTypeKind::I16: {
                if (prim_type.prim_kind == PrimTypeKind::I16 && value.value_i16 == -1) {
                    emit_byte(OpCode::iconst_m1, cur_line);
                }
                else if (value.value_u16 <= 8) {
                    emit_byte((OpCode)((u32)OpCode::iconst_0 + (u32)value.value_u16), cur_line);
                }
                else if (value.value_u16 < 256) {
                    emit_byte(OpCode::iconst_s, cur_line);
                    emit_byte(value.value_u16);
                }
                else {
                    emit_byte(OpCode::iconst, cur_line);
                    emit_u32(value.value_u16);
                }
                break;
            }
            case PrimTypeKind::U32:
            case PrimTypeKind::I32:
            case PrimTypeKind::F32: {
                if (prim_type.prim_kind == PrimTypeKind::I32 && value.value_i32 == -1) {
                    emit_byte(OpCode::iconst_m1, cur_line);
                }
                else {
                    if (prim_type.prim_kind == PrimTypeKind::F32) {
                        emit_byte(OpCode::fconst, cur_line);
                        emit_u32(value.value_u32);
                    }
                    else {
                        if (value.value_u32 <= 8) {
                            emit_byte((OpCode)((u32)OpCode::iconst_0 + value.value_u32), cur_line);
                        }
                        else if (value.value_u32 < 256) {
                            emit_byte(OpCode::iconst_s, cur_line);
                            emit_byte(value.value_u16);
                        }
                        else {
                            emit_byte(OpCode::iconst, cur_line);
                            emit_u32(value.value_u32);
                        }
                    }
                }
                break;
            }
            case PrimTypeKind::U64:
            case PrimTypeKind::I64: {
                emit_byte(OpCode::lconst, cur_line);
                emit_u64(value.value_u64);
                break;
            }
            case PrimTypeKind::F64: {
                emit_byte(OpCode::dconst, cur_line);
                emit_u64(value.value_u64);
                break;
            }
            case PrimTypeKind::String: {
                u32 string_offset = m_cur_chunk->m_outer_module->string_table().add_string(value.str);
                emit_byte(OpCode::ldstr, cur_line);
                emit_u32(string_offset);
                break;
            }
            }
            break;
        }
        case TypeKind::Function:
        case TypeKind::Struct:
            return unimplemented();
        case TypeKind::Inferred:
        case TypeKind::Unassigned:
            return unreachable();
        }
        return ok();
    }

    CompileResult visit_impl(UnaryExpr& expr) {
        u32 cur_line = m_scanner->get_line(expr.get_source_loc());

        auto right_expr = expr.right.get();

        // TODO: Support operator overloading for custom types
        auto prim_kind = expr.type->cast<PrimitiveType>().prim_kind;
        switch (prim_kind) {
        case PrimTypeKind::U8:
        case PrimTypeKind::U16:
        case PrimTypeKind::U32:
        case PrimTypeKind::I8:
        case PrimTypeKind::I16:
        case PrimTypeKind::I32:
        case PrimTypeKind::F32: {
            emit_byte(OpCode::iconst_0, cur_line);
            COMP_TRY(visit(*right_expr));
            if (prim_kind == PrimTypeKind::F32) {
                emit_byte(OpCode::fsub, cur_line);
            }
            else {
                emit_byte(OpCode::isub, cur_line);
            }
            break;
        }
        case PrimTypeKind::U64:
        case PrimTypeKind::I64:
        case PrimTypeKind::F64: {
            emit_byte(OpCode::lconst, cur_line);
            emit_u64(0);
            COMP_TRY(visit(*right_expr));
            if (prim_kind == PrimTypeKind::F64) {
                emit_byte(OpCode::dsub, cur_line);
            }
            else {
                emit_byte(OpCode::lsub, cur_line);
            }
            break;
        }
        default:
            return unimplemented();
        }
        return ok();
    }

    CompileResult visit_impl(VariableExpr& expr) {
        u32 cur_line = m_scanner->get_line(expr.get_source_loc());

        if (AstVarDecl* var_decl = expr.var_origin.get()) {
            // Loading variables from local table
            u32 offset = m_cur_fn_env->get_local_offset(var_decl->local_index);
            auto var_type = var_decl->type.get();
            if (auto prim_type = var_type->try_cast<PrimitiveType>()) {
                if (prim_type->is_within_4_bytes()) {
                    emit_load_u32(offset, cur_line);
                }
                else {
                    if (prim_type->is_string()) {
                        emit_load_ref(offset, cur_line);
                    }
                    else {
                        emit_load_u64(offset, cur_line);
                    }
                }
            }
            else if (auto struct_type = var_type->try_cast<StructType>()) {
                return unimplemented();
            }
            else if (auto fun_type = var_type->try_cast<FunctionType>()) {
                return unimplemented();
            }
            else {
                return unreachable();
            }
        }
        else if (AstFunDecl* fun_decl = expr.fun_origin.get()) {
            // TODO: Loading function pointers from local table
            return unimplemented();
        }
        else {
            return unreachable();
        }

        return ok();
    }

    CompileResult visit_impl(CallExpr& expr) {
        u32 cur_line = m_scanner->get_line(expr.get_source_loc());
        auto callee_expr = expr.callee.get();
        if (auto var_expr = callee_expr->try_cast<VariableExpr>()) {
            if (auto fun_decl = var_expr->fun_origin.get()) {
                // If callee expr is just a function symbol, then just call the function directly!
                // Push arguments
                for (u32 i = 0; i < expr.arguments.size(); i++) {
                    auto arg_expr = expr.arguments[i].get();
                    COMP_TRY(visit(*arg_expr));
                }
                if (fun_decl->is_native) {
                    emit_byte(OpCode::callnative, cur_line);
                }
                else {
                    emit_byte(OpCode::call, cur_line);
                }
                emit_u16(fun_decl->local_index);
                return ok();
            }
        }

        // Else, callee expr is a function pointer, eval it and push it on the stack
        COMP_TRY(visit(*callee_expr));
        for (u32 i = 0; i < expr.arguments.size(); i++) {
            auto arg_expr = expr.arguments[i].get();
            COMP_TRY(visit(*arg_expr));
        }
        // TODO: need an opcode like call_fnptr
        // emit_byte(OpCode::call_fnptr, cur_line);
        // return ok();
        return unimplemented();
    }

    CompileResult visit_impl(GetExpr& expr) {
        u32 cur_line = m_scanner->get_line(expr.get_source_loc());

        auto obj_expr = expr.object.get();
        if (auto obj_var_expr = obj_expr->try_cast<VariableExpr>()) {
            auto var_origin = obj_var_expr->var_origin.get();
            u16 parent_offset = m_cur_fn_env->get_local_offset(var_origin->local_index);

            auto field_name = expr.name.str(m_scanner->source());
            auto parent_obj_type = expr.object->type.get();
            if (auto parent_struct_type = parent_obj_type->try_cast<StructType>()) {
                u32 decl_idx;
                for (decl_idx = 0; decl_idx < parent_struct_type->declarations.size(); decl_idx++) {
                    auto& decl = parent_struct_type->declarations[decl_idx];
                    auto decl_name = decl.name.str(m_scanner->source());
                    if (field_name == decl_name) {
                        break;
                    }
                }
                if (decl_idx == parent_struct_type->declarations.size()) {
                    return error("Cannot find field in struct.");
                }

                auto& found_decl = parent_struct_type->declarations[decl_idx];
                auto field_type = found_decl.type.get();
                if (auto field_prim_type = field_type->try_cast<PrimitiveType>()) {
                    if (field_prim_type->is_4_bytes()) {
                        emit_load_u32(parent_offset + found_decl.offset_bytes_from_parent / 4, cur_line);
                    }
                    else if (field_prim_type->is_8_bytes()) {
                        if (field_prim_type->is_string()) {
                            emit_load_ref(parent_offset + found_decl.offset_bytes_from_parent / 4, cur_line);
                        }
                        else {
                            emit_load_u64(parent_offset + found_decl.offset_bytes_from_parent / 4, cur_line);
                        }
                    }
                    // Else... we need to do some finicky bit manipulations that I haven't implemented yet
                    else {
                        return unimplemented();
                    }
                }
                else if (auto field_struct_type = field_type->try_cast<StructType>()) {
                    return unimplemented();
                }
                else if (auto field_fun_type = field_type->try_cast<FunctionType>()) {
                    return unimplemented();
                }
                return ok();
            }
            else {
                return unreachable();
            }
        }
        else {
            return unimplemented();
        }
    }

    CompileResult visit_impl(SetExpr& expr) {
        u32 cur_line = m_scanner->get_line(expr.get_source_loc());

        auto obj_expr = expr.object.get();
        if (auto obj_var_expr = obj_expr->try_cast<VariableExpr>()) {
            auto var_origin = obj_var_expr->var_origin.get();
            u16 parent_offset = m_cur_fn_env->get_local_offset(var_origin->local_index);

            COMP_TRY(visit(*expr.value));

            auto field_name = expr.name.str(m_scanner->source());
            auto parent_obj_type = expr.object->type.get();
            if (auto parent_struct_type = parent_obj_type->try_cast<StructType>()) {
                u32 decl_idx;
                for (decl_idx = 0; decl_idx < parent_struct_type->declarations.size(); decl_idx++) {
                    auto& decl = parent_struct_type->declarations[decl_idx];
                    auto decl_name = decl.name.str(m_scanner->source());
                    if (field_name == decl_name) {
                        break;
                    }
                }
                if (decl_idx == parent_struct_type->declarations.size()) {
                    return error("Cannot find field in struct.");
                }

                const auto& found_decl = parent_struct_type->declarations[decl_idx];
                auto field_type = found_decl.type.get();
                if (auto field_prim_type = field_type->try_cast<PrimitiveType>()) {
                    // If offset is aligned to 4 bytes, we can use istore
                    if (field_prim_type->is_4_bytes()) {
                        emit_store_u32(parent_offset + found_decl.offset_bytes_from_parent / 4, cur_line);
                    }
                    else if (field_prim_type->is_8_bytes()) {
                        if (field_prim_type->is_string()) {
                            emit_store_ref(parent_offset + found_decl.offset_bytes_from_parent / 4, cur_line);
                        }
                        else {
                            emit_store_u64(parent_offset + found_decl.offset_bytes_from_parent / 4, cur_line);
                        }
                    }
                    else {
                        // Else... we need to do some finicky bit manipulations that I haven't implemented yet
                        return unimplemented();
                    }
                }
                else if (auto field_struct_type = field_type->try_cast<StructType>()) {
                    return unimplemented();
                }
                else if (auto field_fun_type = field_type->try_cast<FunctionType>()) {
                    return unimplemented();
                }
                return ok();
            }
            else {
                return unimplemented();
            }
        }
        else {
            unimplemented();
        }
    }

    static inline CompileResult ok() { return {CompileResultType::Ok, ""}; }

    static inline CompileResult unimplemented() {
        __debugbreak();
        return {CompileResultType::Unimplemented, "Unimplemented code"};
    }

    static inline CompileResult unreachable() {
        __debugbreak();
        return {CompileResultType::Unreachable, "Unreachable code"};
    }

    static inline CompileResult error(std::string message) {
        __debugbreak();
        return {CompileResultType::Error, std::move(message)};
    }

private:
    void emit_byte(OpCode opcode, u32 line) {
        assert(opcode != OpCode::invalid);
        m_cur_chunk->write((u8)opcode, line);
    }

    void emit_byte(u8 value) {
        m_cur_chunk->write(value, -1);
    }

    void emit_u16(u16 value) {
        emit_byte(value & 0xff);
        emit_byte((value >> 8) & 0xff);
    }

    void emit_u32(u32 value) {
        emit_byte(value & 0xff);
        emit_byte((value >> 8) & 0xff);
        emit_byte((value >> 16) & 0xff);
        emit_byte((value >> 24) & 0xff);
    }

    void emit_u64(u64 value) {
        emit_byte(value & 0xff);
        emit_byte((value >> 8) & 0xff);
        emit_byte((value >> 16) & 0xff);
        emit_byte((value >> 24) & 0xff);
        emit_byte((value >> 32) & 0xff);
        emit_byte((value >> 40) & 0xff);
        emit_byte((value >> 48) & 0xff);
        emit_byte((value >> 56) & 0xff);
    }

    u32 emit_jump(OpCode opcode, u32 line) {
        emit_byte(opcode, line);
        emit_u32(0xffffffff);
        return m_cur_chunk->m_bytecode.size() - 4;
    }

    void patch_jump(u32 offset) {
        u32 jump = m_cur_chunk->m_bytecode.size() - offset - 4;
        // TODO: use br_s instead when jump < UINT16_MAX
        m_cur_chunk->m_bytecode[offset + 0] = jump & 0xff;
        m_cur_chunk->m_bytecode[offset + 1] = (jump >> 8) & 0xff;
        m_cur_chunk->m_bytecode[offset + 2] = (jump >> 16) & 0xff;
        m_cur_chunk->m_bytecode[offset + 3] = (jump >> 24) & 0xff;
    }

    void emit_loop(u32 loop_start, u32 line) {
        emit_byte(OpCode::loop, line);

        u32 offset = m_cur_chunk->m_bytecode.size() - loop_start + 4;
        emit_u32(offset);
    }

    void emit_load_u32(u32 offset, u32 line) {
        if (offset <= UINT8_MAX) {
            if (offset < 4) {
                emit_byte((OpCode)((u32)OpCode::iload_0 + offset), line);
            }
            else {
                emit_byte(OpCode::iload_s, line);
                emit_byte(offset);
            }
        }
        else {
            emit_byte(OpCode::iload, line);
            emit_u16((u16)offset);
        }
    }

    void emit_load_u64(u32 offset, u32 line) {
        offset >>= 1;
        if (offset <= UINT8_MAX) {
            if (offset < 4) {
                emit_byte((OpCode)((u32)OpCode::lload_0 + offset), line);
            }
            else {
                emit_byte(OpCode::lload_s, line);
                emit_byte(offset);
            }
        }
        else {
            emit_byte(OpCode::lload, line);
            emit_u16((u16)offset);
        }
    }

    void emit_load_ref(u32 offset, u32 line) {
        offset >>= 1;
        if (offset <= UINT8_MAX) {
            if (offset < 4) {
                emit_byte((OpCode)((u32)OpCode::rload_0 + offset), line);
            }
            else {
                emit_byte(OpCode::rload_s, line);
                emit_byte(offset);
            }
        }
        else {
            emit_byte(OpCode::rload, line);
            emit_u16((u16)offset);
        }
    }

    void emit_store_u32(u16 offset, u32 cur_line) {
        if (offset < 256) {
            if (offset < 4) {
                emit_byte((OpCode)((u32)OpCode::istore_0 + (u32)offset), cur_line);
            }
            else {
                emit_byte(OpCode::istore_s, cur_line);
                emit_byte((u8)offset);
            }
        }
        else {
            emit_byte(OpCode::istore, cur_line);
            emit_u16(offset);
        }
    }

    void emit_store_u64(u16 offset, u32 cur_line) {
        offset >>= 1;
        if (offset < 256) {
            if (offset < 4) {
                emit_byte((OpCode)((u32)OpCode::lstore_0 + (u32)offset), cur_line);
            }
            else {
                emit_byte(OpCode::lstore_s, cur_line);
                emit_byte((u8)offset);
            }
        }
        else {
            emit_byte(OpCode::lstore, cur_line);
            emit_u16(offset);
        }
    }

    void emit_store_ref(u16 offset, u32 cur_line) {
        offset >>= 1;
        if (offset < 256) {
            if (offset < 4) {
                emit_byte((OpCode)((u32)OpCode::rstore_0 + (u32)offset), cur_line);
            }
            else {
                emit_byte(OpCode::rstore_s, cur_line);
                emit_byte((u8)offset);
            }
        }
        else {
            emit_byte(OpCode::rstore, cur_line);
            emit_u16(offset);
        }
    }
};
}
