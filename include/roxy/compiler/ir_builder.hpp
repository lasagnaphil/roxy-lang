#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/vector.hpp"
#include "roxy/core/bump_allocator.hpp"
#include "roxy/compiler/ast.hpp"
#include "roxy/compiler/ssa_ir.hpp"
#include "roxy/compiler/types.hpp"
#include "roxy/compiler/symbol_table.hpp"

#include "roxy/core/tsl/robin_map.h"

namespace rx {

// IRBuilder generates SSA IR from a type-checked AST
// Assumes semantic analysis has already been performed and expr->resolved_type is set
class IRBuilder {
public:
    explicit IRBuilder(BumpAllocator& allocator, TypeCache& types);

    // Build IR module from a program
    IRModule* build(Program* program);

    // Build IR for a single function
    IRFunction* build_function(FunDecl* decl);

private:
    // Block management
    IRBlock* create_block(StringView name = {});
    void set_current_block(IRBlock* block);
    void finish_block_goto(BlockId target, Span<BlockArgPair> args = {});
    void finish_block_branch(ValueId cond, BlockId then_block, BlockId else_block,
                             Span<BlockArgPair> then_args = {}, Span<BlockArgPair> else_args = {});
    void finish_block_return(ValueId value);

    // Instruction emission
    IRInst* emit_inst(IROp op, Type* result_type);
    ValueId emit_const_null();
    ValueId emit_const_bool(bool value);
    ValueId emit_const_int(i64 value, Type* type = nullptr);
    ValueId emit_const_float(f64 value, Type* type = nullptr);
    ValueId emit_const_string(StringView value);

    ValueId emit_binary(IROp op, ValueId left, ValueId right, Type* result_type);
    ValueId emit_unary(IROp op, ValueId operand, Type* result_type);
    ValueId emit_copy(ValueId value, Type* type);

    ValueId emit_call(StringView func_name, Span<ValueId> args, Type* result_type);
    ValueId emit_new(StringView type_name, Span<ValueId> args, Type* result_type);
    ValueId emit_get_field(ValueId object, StringView field_name, u32 field_index, Type* result_type);
    ValueId emit_set_field(ValueId object, StringView field_name, u32 field_index, ValueId value, Type* result_type);
    ValueId emit_get_index(ValueId object, ValueId index, Type* result_type);
    ValueId emit_set_index(ValueId object, ValueId index, ValueId value, Type* result_type);

    // Statement generation
    void gen_stmt(Stmt* stmt);
    void gen_expr_stmt(Stmt* stmt);
    void gen_block_stmt(Stmt* stmt);
    void gen_if_stmt(Stmt* stmt);
    void gen_while_stmt(Stmt* stmt);
    void gen_for_stmt(Stmt* stmt);
    void gen_return_stmt(Stmt* stmt);
    void gen_break_stmt(Stmt* stmt);
    void gen_continue_stmt(Stmt* stmt);
    void gen_delete_stmt(Stmt* stmt);

    // Expression generation - returns the value ID of the result
    ValueId gen_expr(Expr* expr);
    ValueId gen_literal_expr(Expr* expr);
    ValueId gen_identifier_expr(Expr* expr);
    ValueId gen_unary_expr(Expr* expr);
    ValueId gen_binary_expr(Expr* expr);
    ValueId gen_ternary_expr(Expr* expr);
    ValueId gen_call_expr(Expr* expr);
    ValueId gen_index_expr(Expr* expr);
    ValueId gen_get_expr(Expr* expr);
    ValueId gen_assign_expr(Expr* expr);
    ValueId gen_grouping_expr(Expr* expr);
    ValueId gen_this_expr(Expr* expr);
    ValueId gen_new_expr(Expr* expr);

    // Declaration generation
    void gen_decl(Decl* decl);
    void gen_var_decl(Decl* decl);

    // Variable management
    void define_local(StringView name, ValueId value, Type* type);
    ValueId lookup_local(StringView name);

    // Get the appropriate binary opcode for a type
    IROp get_binary_op(BinaryOp op, Type* type);
    IROp get_comparison_op(BinaryOp op, Type* type);
    IROp get_unary_op(UnaryOp op, Type* type);

    BumpAllocator& m_allocator;
    TypeCache& m_types;

    // Current function being built
    IRFunction* m_current_func;
    IRBlock* m_current_block;

    // Local variable mapping: name -> value ID
    struct LocalVar {
        ValueId value;
        Type* type;
    };
    Vector<tsl::robin_map<StringView, LocalVar, StringViewHash, StringViewEqual>> m_local_scopes;

    // Loop control flow info (for break/continue)
    struct LoopInfo {
        IRBlock* header_block;    // Loop header (for continue)
        IRBlock* exit_block;      // Exit block (for break)
    };
    Vector<LoopInfo> m_loop_stack;

    // Helper to create a span in the allocator
    template<typename T>
    Span<T> alloc_span(u32 count) {
        if (count == 0) return {};
        T* data = reinterpret_cast<T*>(m_allocator.alloc_bytes(sizeof(T) * count, alignof(T)));
        return Span<T>(data, count);
    }

    // Push/pop scope for local variables
    void push_scope();
    void pop_scope();
};

}
