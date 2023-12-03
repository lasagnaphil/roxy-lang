#pragma once

#include "roxy/ast_visitor.hpp"
#include "roxy/opcode.hpp"
#include "roxy/chunk.hpp"
#include "roxy/scanner.hpp"

namespace rx {

enum CompileResult {
    Ok, Error, Unimplemented
};

class FnLocalEnv {
private:
    FnLocalEnv* m_outer;

    Vector<u16> m_local_offsets;

public:
    FnLocalEnv(FunctionStmt& stmt) {
        init_from_locals(stmt.locals);
    }
    FnLocalEnv(ModuleStmt& stmt) {
        init_from_locals(stmt.locals);
    }

    FnLocalEnv* get_outer_env() { return m_outer; }

    u16 get_local_offset(u32 local_index) const { return m_local_offsets[local_index]; }

private:
    void init_from_locals(RelSpan<RelPtr<AstVarDecl>>& locals) {
        m_local_offsets.resize(locals.size());
        u32 offset = 0;
        for (u32 i = 0; i < locals.size(); i++) {
            auto& local = locals[i];
            auto type = local->type.get();
            m_local_offsets[i] = offset;
            u32 aligned_size = (offset + type->alignment) & ~type->alignment;
            offset = aligned_size + type->size;
        }
    }
};

class Compiler :
        public StmtVisitorBase<Compiler, CompileResult>,
        public ExprVisitorBase<Compiler, CompileResult> {

private:
    Scanner* m_scanner;
    Chunk* m_cur_chunk;
    FnLocalEnv* m_cur_fn_env;

public:
    using StmtVisitorBase<Compiler, CompileResult>::visit;
    using ExprVisitorBase<Compiler, CompileResult>::visit;

#define COMP_TRY(EXPR) do { auto _res = EXPR; if (_res != CompileResult::Ok) return _res; } while (false);

    Compiler(Scanner* scanner) : m_scanner(scanner) {}

    CompileResult compile(ModuleStmt& stmt, Chunk& out_chunk)
    {
        m_cur_chunk = &out_chunk;

        COMP_TRY(visit(stmt));
        emit_byte(OpCode::Ret, -1);

        m_cur_chunk = nullptr;
        m_cur_fn_env = nullptr;

        return ok();
    }

    CompileResult visit_impl(ErrorStmt& stmt) { return ok(); }
    CompileResult visit_impl(BlockStmt& stmt) {
        for (auto& inner_stmt : stmt.statements) {
            COMP_TRY(visit(*inner_stmt.get()));
        }
        return ok();
    }
    CompileResult visit_impl(ModuleStmt& stmt) {
        FnLocalEnv module_env(stmt);
        m_cur_fn_env = &module_env;

        for (auto& inner_stmt : stmt.statements) {
            COMP_TRY(visit(*inner_stmt.get()));
        }

        m_cur_fn_env = module_env.get_outer_env();
        return ok();
    }

    CompileResult visit_impl(ExpressionStmt& stmt) {
        return visit(*stmt.expr.get());
    }

    CompileResult visit_impl(StructStmt& stmt) { return ok(); }

    CompileResult visit_impl(FunctionStmt& stmt) {
        FnLocalEnv fn_env(stmt);
        m_cur_fn_env = &fn_env;

        for (auto& body_stmt : stmt.body) {
            COMP_TRY(visit(*body_stmt));
        }

        m_cur_fn_env = fn_env.get_outer_env();
        return ok();
    }

    CompileResult visit_impl(IfStmt& stmt) { return unimplemented(); }

    CompileResult visit_impl(PrintStmt& stmt) {
        Expr* expr = stmt.expr.get();
        COMP_TRY(visit(*expr));
        u32 cur_line = m_scanner->get_line(expr->get_source_loc());
        emit_byte(OpCode::Print, cur_line);
        return ok();
    }

    CompileResult visit_impl(VarStmt& stmt) {
        u32 cur_line = m_scanner->get_line(stmt.var.name.get_source_loc());
        if (Expr* init_expr = stmt.initializer.get()) {
            COMP_TRY(visit(*init_expr));
        }
        else {
            // synthesize default initializer
            auto type = stmt.var.type.get();
            switch (type->kind) {
                case TypeKind::Primitive: {
                    auto& prim_type = type->cast<PrimitiveType>();
                    switch (prim_type.prim_kind) {
                        case PrimTypeKind::Void: return error();
                        case PrimTypeKind::Bool:
                        case PrimTypeKind::U8: case PrimTypeKind::I8:
                        case PrimTypeKind::U16: case PrimTypeKind::I16:
                        case PrimTypeKind::U32: case PrimTypeKind::I32: case PrimTypeKind::F32: {
                            emit_byte(OpCode::LdC_i4_0, cur_line); break;
                        }
                        case PrimTypeKind::U64: case PrimTypeKind::I64: case PrimTypeKind::F64: {
                            emit_byte(OpCode::LdC_i8, cur_line);
                            emit_u64(0);
                            break;
                        }
                        case PrimTypeKind::String: {
                            emit_byte(OpCode::LdC_Null, cur_line); break;
                        }
                    }
                }
                case TypeKind::Function:
                case TypeKind::Struct:
                    return unimplemented();
                case TypeKind::Inferred:
                case TypeKind::Unassigned:
                    return error();
            }
        }
        auto type = stmt.var.type.get();
        switch (type->kind) {
            case TypeKind::Primitive: {
                u16 local_index = stmt.var.local_index;
                auto& prim_type = type->cast<PrimitiveType>();
                switch (prim_type.prim_kind) {
                    case PrimTypeKind::U8: case PrimTypeKind::U16: case PrimTypeKind::U32:
                    case PrimTypeKind::I8: case PrimTypeKind::I16: case PrimTypeKind::I32:
                    case PrimTypeKind::F32: {
                        if (local_index < 256) {
                            if (local_index < 4) {
                                emit_byte((OpCode)((u32)OpCode::St_Loc_0 + (u32)local_index), cur_line);
                            }
                            else {
                                emit_byte(OpCode::St_Loc_S, cur_line);
                                emit_byte((u8)local_index);
                            }
                        }
                        else {
                            emit_byte(OpCode::St_Loc, cur_line);
                            emit_u16(local_index);
                        }
                        break;
                    }
                    case PrimTypeKind::U64: case PrimTypeKind::I64: case PrimTypeKind::F64: {
                        if (local_index < 256) {
                            if (local_index < 4) {
                                emit_byte((OpCode)((u32)OpCode::St_Loc_i8 + (u32)local_index), cur_line);
                            }
                            else {
                                emit_byte(OpCode::St_Loc_i8_S, cur_line);
                                emit_byte((u8)local_index);
                            }
                        }
                        else {
                            emit_byte(OpCode::St_Loc_i8, cur_line);
                            emit_u16(local_index);
                        }
                        break;
                    }
                    default:
                        unimplemented();
                }
                break;
            }
            case TypeKind::Function:
            case TypeKind::Struct:
                return unimplemented();
            case TypeKind::Inferred:
            case TypeKind::Unassigned:
                return error();
        }

        return ok();
    }

    CompileResult visit_impl(WhileStmt& stmt) { return unimplemented(); }
    CompileResult visit_impl(ReturnStmt& stmt) { return unimplemented(); }
    CompileResult visit_impl(BreakStmt& stmt) { return unimplemented(); }
    CompileResult visit_impl(ContinueStmt& stmt) { return unimplemented(); }

    CompileResult visit_impl(ErrorExpr& expr) { return error(); }
    CompileResult visit_impl(AssignExpr& expr) {
        u32 cur_line = m_scanner->get_line(expr.name.get_source_loc());
        AstVarDecl* var_decl = expr.origin.get();
        u16 offset = m_cur_fn_env->get_local_offset(var_decl->local_index);
        auto& prim_type = expr.type->cast<PrimitiveType>();
        switch (prim_type.prim_kind) {
            case PrimTypeKind::U8: case PrimTypeKind::U16: case PrimTypeKind::U32:
            case PrimTypeKind::I8: case PrimTypeKind::I16: case PrimTypeKind::I32:
            case PrimTypeKind::F32: {
                if (offset <= UINT8_MAX) {
                    if (offset < 4) {
                        emit_byte((OpCode)((u32)OpCode::St_Loc_0 + (u32)offset), cur_line);
                    }
                    else {
                        emit_byte(OpCode::St_Loc_S, cur_line);
                        emit_byte(offset);
                    }
                }
                else {
                    emit_byte(OpCode::St_Loc, cur_line);
                    emit_u16(offset);
                }
                break;
            }
            case PrimTypeKind::U64: case PrimTypeKind::I64: case PrimTypeKind::F64: {
                if (offset <= UINT8_MAX) {
                    if (offset < 4) {
                        emit_byte((OpCode)((u32)OpCode::St_Loc_i8_0 + (u32)offset), cur_line);
                    }
                    else {
                        emit_byte(OpCode::St_Loc_i8_S, cur_line);
                        emit_byte(offset);
                    }
                }
                else {
                    emit_byte(OpCode::St_Loc_i8, cur_line);
                    emit_u16((u16) offset);
                }
                break;
            }
            default:
                unimplemented();
        }

        return ok();
    }

    CompileResult visit_impl(BinaryExpr& expr) {
        Expr* left_expr = expr.left.get();
        Expr* right_expr = expr.right.get();
        COMP_TRY(visit(*left_expr));
        COMP_TRY(visit(*right_expr));

        u32 cur_line = m_scanner->get_line(expr.get_source_loc());

        // TODO: Support operator overloading for custom types
        PrimTypeKind prim_kind = expr.type->cast<PrimitiveType>().prim_kind;

        switch (expr.op.type) {
            case TokenType::Minus:
                switch (prim_kind) {
                    case PrimTypeKind::U8: case PrimTypeKind::U16: case PrimTypeKind::U32:
                    case PrimTypeKind::I8: case PrimTypeKind::I16: case PrimTypeKind::I32:
                        emit_byte(OpCode::Sub_i4, cur_line); break;
                    case PrimTypeKind::U64: case PrimTypeKind::I64:
                        emit_byte(OpCode::Sub_i8, cur_line); break;
                    case PrimTypeKind::F32:
                        emit_byte(OpCode::Sub_r4, cur_line); break;
                    case PrimTypeKind::F64:
                        emit_byte(OpCode::Sub_r8, cur_line); break;
                    default:
                        return error();
                }
                break;
            case TokenType::Plus:
                switch (prim_kind) {
                    case PrimTypeKind::U8: case PrimTypeKind::U16: case PrimTypeKind::U32:
                    case PrimTypeKind::I8: case PrimTypeKind::I16: case PrimTypeKind::I32:
                        emit_byte(OpCode::Add_i4, cur_line); break;
                    case PrimTypeKind::U64: case PrimTypeKind::I64:
                        emit_byte(OpCode::Add_i8, cur_line); break;
                    case PrimTypeKind::F32:
                        emit_byte(OpCode::Add_r4, cur_line); break;
                    case PrimTypeKind::F64:
                        emit_byte(OpCode::Add_r8, cur_line); break;
                    default:
                        return error();
                }
                break;
            case TokenType::Star:
                switch (prim_kind) {
                    case PrimTypeKind::I8: case PrimTypeKind::I16: case PrimTypeKind::I32:
                        emit_byte(OpCode::Mul_i4, cur_line); break;
                    case PrimTypeKind::U8: case PrimTypeKind::U16: case PrimTypeKind::U32:
                        emit_byte(OpCode::Mul_u4, cur_line); break;
                    case PrimTypeKind::I64:
                        emit_byte(OpCode::Mul_i8, cur_line); break;
                    case PrimTypeKind::U64:
                        emit_byte(OpCode::Mul_u8, cur_line); break;
                    case PrimTypeKind::F32:
                        emit_byte(OpCode::Mul_r4, cur_line); break;
                    case PrimTypeKind::F64:
                        emit_byte(OpCode::Mul_r8, cur_line); break;
                    default:
                        return error();
                }
                break;
            case TokenType::Slash:
                switch (prim_kind) {
                    case PrimTypeKind::I8: case PrimTypeKind::I16: case PrimTypeKind::I32:
                        emit_byte(OpCode::Div_i4, cur_line); break;
                    case PrimTypeKind::U8: case PrimTypeKind::U16: case PrimTypeKind::U32:
                        emit_byte(OpCode::Div_u4, cur_line); break;
                    case PrimTypeKind::I64:
                        emit_byte(OpCode::Div_i8, cur_line); break;
                    case PrimTypeKind::U64:
                        emit_byte(OpCode::Div_u8, cur_line); break;
                    case PrimTypeKind::F32:
                        emit_byte(OpCode::Div_r4, cur_line); break;
                    case PrimTypeKind::F64:
                        emit_byte(OpCode::Div_r8, cur_line); break;
                    default:
                        return error();
                }
                break;
            default:
                return error();
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
                    case PrimTypeKind::Void: return error();
                    case PrimTypeKind::Bool: {
                        emit_byte(value.value_bool? OpCode::LdC_i4_1 : OpCode::LdC_i4_0, cur_line); break;
                    }
                    case PrimTypeKind::U8: case PrimTypeKind::I8: {
                        emit_byte(OpCode::LdC_i4_S, cur_line);
                        emit_byte(value.value_u8);
                        break;
                    }
                    case PrimTypeKind::U16: case PrimTypeKind::I16: {
                        if (prim_type.prim_kind == PrimTypeKind::I16 && value.value_i16 == -1) {
                            emit_byte(OpCode::LdC_i4_M1, cur_line);
                        }
                        else if (value.value_u16 <= 8) {
                            emit_byte((OpCode)((u32)OpCode::LdC_i4_0 + (u32)value.value_u16), cur_line);
                        }
                        else {
                            emit_byte(OpCode::LdC_i4, cur_line);
                            emit_u32(value.value_u16);
                        }
                        break;
                    }
                    case PrimTypeKind::U32: case PrimTypeKind::I32: case PrimTypeKind::F32: {
                        if (prim_type.prim_kind == PrimTypeKind::I32 && value.value_i32 == -1) {
                            emit_byte(OpCode::LdC_i4_M1, cur_line);
                        }
                        else if (prim_type.prim_kind != PrimTypeKind::F32 && value.value_u32 <= 8) {
                            emit_byte((OpCode)((u32)OpCode::LdC_i4_0 + value.value_u32), cur_line);
                        }
                        else {
                            emit_byte(OpCode::LdC_i4, cur_line);
                            emit_u32(value.value_u32);
                        }
                        break;
                    }
                    case PrimTypeKind::U64: case PrimTypeKind::I64: case PrimTypeKind::F64: {
                        emit_byte(OpCode::LdC_i8, cur_line);
                        emit_u64(value.value_u64);
                        break;
                    }
                    case PrimTypeKind::String: {
                        u32 string_offset = m_cur_chunk->add_string(value.str);
                        emit_byte(OpCode::LdStr, cur_line);
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
                return error();
        }
        return ok();
    }

    CompileResult visit_impl(UnaryExpr& expr) {
        u32 cur_line = m_scanner->get_line(expr.get_source_loc());

        auto right_expr = expr.right.get();

        // TODO: Support operator overloading for custom types
        auto prim_kind = expr.type->cast<PrimitiveType>().prim_kind;
        switch (prim_kind) {
            case PrimTypeKind::U8: case PrimTypeKind::U16: case PrimTypeKind::U32:
            case PrimTypeKind::I8: case PrimTypeKind::I16: case PrimTypeKind::I32:
            case PrimTypeKind::F32: {
                emit_byte(OpCode::LdC_i4_0, cur_line);
                COMP_TRY(visit(*right_expr));
                if (prim_kind == PrimTypeKind::F32) {
                    emit_byte(OpCode::Sub_r4, cur_line);
                }
                else {
                    emit_byte(OpCode::Sub_i4, cur_line);
                }
                break;
            }
            case PrimTypeKind::U64: case PrimTypeKind::I64: case PrimTypeKind::F64: {
                emit_byte(OpCode::LdC_i8, cur_line);
                emit_u64(0);
                COMP_TRY(visit(*right_expr));
                if (prim_kind == PrimTypeKind::F64) {
                    emit_byte(OpCode::Sub_r8, cur_line);
                }
                else {
                    emit_byte(OpCode::Sub_i8, cur_line);
                }
                break;
            }
            default:
                return error();
        }
        return ok();
    }

    CompileResult visit_impl(VariableExpr& expr) {
        u32 cur_line = m_scanner->get_line(expr.get_source_loc());
        AstVarDecl* var_decl = expr.origin.get();
        u32 offset = m_cur_fn_env->get_local_offset(var_decl->local_index);
        auto prim_kind = var_decl->type->cast<PrimitiveType>().prim_kind;
        switch (prim_kind) {
            case PrimTypeKind::U8: case PrimTypeKind::U16: case PrimTypeKind::U32:
            case PrimTypeKind::I8: case PrimTypeKind::I16: case PrimTypeKind::I32:
            case PrimTypeKind::F32: {
                if (offset <= UINT8_MAX) {
                    if (offset < 4) {
                        if (offset == 0)       emit_byte(OpCode::Ld_Loc_0, cur_line);
                        else if (offset == 1)  emit_byte(OpCode::Ld_Loc_1, cur_line);
                        else if (offset == 2)  emit_byte(OpCode::Ld_Loc_2, cur_line);
                        else if (offset == 3)  emit_byte(OpCode::Ld_Loc_3, cur_line);
                    }
                    else {
                        emit_byte(OpCode::Ld_Loc_S, cur_line);
                        emit_byte(offset);
                    }
                }
                else {
                    emit_byte(OpCode::Ld_Loc, cur_line);
                    emit_u16((u16) offset);
                }
                break;
            }
            case PrimTypeKind::U64: case PrimTypeKind::I64: case PrimTypeKind::F64: {
                if (offset <= UINT8_MAX) {
                    if (offset < 4) {
                        if (offset == 0)       emit_byte(OpCode::Ld_Loc_i8_0, cur_line);
                        else if (offset == 1)  emit_byte(OpCode::Ld_Loc_i8_1, cur_line);
                        else if (offset == 2)  emit_byte(OpCode::Ld_Loc_i8_2, cur_line);
                        else if (offset == 3)  emit_byte(OpCode::Ld_Loc_i8_3, cur_line);
                    }
                    else {
                        emit_byte(OpCode::Ld_Loc_i8_S, cur_line);
                        emit_byte(offset);
                    }
                }
                else {
                    emit_byte(OpCode::Ld_Loc_i8, cur_line);
                    emit_u16((u16) offset);
                }
                break;
            }
            default:
                unimplemented();
        }

        return ok();
    }

    CompileResult visit_impl(CallExpr& expr) {
        u32 cur_line = m_scanner->get_line(expr.get_source_loc());
        auto callee_expr = expr.callee.get();
        COMP_TRY(visit(*callee_expr));
        for (u32 i = 0; i < expr.arguments.size(); i++) {
            auto arg_expr = expr.arguments[i].get();
            COMP_TRY(visit(*arg_expr));
        }
        emit_byte(OpCode::Call, cur_line);
        return ok();
    }

    CompileResult visit_impl(GetExpr& expr) { return unimplemented(); }

    CompileResult visit_impl(SetExpr& expr) { return unimplemented(); }

    static inline CompileResult ok() { return CompileResult::Ok; }
    static inline CompileResult unimplemented() { return CompileResult::Unimplemented; }
    static inline CompileResult error() { return CompileResult::Error; }

private:
    void emit_byte(OpCode opcode, u32 line) {
        m_cur_chunk->write((u8)opcode, line);

    }
    void emit_byte(u8 value) {
        m_cur_chunk->write(value, -1);
    }

    void emit_u16(u16 value) {
        emit_byte((value >> 8) & 0xff);
        emit_byte(value & 0xff);
    }

    void emit_u32(u32 value) {
        emit_byte((value >> 24) & 0xff);
        emit_byte((value >> 16) & 0xff);
        emit_byte((value >> 8) & 0xff);
        emit_byte(value & 0xff);
    }

    void emit_u64(u64 value) {
        emit_byte((value >> 56) & 0xff);
        emit_byte((value >> 48) & 0xff);
        emit_byte((value >> 40) & 0xff);
        emit_byte((value >> 32) & 0xff);
        emit_byte((value >> 24) & 0xff);
        emit_byte((value >> 16) & 0xff);
        emit_byte((value >> 8) & 0xff);
        emit_byte(value & 0xff);
    }
};

}