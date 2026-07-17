#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/vector.hpp"
#include "roxy/core/bump_allocator.hpp"
#include "roxy/core/format.hpp"
#include "roxy/compiler/ast.hpp"
#include "roxy/compiler/ir_fold.hpp"
#include "roxy/compiler/ownership_tracker.hpp"
#include "roxy/compiler/ssa_ir.hpp"
#include "roxy/compiler/type_env.hpp"
#include "roxy/compiler/types.hpp"
#include "roxy/compiler/symbol_table.hpp"

#include "roxy/core/tsl/robin_map.h"

#include <initializer_list>

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

    // Invoke fn(instance) for every generic struct instance that reaches
    // codegen from THIS module: analyzed, with a concrete type, not a Phase-B
    // abstract artifact (those are never codegen'd), and owned by the current
    // module. Ownership mirrors build_generic_fun_instances: an instance's
    // ctors/dtors/methods are emitted only from the module that defined the
    // generic struct template, so they are built once instead of once per
    // module (all instances are visible everywhere because analysis completes
    // for the whole program before any module's IR build). An empty
    // template_module (single-module compilation) or empty m_module_name falls
    // through and emits from the current module. This filter previously
    // appeared verbatim at five build-phase sites.
    template<typename Fn>
    void for_each_concrete_struct_instance(Fn&& fn) {
        for (auto* instance : m_type_env.generics().all_struct_instances()) {
            if (!instance->is_analyzed || !instance->concrete_type) continue;
            if (instance->is_abstract) continue;
            bool owns =
                instance->template_module.empty() ||
                m_module_name.empty() ||
                instance->template_module == m_module_name;
            if (!owns) continue;
            fn(instance);
        }
    }

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
    // The pure constant evaluation lives in ir_fold.{hpp,cpp} (fold_*_const);
    // these wrappers look up the operand instructions and emit the result.
    ValueId emit_folded_const(const FoldedConst& folded, Type* result_type);
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
    // Emit a CallNative to a registry native with a fixed argument list, folding
    // the get_index lookup + arena span allocation every site used to hand-roll.
    // Reports an internal error and returns invalid when `name` is not registered;
    // sites that legitimately skip a missing native check get_index themselves.
    ValueId emit_native(StringView name, std::initializer_list<ValueId> args, Type* result_type);
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

    // One-operand emission helpers for the void-result annotation/lifecycle ops.
    // emit_delete: typed Delete (runtime handles null checks, destructor
    // dispatch, container element iteration, freeing); pass void_type() for a
    // type-erased free. emit_nullify: compile-time cleanup-record narrowing
    // annotation. emit_assert_heap: the promotion gate that traps a
    // stack-allocated receiver (lifetimes.md "Promotion").
    void emit_delete(ValueId value, Type* type);
    void emit_nullify(ValueId value);
    void emit_assert_heap(ValueId value);

    // Reference counting for constraint reference model
    void emit_ref_inc(ValueId ptr);
    void emit_ref_dec(ValueId ptr);
    void emit_str_retain(ValueId ptr);
    void emit_str_release(ValueId ptr);
    // Retain `val` if its type is a reference-counted string (finding 9b). The
    // no-op-for-non-string form lets copy sites call it unconditionally.
    void maybe_str_retain(ValueId val, Type* type);

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
    // Wraps a uniq/ref/fun value into a `weak` when target_type is weak. When
    // `source_expr` is a bare `self` (a promotion of a possibly-stack receiver),
    // a heap gate (AssertHeap) is emitted before the WeakCreate so a stack
    // receiver traps instead of snapshotting a bogus generation (lifetimes.md
    // "Promotion"). Pass source_expr = nullptr at sites that already emit their
    // own gate (call args) or gate upstream (closure captures via needs_heap_check).
    ValueId maybe_wrap_weak(ValueId value, Type* source_type, Type* target_type,
                            Expr* source_expr = nullptr);

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

    // Local variable mapping entry: name -> SSA value + type (the scope maps
    // in m_local_scopes hold these; declared here so ScopeSnapshot can use it).
    struct LocalVar {
        ValueId value;
        Type* type;
        // True for out/inout params and `self` (the pointer-passed params in
        // m_param_is_ptr). Cached here so gen_identifier_expr can test it off the
        // LocalVar it already found instead of a separate m_param_is_ptr probe
        // (§3.7). Fixed at definition — reassignments store through the pointer.
        bool is_ptr = false;
    };

    // ── Branch/merge machinery shared by if / else-if chain / when / try ──

    // Deep snapshot of the local-scope SSA bindings, optionally including the
    // owned locals' is_moved flags. Branch codegen snapshots before the first
    // branch and restores per branch so each sees the pre-branch state, then
    // picks a merge-point state (see each statement's policy comment). These
    // copies were a top profile entry in the 2026-07-05 measurement — if that
    // ever matters, this type is the single place to swap in an undo log.
    struct ScopeSnapshot {
        Vector<tsl::robin_map<StringView, LocalVar>> scopes;
        Vector<bool> is_moved;       // captured only when has_move_state
        bool has_move_state = false;
    };
    ScopeSnapshot snapshot_scopes(bool with_move_state);
    // Copy-restore (snapshot stays reusable). is_moved flags are restored only
    // when the snapshot captured them AND restore_move_state is true.
    void restore_scopes(const ScopeSnapshot& snapshot, bool restore_move_state = true);
    // Move-restore (single use; steals the scope maps instead of deep-copying).
    void restore_scopes_move(ScopeSnapshot&& snapshot);

    // Info about a variable needing a phi (block param) at a merge point.
    struct PhiInfo {
        StringView name;
        Type* type;
        ValueId merge_param;    // Block param ValueId on the merge block
        ValueId original_value; // Value before the branch (fall-through edges)
    };
    // Deduplicate `modified` down to variables that exist (with a valid SSA
    // value) before the branch, create one merge-block param per survivor,
    // and return the PhiInfo list (in first-occurrence order).
    Vector<PhiInfo> make_merge_phis(IRBlock* merge_block, const Vector<StringView>& modified);
    // Block args carrying each phi's pre-branch value (fall-through edge).
    Span<BlockArgPair> phi_original_args(const Vector<PhiInfo>& phi_info);
    // Block args carrying each phi's current value (a branch-exit edge).
    Span<BlockArgPair> phi_current_args(const Vector<PhiInfo>& phi_info);
    // Rebind each phi var to its merge-block param.
    void bind_merge_phis(const Vector<PhiInfo>& phi_info);
    // If the current block is still open, jump to the merge block passing the
    // phis' current values.
    void goto_merge_if_open(IRBlock* merge_block, const Vector<PhiInfo>& phi_info);

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
    // Shared argument lowering for the simple call shapes — constructor, super,
    // and named-destructor calls (no hidden output pointer, no inout reload
    // tracking). out/inout args pass an lvalue address; a noncopyable value arg
    // is consumed (temp Nullify + moved-out-field null), matching
    // lower_call_args. mark_simple_args_moved is the post-call companion:
    // noncopyable identifier args are marked moved (ownership transferred to
    // the callee) so scope-exit cleanup skips them.
    Span<ValueId> lower_simple_args(Span<CallArg> arguments);
    void mark_simple_args_moved(Span<CallArg> arguments);
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
    StringView intern_format(fmt_string<sizeof...(Args)> fmt, const Args&... args) {
        return format_to_arena(m_allocator, runtime_format_string{fmt.str}, args...);
    }

    // Fast path for synthetic "<prefix><id>" names (temp locals, string temps):
    // builds the string directly rather than through format_to_arena, which runs
    // the printf-style formatter twice (a length probe, then the write). One such
    // name is minted per noncopyable/string call temp during IR building, so this
    // path was a visible slice of the compile profile's formatting time.
    StringView intern_synthetic_name(const char* prefix, u32 id) {
        char buf[24];
        u32 n = 0;
        while (*prefix) buf[n++] = *prefix++;            // short fixed prefix ("__tmp"/"__str")
        char rev[10];                                    // u32 is at most 10 decimal digits
        u32 digits = 0;
        do { rev[digits++] = static_cast<char>('0' + id % 10); id /= 10; } while (id);
        while (digits > 0) buf[n++] = rev[--digits];
        char* dst = reinterpret_cast<char*>(m_allocator.alloc_bytes(n + 1, 1));
        for (u32 i = 0; i < n; i++) dst[i] = buf[i];
        dst[n] = '\0';
        return StringView(dst, n);
    }

    // Fast path for "<a><b>" concatenation of two fixed C strings — same output
    // as intern_format("{}...", ...) but without the double formatter pass. Used
    // for synthetic block labels (debug-only names), minted per variant guard.
    StringView intern_concat(const char* a, const char* b) {
        u32 la = 0; while (a[la]) la++;
        u32 lb = 0; while (b[lb]) lb++;
        char* dst = reinterpret_cast<char*>(m_allocator.alloc_bytes(la + lb + 1, 1));
        for (u32 i = 0; i < la; i++) dst[i] = a[i];
        for (u32 i = 0; i < lb; i++) dst[la + i] = b[i];
        dst[la + lb] = '\0';
        return StringView(dst, la + lb);
    }

    // Zero `slot_count` contiguous u32 slots of `self_ptr` starting at
    // `start_slot`. Used by constructors to null-init a struct's own slot
    // range so the destroy-old preamble in `self.field = …` never runs on
    // the caller's stale local_stack bytes. Emits 2-slot SET_FIELDs in bulk
    // with a single reusable null value, one trailing 1-slot write if the
    // count is odd.
    void emit_zero_slots(ValueId self_ptr, u32 start_slot, u32 slot_count);

    // Zero/"empty" constant for a scalar field (bool false, int/enum 0, float
    // 0.0, string ""); null for pointer-shaped types. Struct fields recurse via
    // emit_struct_default_init instead.
    ValueId emit_zero_value(Type* type);

    // Default-initialize `struct_type`'s OWN declared fields in place at
    // `self_ptr`: declared default values, zero/empty scalars, recursive init
    // for nested value-struct fields, zeroed discriminants and union regions.
    // Inherited fields are NOT touched — the synthesized default constructor
    // covers them via the parent-constructor call, and nested fields via the
    // chain walk in emit_struct_default_init.
    void emit_own_field_default_init(ValueId self_ptr, Type* struct_type);

    // Default-initialize EVERY field of `struct_type` in place at `ptr`,
    // walking the inheritance chain — for nested value-struct fields, which
    // have no parent-constructor call to cover inherited fields. Recurses to
    // any depth (the old code unrolled exactly one level, so a struct field
    // inside a nested struct was null-filled instead of taking its declared
    // defaults, and nested discriminants/union regions kept garbage).
    void emit_struct_default_init(ValueId ptr, Type* struct_type);

    // Apply reference wrapper to base type based on RefKind
    Type* apply_ref_kind(Type* base_type, RefKind ref_kind);

    // Function-building scaffolding shared by every build_* entry point.
    // begin_ir_function allocates m_current_func, stamps name / is_pub /
    // source_line (0 = synthesized), and resets the per-instruction source-line
    // tracker. finish_ir_function returns the built function and clears the
    // builder's current-function/block state.
    void begin_ir_function(StringView name, bool is_pub, u32 source_line);
    IRFunction* finish_ir_function();

    // Generate a StmtBlock body's declarations (no-op for null / non-block).
    void gen_body(Stmt* body);

    // Append the hidden output-pointer parameter when the current function
    // returns a large struct (no-op otherwise). Call after return_type is set,
    // before begin_function_body.
    void add_hidden_return_param();

    // Resolve a declared return type (null = void). When `symbol_name` names a
    // function symbol whose type semantic analysis already resolved, prefer
    // that; otherwise resolve by name + ref kind. Methods pass an empty
    // symbol_name — they aren't in the symbol table, and a same-named free
    // function must not hijack the lookup.
    Type* resolve_return_type(TypeExpr* return_type_expr, StringView symbol_name);

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

    // Local variable mapping: name -> LocalVar (struct declared above the
    // branch/merge machinery section)
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
    // (uniq references, value structs with destructors, containers, ref
    // borrows, owned strings). The OwnershipTracker collaborator owns the
    // state and keyed lookups (ownership_tracker.hpp); the IRBuilder keeps
    // all IR emission (destroys, Nullify annotations, SSA rebinds) and
    // transitions the tracker — mirroring the semantic side's LifetimeChecker.
    OwnershipTracker m_ownership;

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

    // String reference-counting (finding 9b). A string PRODUCER (concat, f-string,
    // substr, to_string, a user function return) yields an owned count-1 temp;
    // track it so it's released at scope exit unless consumed. At a copy site
    // (bind / store / push / return), consume_or_retain_string adopts a tracked
    // owned temp (count transfers, no retain) or retains an existing owner (an
    // identifier, a borrowed field/element read). No-op for non-string types.
    void track_string_temp(ValueId val, Type* type);
    void consume_or_retain_string(ValueId val, Type* type, bool adopted_by_variable);

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

    // Resolved field-access info shared by gen_get_expr / gen_assign_field:
    // a regular field, or a variant field with its guard context. slot_offset
    // is absolute (union base already applied for variant fields).
    struct FieldAccess {
        u32 slot_offset = 0;
        u32 slot_count = 1;
        Type* field_type = nullptr;
        bool is_variant_field = false;
        const WhenClauseInfo* when_clause = nullptr;
        const VariantInfo* variant = nullptr;
    };
    FieldAccess resolve_field_access(Type* struct_type, StringView name);

    // Runtime discriminant check for a variant-field access: trap when the
    // union currently holds a different variant. `label` prefixes the
    // pass/fail block names. No-op for non-variant accesses.
    void emit_variant_guard(ValueId obj, const FieldAccess& access, const char* label);

    // Emit cleanup (implicit destruction) for all live owned locals at or above min_scope_depth
    void emit_scope_cleanup(u32 min_scope_depth);

    // The current block's id, or the last created block's when the current
    // block is already closed — the end of a cleanup-record range.
    BlockId current_or_last_block_id() const;

    // Record exception-path cleanup records (Delete / RefDec / StrRelease by
    // owned kind) for every owned local at or above `depth`, ending at the
    // current-or-last block. Uses initial_value (the SSA value at declaration)
    // so lowering maps each record to the right register; the VM null-checks
    // the register so an already-cleaned value is safely skipped. Records are
    // pushed in declaration order — the VM's execute_cleanup iterates in
    // reverse for LIFO cleanup. Shared by pop_scope and end_function_body.
    void record_scope_cleanup_records(u32 depth);

    // Variable management (declared after LocalVar struct)
    void define_local(StringView name, ValueId value, Type* type, bool is_ptr = false);
    ValueId lookup_local(StringView name);
    LocalVar* find_local(StringView name);  // Returns pointer to LocalVar or nullptr

    // Loop variable analysis (declared after LoopVarInfo struct). The public
    // entry points seed a membership set from `out`'s current contents (call
    // sites accumulate across several collect calls — if/else branches, for
    // body + increment — and loop-header param creation relies on `out` being
    // duplicate-free), then delegate to the _impl recursion, which dedupes
    // through the set instead of rescanning `out` per assignment (O(n²)).
    void collect_assigned_vars(Stmt* stmt, Vector<StringView>& out);
    void collect_assigned_vars_expr(Expr* expr, Vector<StringView>& out);
    void collect_assigned_vars_impl(Stmt* stmt, Vector<StringView>& out,
                                    tsl::robin_map<StringView, bool>& seen);
    void collect_assigned_vars_expr_impl(Expr* expr, Vector<StringView>& out,
                                         tsl::robin_map<StringView, bool>& seen);
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

    // Helper to allocate a span from a braced list: alloc_span({a, b, c})
    template<typename T>
    Span<T> alloc_span(std::initializer_list<T> items) {
        if (items.size() == 0) return {};
        Span<T> span = alloc_span<T>(static_cast<u32>(items.size()));
        u32 i = 0;
        for (const T& item : items) {
            span[i++] = item;
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
