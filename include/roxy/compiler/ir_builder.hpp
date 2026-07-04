#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/vector.hpp"
#include "roxy/core/bump_allocator.hpp"
#include "roxy/core/format.hpp"
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

    // build() phases. Each appends to m_module and records failures via m_has_error;
    // build() bails between phases so a later phase never runs on a half-built module.
    // has_default_ctor tracks which struct / generic-instance names already have a
    // user-defined default constructor (written by the decl phases, read by the
    // synthesized-default-ctor phase).
    void build_user_decls(Program* program, tsl::robin_map<StringView, bool>& has_default_ctor);
    void build_synthetic_decls(Span<Decl*> synthetic_decls);
    void build_generic_fun_instances();
    void build_generic_struct_ctors_dtors(tsl::robin_map<StringView, bool>& has_default_ctor);
    void build_synthesized_default_ctors(Program* program,
                                         const tsl::robin_map<StringView, bool>& has_default_ctor);
    void build_generic_struct_methods();
    void build_synthesized_default_dtors(Program* program);
    void build_coroutine_cleanup_wrappers();
    void collect_backend_types(Program* program);

    // Module-level globals (lifetimes/globals support). collect_globals assigns
    // each top-level `var` a slot offset in the module's global array (run first
    // so function bodies can reference globals); build_module_init synthesizes
    // `__module_init` (runs initializers / constructors before main);
    // build_module_shutdown synthesizes `__module_shutdown` (RAII teardown of
    // noncopyable globals at VM destroy). emit_global_addr yields a global's
    // address (GlobalAddr op); gen_global_read loads/derefs a global by name.
    void collect_globals(Program* program);
    IRFunction* build_module_init(Program* program);
    IRFunction* build_module_shutdown();
    ValueId emit_global_addr(u32 slot_offset, Type* type);
    ValueId gen_global_read(u32 global_index, Type* result_type);

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

    // Phase 1 IR optimizations: applied during emission. Return invalid if no
    // fold/simplification applies; the caller falls through to normal emission.
    ValueId try_fold_binary(IROp op, ValueId left, ValueId right, Type* result_type);
    ValueId try_fold_unary(IROp op, ValueId operand, Type* result_type);
    ValueId try_fold_cast(ValueId source, Type* source_type, Type* target_type);
    ValueId try_simplify_binary(IROp op, ValueId left, ValueId right, Type* result_type);
    ValueId try_simplify_unary(IROp op, ValueId operand, Type* result_type);

    ValueId emit_call(StringView func_name, Span<ValueId> args, Type* result_type);
    ValueId emit_call_native(StringView func_name, Span<ValueId> args, Type* result_type, u32 native_index);
    ValueId emit_call_external(StringView module_name, StringView func_name, Span<ValueId> args, Type* result_type);

    // Native-vs-script dispatch: emit CallNative when `name` is a registered native,
    // otherwise a plain Call. Folds the get_index(name) >= 0 ? ... : ... idiom.
    ValueId emit_call_resolved(StringView name, Span<ValueId> args, Type* result_type);
    // Emit a CALL_INDIRECT through an already-evaluated closure value. Returns
    // invalid when the current block is terminated (emit_inst guard).
    ValueId emit_call_indirect(ValueId callee_val, Span<ValueId> args, Type* result_type);
    // Build a method-argument span [self] + args, with a trailing output pointer
    // appended when output_ptr is valid (large struct return convention).
    Span<ValueId> prepend_self(ValueId self, Span<ValueId> args, ValueId output_ptr = ValueId::invalid());
    ValueId emit_index_get(ValueId container, ValueId index, ContainerKind kind, Type* result_type);
    void emit_index_set(ValueId container, ValueId index, ValueId value, ContainerKind kind);
    // Address of a container element (out/inout argument lvalue). result_type is
    // the element/value (pointee) type, mirroring emit_get_field_addr. The pointer
    // is valid only while the container is pinned (lifetimes.md "Container element lvalues").
    ValueId emit_index_addr(ValueId container, ValueId index, ContainerKind kind, Type* result_type);
    ValueId emit_new(StringView type_name, Span<ValueId> args, Type* result_type);
    ValueId emit_stack_alloc(u32 slot_count, Type* result_type);
    ValueId emit_get_field(ValueId object, StringView field_name, u32 slot_offset, u32 slot_count, Type* result_type);
    ValueId emit_get_field_addr(ValueId object, StringView field_name, u32 slot_offset, Type* result_type);
    ValueId emit_set_field(ValueId object, StringView field_name, u32 slot_offset, u32 slot_count, ValueId value, Type* result_type);
    ValueId emit_load_ptr(ValueId ptr, u32 slot_count, Type* result_type);
    ValueId emit_store_ptr(ValueId ptr, ValueId value, u32 slot_count, Type* result_type);
    void emit_struct_copy(ValueId dest_ptr, ValueId source_ptr, u32 slot_count);

    // Reference counting for constraint reference model
    void emit_ref_inc(ValueId ptr);
    void emit_ref_dec(ValueId ptr);

    // Emit `if (map.contains(key)) delete map[key];` — the contains-guarded destroy
    // of a noncopyable map value, so an overwritten (`m[k]=v`, `m.insert`) or
    // removed (`m.remove`) value isn't leaked (lifetimes.md "Value lifecycle"). No-op
    // unless the map's value type is noncopyable (a `ref` value is released by the
    // runtime value_is_ref path; a trivial value needs nothing).
    void emit_map_value_delete_if_present(ValueId map_obj, Type* map_type, ValueId key_val);
    // Emit a bucket-iteration loop that deletes every live noncopyable value of a
    // map (uses the __map_iter_* natives), for `m.clear()` cleanup. No-op unless
    // the value type is noncopyable.
    void emit_map_clear_value_cleanup(ValueId map_obj, Type* map_type);
    // True for `<map>.insert(k, v)` whose value type is noncopyable — the
    // replace-cleanup case. Its value-arg consume is deferred past the insert (the
    // contains-guard branch would otherwise strand the value-Nullify before the
    // insert; lifetimes.md "Value lifecycle"). lower_call_args skips the consume; the
    // map-method dispatch in gen_call_member emits contains-guard → insert →
    // consume so they stay in one block.
    bool is_map_insert_noncopyable_value(CallExpr& call_expr) const;
    // Increment a `ref` borrow whose source value is `source`, inserting the
    // self-promotion heap gate when `source` is a bare `self`: promoting the
    // second-class receiver borrow to a counted `ref` (bind / return / store) is
    // sound only on a heap receiver, so an `AssertHeap` traps a stack receiver
    // before the inc (lifetimes.md "Promotion"). Non-`self` sources are statically heap.
    void emit_ref_borrow_inc(ValueId val, Expr* source);
    // Pin/unpin a container around a call so a borrowed element can't be dangled
    // by a mid-call realloc/free (lifetimes.md "Container element lvalues"). `container` is a List/Map
    // value (or a pinned copy of one).
    void emit_container_pin(ValueId container);
    void emit_container_unpin(ValueId container);
    void emit_ref_param_decrements();  // Emit RefDec for all ref-typed parameters
    // A `Copy` that copy propagation will not collapse, minting a distinct SSA
    // value/register that aliases `src` at runtime. Used for call-site receiver
    // borrows so their cleanup identity is independent of the receiver's
    // owned-local. See IRInst::no_copy_prop and gen_call_member.
    ValueId emit_pinned_copy(ValueId src, Type* type);

    // Weak reference creation
    ValueId emit_weak_create(ValueId ptr, Type* weak_type);
    ValueId maybe_wrap_weak(ValueId value, Type* source_type, Type* target_type);

    // Generate address of an lvalue expression (for out/inout arguments)
    ValueId gen_lvalue_addr(Expr* expr);

    // For an out/inout argument lvalue, return a heap data pointer to the heap
    // object whose storage the argument points into (its "heap root"), or invalid
    // if the lvalue is stack-rooted or not a pure identifier/field chain. The
    // call site counts this root for the call's duration so a mid-call free of it
    // traps rather than dangling the out/inout pointer (lifetimes.md "Counting mechanics"). When
    // a root is returned, *out_type receives its (uniq) type. Only re-evaluates
    // pure field paths, so the extra load it emits is side-effect-free.
    ValueId heap_root_of_lvalue(Expr* lvalue, Type** out_type);

    // Statement generation
    void gen_stmt(Stmt* stmt);
    void gen_expr_stmt(Stmt* stmt);
    void gen_block_stmt(Stmt* stmt);
    void gen_if_stmt(Stmt* stmt);
    void gen_if_else_chain(Stmt* stmt);  // Flattened codegen for else-if chains
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

    // gen_call_expr decomposition. gen_call_expr handles the early type-driven
    // delegations (cast / constructor / list / map / super), lowers arguments once
    // via lower_call_args, dispatches on callee shape, then reloads inout args and
    // marks moved arguments. The dispatch helpers consume the shared CallLowering.
    struct InoutArg {
        StringView name;     // identifier to reload after the call
        ValueId addr;        // stack address passed as the out/inout argument
        Type* type;
        u32 slot_count;
    };
    struct CallLowering {
        Span<ValueId> args;           // user args (addresses for out/inout)
        Span<ValueId> final_args;     // args, plus the hidden output ptr for large struct returns
        Vector<InoutArg> inout_args;  // primitive out/inout identifiers needing a post-call reload
        ValueId output_ptr = ValueId::invalid();  // stack slot for large struct returns
        bool returns_large_struct = false;
    };
    ValueId gen_list_constructor(Expr* expr);  // List<T>() / List<T>(cap)
    ValueId gen_map_constructor(Expr* expr);   // Map<K,V>() / Map<K,V>(cap)
    CallLowering lower_call_args(Expr* expr);
    ValueId gen_call_direct(Expr* expr, const CallLowering& lowered);  // callee is an identifier
    ValueId gen_call_member(Expr* expr, const CallLowering& lowered);  // callee is obj.method / module.fn
    void reload_inout_args(const CallLowering& lowered);
    void mark_call_args_moved(Expr* expr);
    // Bytecode function index of struct_type's `method_name` (mangled at its
    // defining type) within the current module, or -1 if absent.
    i32 find_method_fn_index(Type* struct_type, StringView method_name);

    ValueId gen_primitive_cast(Expr* expr);
    ValueId gen_constructor_call(Expr* expr);
    ValueId gen_super_call(Expr* expr);  // Handle super() and super.method() calls
    ValueId gen_index_expr(Expr* expr);
    ValueId gen_get_expr(Expr* expr);
    ValueId gen_assign_expr(Expr* expr);
    // gen_assign_expr decomposition. gen_assign_expr evaluates the RHS, applies any
    // compound operator, then dispatches on the target's kind. Each helper takes the
    // already-evaluated (and compound-folded) RHS value.
    //
    // gen_compound_assign handles `+=`/`-=`/... : for struct targets with a
    // compound-assign trait method it emits the in-place call and sets handled=true
    // (the whole assignment is done); otherwise it folds the RHS against the target's
    // current value and returns the combined value to store, with handled=false.
    ValueId gen_compound_assign(Expr* expr, ValueId rhs, bool& handled);
    ValueId gen_assign_local(Expr* expr, ValueId value);  // target is an identifier
    ValueId gen_assign_field(Expr* expr, ValueId value);  // target is obj.field
    ValueId gen_assign_index(Expr* expr, ValueId value);  // target is container[index]
    ValueId gen_grouping_expr(Expr* expr);
    ValueId gen_this_expr(Expr* expr);
    ValueId gen_struct_literal_expr(Expr* expr);
    ValueId gen_static_get_expr(Expr* expr);
    ValueId gen_string_interp_expr(Expr* expr);
    ValueId gen_lambda_expr(Expr* expr);

    // Bare named-function-as-value (`var f = double`). Synthesizes (or reuses
    // a cached) trampoline IRFunction + empty env struct, then emits
    // IROp::Closure pointing at them. The target descriptor selects the body
    // call op (Call / CallNative / CallExternal) and the cache key.
    struct FunctionRefTarget {
        enum class Kind {
            Script,           // user-defined script function (incl. generic instances)
            Native,           // top-level native function (declared with `native`)
            ImportedScript,   // cross-module script function
            ImportedNative,   // cross-module native function
        };
        Kind kind = Kind::Script;
        StringView name;            // body call target + cache key (mangled where applicable)
        StringView module_name;     // ImportedScript only
        u32 native_index = 0;       // Native / ImportedNative
        Type* function_type = nullptr;
    };
    ValueId gen_function_ref(Expr* expr, const FunctionRefTarget& target);

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

    // Mangle a non-pub function with the current module prefix to keep names
    // module-private after IR modules are merged into one global table.
    // Returns name unchanged if module is empty (single-file mode).
    StringView mangle_module_local(StringView name);

    // Mangle a name into an arena-allocated StringView without truncation.
    // Thin wrapper over rx::format_to_arena bound to this builder's allocator.
    template<typename... Args>
    StringView intern_format(const char* fmt, const Args&... args) {
        return format_to_arena(m_allocator, fmt, args...);
    }

    // Zero `slot_count` contiguous u32 slots of `self_ptr` starting at
    // `start_slot`. Used by constructors to null-init a struct's own slot
    // range so the destroy-old preamble in `self.field = …` never runs on
    // the caller's stale local_stack bytes. Emits 2-slot SET_FIELDs in bulk
    // with a single reusable null value, one trailing 1-slot write if the
    // count is odd.
    void emit_zero_slots(ValueId self_ptr, u32 start_slot, u32 slot_count);

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

    // Name of the module currently being built. Set in build() from
    // program->module_name. Used by mangle_module_local to scope non-pub
    // function names so they don't collide across modules at link time.
    StringView m_module_name;

    // The IR module currently being built. Set at the top of build() and
    // accessed by helpers that need to look up function indices (e.g. when
    // emitting a call to native_map_alloc with a struct key, the IR builder
    // looks up the user's hash/eq method indices from this module). Lowering
    // preserves IR-module function order in the BCModule, so the index
    // emitted here matches the bytecode function index at runtime.
    IRModule* m_module = nullptr;

    // Current function being built
    IRFunction* m_current_func;
    IRBlock* m_current_block;

    // 1-indexed source line of the AST node currently being lowered. Stamped
    // onto every `IRInst` by `emit_inst`; consumed by the C backend's `#line`
    // directive emission. Reset at each statement / declaration boundary in
    // `gen_stmt` / `gen_decl`. 0 = unknown (synthesized stubs, builtins).
    u32 m_current_source_line = 0;

    // Local variable mapping: name -> value ID
    struct LocalVar {
        ValueId value;
        Type* type;
    };
    Vector<tsl::robin_map<StringView, LocalVar>> m_local_scopes;

    // Track which parameters are pointers (for out/inout semantics)
    tsl::robin_map<StringView, bool> m_param_is_ptr;

    // Track ref-typed parameters for RefInc/RefDec at function boundaries.
    // The type is retained so the exception-path RefDec cleanup record can be
    // emitted in end_function_body (see m_ref_param_entry_block).
    struct RefParamInfo {
        ValueId value;
        Type* type;
    };
    Vector<RefParamInfo> m_ref_params;

    // Entry block of the current function (captured once params are inc'd), used
    // as the start of each ref param's whole-function RefDec cleanup record so an
    // exception unwinding out of the frame decrements the borrow.
    BlockId m_ref_param_entry_block;

    // Module-level globals: name -> index into m_module->globals. Populated by
    // collect_globals before any function is built, so global reads/writes in
    // function bodies resolve to the right slot offset.
    tsl::robin_map<StringView, u32> m_global_indices;

    // Deferred call-site receiver-borrow cleanup records (lifetimes.md "Counting mechanics" / "Promotion").
    // A borrow's exception-path RefDec must run BEFORE the receiver-owner's
    // Delete on unwind (else Delete sees ref_count != 0 and spuriously traps).
    // execute_cleanup runs records in reverse of cleanup_info order, and the
    // owner's Delete record is pushed late (at scope close), so these are
    // collected during the body and appended at end_function_body — after every
    // owned-local record — so reverse iteration releases the borrow first.
    Vector<IRCleanupInfo> m_call_borrow_cleanups;

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

    // Synthesized closure-env struct types encountered while building lambdas.
    // After all bodies are built, each gets a synthesized destructor so a
    // closure cleans its captures on delete (dispatched at runtime by type_id).
    Vector<Type*> m_env_struct_types;

    // Ownership tracking for locals that need RAII / implicit destruction
    // Covers both uniq references (heap-allocated) and value structs with destructors
    // Whether a tracked local owns its value (destroy on cleanup) or is a `ref`
    // borrow (decrement its count on cleanup). RefBorrow locals reuse the owned-
    // local machinery (LIFO scope cleanup, exception records, liveness) but emit
    // RefDec instead of Delete.
    enum class OwnedKind : u8 { Owned, RefBorrow };

    struct OwnedLocalInfo {
        StringView name;
        Type* type;            // Full variable type (uniq T or struct T)
        u32 scope_depth;       // Scope level where declared
        bool is_moved;         // Ownership transferred (pass, return, explicit delete)
        bool is_temporary;     // True for compiler-generated temporaries (__tmp*)
        BlockId start_block;   // Block where variable becomes live (for cleanup records)
        ValueId initial_value; // SSA value at declaration (for cleanup record register mapping)
        OwnedKind kind = OwnedKind::Owned;  // Owned value vs ref borrow
    };
    Vector<OwnedLocalInfo> m_owned_locals;

    u32 m_next_temp_id = 0;  // Counter for generating unique temporary names (__tmp0, __tmp1, ...)

    // Function-reference cache. Keyed by the unmangled function name; one
    // trampoline + empty env struct per referenced function, deduped across
    // multiple `var f = name` references in the same module.
    struct FunctionRefInfo {
        StringView env_struct_name;     // synthesized env struct's name
        StringView trampoline_name;     // trampoline IRFunction's name (already mangled)
    };
    tsl::robin_map<StringView, FunctionRefInfo> m_function_refs;
    u32 m_funref_id_counter = 0;

    // Consume a temporary noncopyable value (ownership transferred to callee/variable).
    // Finds the temporary OwnedLocalInfo entry by ValueId and marks it moved.
    // When adopted_by_variable=true, the caller takes over the same register
    // (e.g., var decl adopting a temp), so no Nullify annotation is needed —
    // the variable's own cleanup record handles it.
    void consume_temp_noncopyable(ValueId val, bool adopted_by_variable = false);

    // Track a noncopyable call result as an owned temporary (so it's cleaned at
    // scope exit unless bound/consumed). No-op for copyable types or values
    // already tracked as a temp (constructor paths self-track).
    void track_noncopyable_call_temp(ValueId val, Type* type);

    // Mark the owned local `name` as moved so scope-exit / exception cleanup skip
    // it. No-op if `name` is not a live (un-moved) owned local. For `uniq` locals
    // (when null_ssa) it re-points the SSA name at null so a later scope-exit
    // Delete is a safe no-op; unless nullify_record is false it also emits a
    // Nullify annotation zeroing the cleanup record's register. Callers that
    // share the moved-from register with the destination/return value pass
    // null_ssa=false; callers that already freed the storage (explicit delete)
    // pass null_ssa=false too. This consolidates the move-marking that was
    // previously copy-pasted across every move site.
    void mark_moved_from(StringView name, bool null_ssa = true, bool nullify_record = true);

    // When a noncopyable pointer field is moved out (`var x = o.field`,
    // `f(o.field)`, `return o.field`, `Foo { x = o.field }`, `y = o.field`), null
    // the source field in the root struct so the root's destructor sees null
    // there and no-ops it, instead of re-freeing the value the move transferred
    // out. No-op unless `consumed` is a field access of a noncopyable field.
    // (Semantic analysis rejects moving a *value-struct* field out, so only
    // pointer-valued fields — uniq/List/Map/Coro/fun — reach here.)
    void nullify_moved_field_source(Expr* consumed);

    // RAII helpers: emit destructor + Delete (for uniq) or destructor only (for value struct)
    void emit_implicit_destroy(OwnedLocalInfo& info);

    // Emit cleanup code for uniq fields of a struct (called from destructors)
    void emit_field_cleanup(ValueId self_ptr, Type* struct_type);

    // Emit cleanup for a single field (uniq or value-struct with destructor)
    void emit_single_field_destroy(ValueId obj_ptr, StringView field_name,
                                   u32 slot_offset, u32 slot_count, Type* field_type);

    // On a tagged-union discriminant reassignment (`s.kind = NEW`), drop the
    // outgoing variant's owned fields and clear the union storage. See the
    // definition for the full rationale (leak + stale-pointer-free hazards).
    void emit_discriminant_reassign_cleanup(ValueId obj,
                                            const WhenClauseInfo& clause,
                                            ValueId new_disc);

    // Before dereferencing a `weak T` value (field read/write, method call),
    // validate the referent is still alive; trap on a dangling/recycled weak.
    void emit_weak_deref_check(ValueId weak_val);

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

    // Build a standalone cleanup wrapper function for a noncopyable List or Map type.
    // Used by coroutine destructors to clean up promoted fields.
    IRFunction* build_cleanup_wrapper(Type* noncopyable_type, u32 wrapper_index);

    // Push/pop scope for local variables
    void push_scope();
    void pop_scope();
};

}
