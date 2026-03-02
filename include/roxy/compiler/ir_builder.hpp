#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/vector.hpp"
#include "roxy/core/bump_allocator.hpp"
#include "roxy/compiler/ast.hpp"
#include "roxy/compiler/ssa_ir.hpp"
#include "roxy/compiler/type_env.hpp"
#include "roxy/compiler/types.hpp"
#include "roxy/compiler/symbol_table.hpp"

#include "roxy/core/tsl/robin_map.h"

namespace rx {

// Forward declarations
class NativeRegistry;
class ModuleRegistry;

// IRBuilder generates SSA IR from a type-checked AST
// Assumes semantic analysis has already been performed and expr->resolved_type is set
class IRBuilder {
public:
    IRBuilder(BumpAllocator& allocator, TypeEnv& type_env, NativeRegistry& registry,
              SymbolTable& symbols, ModuleRegistry& module_registry);

    // Build IR module from a program
    IRModule* build(Program* program, Span<Decl*> synthetic_decls = {});

    // Build IR for a single function
    IRFunction* build_function(FunDecl* decl);

    // Build IR for a constructor
    IRFunction* build_constructor(ConstructorDecl* decl, Type* struct_type);

    // Build IR for a destructor
    IRFunction* build_destructor(DestructorDecl* decl, Type* struct_type);

    // Build IR for a method
    IRFunction* build_method(MethodDecl* decl, Type* struct_type);

    // Build synthesized default constructor for a struct type
    IRFunction* build_synthesized_default_constructor(Type* struct_type);

    // Build synthesized default destructor for a struct type (cleans up uniq fields)
    IRFunction* build_synthesized_default_destructor(Type* struct_type);

    // Error reporting
    bool has_error() const { return m_has_error; }
    const char* error() const { return m_error; }

private:
    // Report an internal compiler error (stores the first error only)
    void report_error(const char* message);
    // Block management
    IRBlock* create_block(StringView name = {});
    void set_current_block(IRBlock* block);
    void finish_block_goto(BlockId target, Span<BlockArgPair> args = {});
    void finish_block_branch(ValueId cond, BlockId then_block, BlockId else_block,
                             Span<BlockArgPair> then_args = {}, Span<BlockArgPair> else_args = {});
    void finish_block_return(ValueId value);
    void finish_block_unreachable();

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
    ValueId emit_call_native(StringView func_name, Span<ValueId> args, Type* result_type, u8 native_index);
    ValueId emit_call_external(StringView module_name, StringView func_name, Span<ValueId> args, Type* result_type);
    ValueId emit_new(StringView type_name, Span<ValueId> args, Type* result_type);
    ValueId emit_stack_alloc(u32 slot_count, Type* result_type);
    ValueId emit_get_field(ValueId object, StringView field_name, u32 slot_offset, u32 slot_count, Type* result_type);
    ValueId emit_get_field_addr(ValueId object, StringView field_name, u32 slot_offset, Type* result_type);
    ValueId emit_set_field(ValueId object, StringView field_name, u32 slot_offset, u32 slot_count, ValueId value, Type* result_type);
    ValueId emit_load_ptr(ValueId ptr, u32 slot_count, Type* result_type);
    ValueId emit_store_ptr(ValueId ptr, ValueId value, u32 slot_count, Type* result_type);
    void emit_struct_copy(ValueId dest_ptr, ValueId source_ptr, u32 slot_count);
    ValueId emit_var_addr(StringView name, Type* result_type);

    // Reference counting for constraint reference model
    void emit_ref_inc(ValueId ptr);
    void emit_ref_dec(ValueId ptr);
    void emit_ref_param_decrements();  // Emit RefDec for all ref-typed parameters

    // Weak reference creation
    ValueId emit_weak_create(ValueId ptr, Type* weak_type);
    ValueId maybe_wrap_weak(ValueId value, Type* source_type, Type* target_type);

    // Generate address of an lvalue expression (for out/inout arguments)
    ValueId gen_lvalue_addr(Expr* expr);

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
    void gen_when_stmt(Stmt* stmt);
    void gen_throw_stmt(Stmt* stmt);
    void gen_try_stmt(Stmt* stmt);
    void gen_yield_stmt(Stmt* stmt);

    // Expression generation - returns the value ID of the result
    ValueId gen_expr(Expr* expr);
    ValueId gen_literal_expr(Expr* expr);
    ValueId gen_identifier_expr(Expr* expr);
    ValueId gen_unary_expr(Expr* expr);
    ValueId gen_binary_expr(Expr* expr);
    ValueId gen_ternary_expr(Expr* expr);
    ValueId gen_call_expr(Expr* expr);
    ValueId gen_primitive_cast(Expr* expr);
    ValueId gen_constructor_call(Expr* expr);
    ValueId gen_super_call(Expr* expr);  // Handle super() and super.method() calls
    ValueId gen_index_expr(Expr* expr);
    ValueId gen_get_expr(Expr* expr);
    ValueId gen_assign_expr(Expr* expr);
    ValueId gen_grouping_expr(Expr* expr);
    ValueId gen_this_expr(Expr* expr);
    ValueId gen_struct_literal_expr(Expr* expr);
    ValueId gen_static_get_expr(Expr* expr);
    ValueId gen_string_interp_expr(Expr* expr);

    // Declaration generation
    void gen_decl(Decl* decl);
    void gen_var_decl(Decl* decl);

    // Get the appropriate binary opcode for a type
    IROp get_binary_op(BinaryOp op, Type* type);
    IROp get_comparison_op(BinaryOp op, Type* type);
    IROp get_unary_op(UnaryOp op, Type* type);

