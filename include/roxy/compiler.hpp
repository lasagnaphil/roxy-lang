#pragma once

#include "roxy/ast_visitor.hpp"
#include "roxy/opcode.hpp"

namespace rx {

struct UntypedValue {
    union {
        bool value_bool;
        u8 value_u8;
        u16 value_u16;
        u32 value_u32;
        u64 value_u64;
        i8 value_i8;
        i16 value_i16;
        i32 value_i32;
        i64 value_i64;
        f32 value_f32;
        f64 value_f64;
        Obj* obj;
        u8 bytes[4];
    };
};

class ConstantTable {
public:
    u32 add_string(std::string_view str) {
        u32 offset = (u32)m_string_buf.size();
        m_string_buf += str;
        return offset;
    }

    u32 add_value(UntypedValue value) {
        u32 offset = m_values.size();
        m_values.push_back(value);
        return offset;
    }

private:
    std::string m_string_buf;
    Vector<UntypedValue> m_values;
};

enum CompileResult {
    Ok, Error, Unimplemented
};

class FnLocalEnv {
private:
    FnLocalEnv* m_outer;

    Vector<u32> m_local_offsets;

public:
    FnLocalEnv(FunctionStmt& stmt) {
        m_local_offsets.resize(stmt.locals.size());
        u32 offset = 0;
        for (u32 i = 0; i < stmt.locals.size(); i++) {
            auto& local = stmt.locals[i];
            auto type = local->type.get();
            // Align parameters with 4 bytes (adhering to stack convention)
            m_local_offsets[i] = offset;
            u32 aligned_size = (offset + 3) & ~3;
            offset = aligned_size + type->size;
        }
    }

    FnLocalEnv* get_outer_env() { return m_outer; }

    u32 get_local_offset(u32 local_index) const { return m_local_offsets[local_index]; }
};

class Compiler :
        public StmtVisitorBase<Compiler, CompileResult>,
        public ExprVisitorBase<Compiler, CompileResult> {

private:
    Vector<u8> m_bytecode;
    ConstantTable m_constant_table;
    FnLocalEnv* m_cur_fn_env;

public:
    using StmtVisitorBase<Compiler, CompileResult>::visit;
    using ExprVisitorBase<Compiler, CompileResult>::visit;

#define COMP_TRY(EXPR) do { auto _res = EXPR; if (_res != CompileResult::Ok) return _res; } while (false);

    Compiler() {}

    CompileResult visit_impl(ErrorStmt& stmt) { return ok(); }
    CompileResult visit_impl(BlockStmt& stmt) {
        for (auto& inner_stmt : stmt.statements) {
            auto _ = visit(*inner_stmt.get());
        }
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
            auto _ = visit(*body_stmt);
        }

        m_cur_fn_env = fn_env.get_outer_env();
    }

    CompileResult visit_impl(IfStmt& stmt) { return unimplemented(); }

    CompileResult visit_impl(PrintStmt& stmt) { return unimplemented(); }