    // Name mangling helpers - allocate mangled names in the allocator
    StringView mangle_method(StringView struct_name, StringView method_name);
    StringView mangle_constructor(StringView struct_name, StringView ctor_name = {});
    StringView mangle_destructor(StringView struct_name, StringView dtor_name = {});

    // Apply reference wrapper to base type based on RefKind
    Type* apply_ref_kind(Type* base_type, RefKind ref_kind);

    // Set up function parameters from a list of Params
    // If self_type is non-null, adds 'self' as the first parameter with ref<self_type>
    void setup_parameters(Span<Param> params, Type* self_type = nullptr);

    // Initialize function body: create entry block, push scope, add params to scope
    // skip_hidden_return: if true, don't add the last parameter (hidden return ptr) to scope
    void begin_function_body(bool skip_hidden_return = false);

    // Finalize function body: add implicit return if needed, pop scope, cleanup
    void end_function_body();

    BumpAllocator& m_allocator;
    TypeEnv& m_type_env;
    TypeCache& m_types;  // Cached ref to m_type_env.types()
    NativeRegistry& m_registry;
    SymbolTable& m_symbols;
    ModuleRegistry& m_module_registry;

    // Current function being built
    IRFunction* m_current_func;
    IRBlock* m_current_block;

    // Local variable mapping: name -> value ID
    struct LocalVar {
        ValueId value;
        Type* type;
    };
    Vector<tsl::robin_map<StringView, LocalVar>> m_local_scopes;

    // Track which parameters are pointers (for out/inout semantics)
    tsl::robin_map<StringView, bool> m_param_is_ptr;

    // Track ref-typed parameters for RefInc/RefDec at function boundaries
    Vector<ValueId> m_ref_params;

    // Error state
    bool m_has_error;
    const char* m_error;

    // Info about a variable that needs a phi at loop header
    struct LoopVarInfo {
        StringView name;
        Type* type;
        ValueId header_param;   // Block param ValueId in header
        ValueId initial_value;  // Value before loop
    };

    // Loop control flow info (for break/continue)
    struct LoopInfo {
        IRBlock* header_block;       // Loop header (for continue in while, or header in for)
        IRBlock* exit_block;         // Exit block (for break)
        IRBlock* continue_block;     // Continue target (same as header for while, increment for for)
        Vector<LoopVarInfo> loop_vars;  // Variables modified in loop
        u32 scope_depth;             // Scope depth when loop was entered (for RAII cleanup)
    };
    Vector<LoopInfo> m_loop_stack;

    // Ownership tracking for locals that need RAII / implicit destruction
    // Covers both uniq references (heap-allocated) and value structs with destructors
    struct OwnedLocalInfo {
        StringView name;
        Type* type;            // Full variable type (uniq T or struct T)
        u32 scope_depth;       // Scope level where declared
        bool is_moved;         // Ownership transferred (pass, return, explicit delete)
    };
    Vector<OwnedLocalInfo> m_owned_locals;

    // RAII helpers: emit destructor + Delete (for uniq) or destructor only (for value struct)
    void emit_implicit_destroy(OwnedLocalInfo& info);

    // Emit cleanup code for uniq fields of a struct (called from destructors)
    void emit_field_cleanup(ValueId self_ptr, Type* struct_type);

    // Emit cleanup for a single field (uniq or value-struct with destructor)
    void emit_single_field_destroy(ValueId obj_ptr, StringView field_name,
                                   u32 slot_offset, u32 slot_count, Type* field_type);

    // Emit destruction of a single element value based on its type
    void emit_element_destroy(ValueId elem_value, Type* elem_type);

    // Emit a cleanup loop that destroys all elements in a list, then frees the list
    void emit_list_cleanup(ValueId list_ptr, Type* list_type);

    // Emit cleanup that destroys all entries in a map, then frees the map
    void emit_map_cleanup(ValueId map_ptr, Type* map_type);

    // Emit cleanup (implicit destruction) for all live owned locals at or above min_scope_depth
    void emit_scope_cleanup(u32 min_scope_depth);

    // Find an owned local by name
    OwnedLocalInfo* find_owned_local(StringView name);

    // Variable management (declared after LocalVar struct)
    void define_local(StringView name, ValueId value, Type* type);
    ValueId lookup_local(StringView name);
    LocalVar* find_local(StringView name);  // Returns pointer to LocalVar or nullptr

    // Loop variable analysis (declared after LoopVarInfo struct)
    void collect_assigned_vars(Stmt* stmt, Vector<StringView>& out);
    void collect_assigned_vars_expr(Expr* expr, Vector<StringView>& out);
    Span<BlockArgPair> make_loop_args(const Vector<LoopVarInfo>& loop_vars);

    // Helper to create a span in the allocator
    template<typename T>
    Span<T> alloc_span(u32 count) {
        if (count == 0) return {};
        T* data = reinterpret_cast<T*>(m_allocator.alloc_bytes(sizeof(T) * count, alignof(T)));
        return Span<T>(data, count);
    }

    // Helper to allocate a span from a vector
    template<typename T>
    Span<T> alloc_span(const Vector<T>& vec) {
        if (vec.empty()) return {};
        Span<T> span = alloc_span<T>(static_cast<u32>(vec.size()));
        for (u32 i = 0; i < vec.size(); i++) {
            span[i] = vec[i];
        }
        return span;
    }

    // Push/pop scope for local variables
    void push_scope();
    void pop_scope();
};

}