    CompileResult visit_impl(VarStmt& stmt) {
        if (Expr* init_expr = stmt.initializer.get()) {
            COMP_TRY(visit(*init_expr));
        }
        else {
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
                            emit_byte(OpCode::LdC_i4_0); break;
                        }
                        case PrimTypeKind::U64: case PrimTypeKind::I64: case PrimTypeKind::F64: {
                            emit_byte(OpCode::LdC_i8);
                            emit_u64(0);
                            break;
                        }
                        case PrimTypeKind::String: {
                            emit_byte(OpCode::LdC_Null); break;
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
    }

    CompileResult visit_impl(WhileStmt& stmt) { return unimplemented(); }
    CompileResult visit_impl(ReturnStmt& stmt) { return unimplemented(); }
    CompileResult visit_impl(BreakStmt& stmt) { return unimplemented(); }
    CompileResult visit_impl(ContinueStmt& stmt) { return unimplemented(); }

    CompileResult visit_impl(ErrorExpr& expr) { return error(); }
    CompileResult visit_impl(AssignExpr& expr) {
        AstVarDecl* var_decl = expr.origin.get();
        u32 offset = m_cur_fn_env->get_local_offset(var_decl->local_index);
        if (offset <= UINT8_MAX) {
            if (offset < 4) {
                if (offset == 0)       emit_byte(OpCode::St_Loc_0);
                else if (offset == 1)  emit_byte(OpCode::St_Loc_1);
                else if (offset == 2)  emit_byte(OpCode::St_Loc_2);
                else if (offset == 3)  emit_byte(OpCode::St_Loc_3);
            }
            else {
                emit_bytes(OpCode::St_Loc_S, (u8)offset);
            }
        }
        else {
            emit_byte(OpCode::St_Loc);
            emit_u16((u16) offset);
        }
    }

    CompileResult visit_impl(BinaryExpr& expr) {
        Expr* left_expr = expr.left.get();
        Expr* right_expr = expr.right.get();
        COMP_TRY(visit(*left_expr));
        COMP_TRY(visit(*right_expr));

        // TODO: Support operator overloading for custom types
        PrimTypeKind prim_kind = expr.type->cast<PrimitiveType>().prim_kind;

        switch (expr.op.type) {
            case TokenType::Minus:
                switch (prim_kind) {
                    case PrimTypeKind::U8: case PrimTypeKind::U16: case PrimTypeKind::U32:
                    case PrimTypeKind::I8: case PrimTypeKind::I16: case PrimTypeKind::I32:
                        emit_byte(OpCode::Sub_i4); break;
                    case PrimTypeKind::U64: case PrimTypeKind::I64:
                        emit_byte(OpCode::Sub_i8); break;
                    case PrimTypeKind::F32:
                        emit_byte(OpCode::Sub_r4); break;
                    case PrimTypeKind::F64:
                        emit_byte(OpCode::Sub_r8); break;
                    default:
                        return error();
                }
                break;
            case TokenType::Plus:
                switch (prim_kind) {
                    case PrimTypeKind::U8: case PrimTypeKind::U16: case PrimTypeKind::U32:
                    case PrimTypeKind::I8: case PrimTypeKind::I16: case PrimTypeKind::I32:
                        emit_byte(OpCode::Add_i4); break;
                    case PrimTypeKind::U64: case PrimTypeKind::I64:
                        emit_byte(OpCode::Add_i8); break;
                    case PrimTypeKind::F32:
                        emit_byte(OpCode::Add_r4); break;
                    case PrimTypeKind::F64:
                        emit_byte(OpCode::Add_r8); break;
                    default:
                        return error();
                }
                break;
            case TokenType::Star:
                switch (prim_kind) {
                    case PrimTypeKind::I8: case PrimTypeKind::I16: case PrimTypeKind::I32:
                        emit_byte(OpCode::Mul_i4); break;
                    case PrimTypeKind::U8: case PrimTypeKind::U16: case PrimTypeKind::U32:
                        emit_byte(OpCode::Mul_u4); break;
                    case PrimTypeKind::I64:
                        emit_byte(OpCode::Mul_i8); break;
                    case PrimTypeKind::U64:
                        emit_byte(OpCode::Mul_u8); break;
                    case PrimTypeKind::F32:
                        emit_byte(OpCode::Mul_r4); break;
                    case PrimTypeKind::F64:
                        emit_byte(OpCode::Mul_r8); break;
                    default:
                        return error();
                }
                break;
            case TokenType::Slash:
                switch (prim_kind) {
                    case PrimTypeKind::I8: case PrimTypeKind::I16: case PrimTypeKind::I32:
                        emit_byte(OpCode::Div_i4); break;
                    case PrimTypeKind::U8: case PrimTypeKind::U16: case PrimTypeKind::U32:
                        emit_byte(OpCode::Div_u4); break;
                    case PrimTypeKind::I64:
                        emit_byte(OpCode::Div_i8); break;
                    case PrimTypeKind::U64:
                        emit_byte(OpCode::Div_u8); break;
                    case PrimTypeKind::F32:
                        emit_byte(OpCode::Div_r4); break;
                    case PrimTypeKind::F64:
                        emit_byte(OpCode::Div_r8); break;
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
        AnyValue value = expr.value;
        auto type = expr.type.get();
        switch (type->kind) {
            case TypeKind::Primitive: {
                auto& prim_type = type->cast<PrimitiveType>();
                switch (prim_type.prim_kind) {
                    case PrimTypeKind::Void: return error();
                    case PrimTypeKind::Bool: {
                        emit_byte(value.value_bool? OpCode::LdC_i4_1 : OpCode::LdC_i4_0); break;
                    }
                    case PrimTypeKind::U8: case PrimTypeKind::I8: {
                        emit_bytes(OpCode::LdC_i4_S, value.value_u8); break;
                    }
                    case PrimTypeKind::U16: case PrimTypeKind::I16: {
                        emit_byte(OpCode::LdC_i4);
                        emit_u32((u32)value.value_u16);
                        break;
                    }
                    case PrimTypeKind::U32: case PrimTypeKind::I32: case PrimTypeKind::F32: {
                        emit_byte(OpCode::LdC_i4);
                        emit_u32(value.value_u32);
                        break;
                    }
                    case PrimTypeKind::U64: case PrimTypeKind::I64: case PrimTypeKind::F64: {
                        emit_byte(OpCode::LdC_i4);
                        emit_u64(value.value_u64);
                        break;
                    }
                    case PrimTypeKind::String: {
                        u32 string_offset = m_constant_table.add_string(value.str);
                        emit_byte(OpCode::LdStr);
                        emit_u32(string_offset);
                        break;
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
        return ok();
    }

    CompileResult visit_impl(UnaryExpr& expr) {
        auto right_expr = expr.right.get();

        // TODO: Support operator overloading for custom types
        auto prim_kind = expr.type->cast<PrimitiveType>().prim_kind;
        switch (prim_kind) {
            case PrimTypeKind::U8: case PrimTypeKind::U16: case PrimTypeKind::U32:
            case PrimTypeKind::I8: case PrimTypeKind::I16: case PrimTypeKind::I32:
            case PrimTypeKind::F32: {
                emit_byte(OpCode::LdC_i4_0);
                COMP_TRY(visit(*right_expr));
                if (prim_kind == PrimTypeKind::F32) {
                    emit_byte(OpCode::Sub_r4);
                }
                else {
                    emit_byte(OpCode::Sub_i4);
                }
                break;
            }
            case PrimTypeKind::U64: case PrimTypeKind::I64: case PrimTypeKind::F64: {
                emit_byte(OpCode::LdC_i8);
                emit_u64(0);
                COMP_TRY(visit(*right_expr));
                if (prim_kind == PrimTypeKind::F64) {
                    emit_byte(OpCode::Sub_r8);
                }
                else {
                    emit_byte(OpCode::Sub_i8);
                }
                break;
            }
            default:
                return error();
        }
        return ok();
    }

    CompileResult visit_impl(VariableExpr& expr) {
        AstVarDecl* var_decl = expr.origin.get();
        u32 offset = m_cur_fn_env->get_local_offset(var_decl->local_index);
        if (offset <= UINT8_MAX) {
            if (offset < 4) {
                if (offset == 0)       emit_byte(OpCode::Ld_Loc_0);
                else if (offset == 1)  emit_byte(OpCode::Ld_Loc_1);
                else if (offset == 2)  emit_byte(OpCode::Ld_Loc_2);
                else if (offset == 3)  emit_byte(OpCode::Ld_Loc_3);
            }
            else {
                emit_bytes(OpCode::Ld_Loc_S, (u8)offset);
            }
        }
        else {
            emit_byte(OpCode::Ld_Loc);
            emit_u16((u16) offset);
        }
        return ok();
    }

    CompileResult visit_impl(CallExpr& expr) {
        auto callee_expr = expr.callee.get();
        COMP_TRY(visit(*callee_expr));
        for (u32 i = 0; i < expr.arguments.size(); i++) {
            auto arg_expr = expr.arguments[i].get();
            COMP_TRY(visit(*arg_expr));
        }
        emit_byte(OpCode::Call);
        return ok();
    }

    CompileResult visit_impl(GetExpr& expr) { return unimplemented(); }

    CompileResult visit_impl(SetExpr& expr) { return unimplemented(); }

    static inline CompileResult ok() { return CompileResult::Ok; }
    static inline CompileResult unimplemented() { return CompileResult::Unimplemented; }
    static inline CompileResult error() { return CompileResult::Error; }

private:
    void emit_byte(OpCode opcode) {

    }
    void emit_byte(u8 value) {
        m_bytecode.push_back(value);
    }

    template <typename ... Args>
    void emit_bytes(u8 value, Args&&... args) {
        m_bytecode.push_back(value);
        m_bytecode.push_back(std::forward<Args>(args)...);
    }

    template <typename ... Args>
    void emit_bytes(OpCode value, Args&&... args) {
        m_bytecode.push_back((u8)value);
        m_bytecode.push_back(std::forward<Args>(args)...);
    }

    void emit_u16(u16 value) {
        m_bytecode.push_back((value >> 8) & 0xff);
        m_bytecode.push_back(value & 0xff);
    }

    void emit_u32(u32 value) {
        m_bytecode.push_back((value >> 24) & 0xff);
        m_bytecode.push_back((value >> 16) & 0xff);
        m_bytecode.push_back((value >> 8) & 0xff);
        m_bytecode.push_back(value & 0xff);
    }

    void emit_u64(u64 value) {
        m_bytecode.push_back((value >> 56) & 0xff);
        m_bytecode.push_back((value >> 48) & 0xff);
        m_bytecode.push_back((value >> 40) & 0xff);
        m_bytecode.push_back((value >> 32) & 0xff);
        m_bytecode.push_back((value >> 24) & 0xff);
        m_bytecode.push_back((value >> 16) & 0xff);
        m_bytecode.push_back((value >> 8) & 0xff);
        m_bytecode.push_back(value & 0xff);
    }
};

}