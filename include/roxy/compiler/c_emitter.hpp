#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/string.hpp"
#include "roxy/core/string_view.hpp"
#include "roxy/core/vector.hpp"
#include "roxy/core/bump_allocator.hpp"
#include "roxy/core/tsl/robin_set.h"
#include "roxy/compiler/ssa_ir.hpp"

namespace rx {

// Forward declaration — full type lives in roxy/vm/binding/registry.hpp.
class NativeRegistry;

struct CEmitterConfig {
    Vector<String> native_include_paths;
    bool emit_main_entry = true;
    // Optional. When set, `emit_native_call` consults the registry for any
    // CallNative name that doesn't match the static built-in mapping. Hits
    // emit a direct call to the user's C++ function (assumed to be declared
    // by one of the `native_include_paths` headers); misses fall through to
    // the warning fallback.
    const NativeRegistry* native_registry = nullptr;
    // Optional source-file path used in `#line N "<source_path>"` directives
    // emitted at the start of each generated function body. Empty disables
    // `#line` emission. The C compiler's debug info will then attribute
    // function-body lines to the original Roxy source.
    String source_path;
};

class CEmitter {
public:
    CEmitter(BumpAllocator& alloc, const CEmitterConfig& config);

    void emit_source(const IRModule* module, String& output);
    void emit_header(const IRModule* module, String& output);

private:
    // Type emission
    void emit_type(Type* type, String& out);

    // Name mangling: $$ -> __, $ -> _
    void emit_mangled_name(StringView name, String& out);

    // Emit the C symbol name for a function. Behaves like `emit_mangled_name`
    // except that the user's `main` is renamed to `main_entry` when
    // `emit_main_entry` is true, so the generator can wrap it with a C `main()`
    // that initializes the runtime context.
    void emit_function_symbol(StringView name, String& out);

    // Type definition emission
    void emit_enum_typedefs(const IRModule* module, String& out);
    void emit_struct_forward_declarations(const IRModule* module, String& out);
    void emit_struct_typedefs(const IRModule* module, String& out);

    // Module-level globals: emit one C global variable per Roxy global (zero-init
    // statics; `__module_init` runs the real initializers at startup). GlobalAddr
    // lowers to `&<global_symbol>`; the symbol is `g_<name>`.
    void emit_global_definitions(const IRModule* module, String& out);
    void emit_global_symbol(StringView name, String& out);
    const IRGlobal* find_global_by_offset(u32 slot_offset);

    // Recursive typed delete (the C analogue of the VM's descriptor-driven
    // delete_value). `ptr_expr` is a C expression for the object/struct pointer;
    // `free_obj` controls the trailing roxy_free (true for heap kinds: uniq /
    // List / Map / Coro; false for an inline value struct). Containers iterate
    // their noncopyable elements/keys/values, recurse, then free their buffers
    // (roxy_list_delete / roxy_map_delete) and finally the header.
    void emit_typed_delete(Type* type, StringView ptr_expr, bool free_obj, String& out);
    // Clean up one container slot: `slot_expr` is a `uint32_t*` to the element /
    // key / value at bucket i. Pointer-shaped elements (uniq/List/Map/Coro) load
    // the pointer and recurse with free_obj=true; inline value structs recurse
    // in place with free_obj=false.
    void emit_delete_slot(Type* elem_type, StringView slot_expr, String& out);
    u32 m_delete_tmp = 0;  // unique-temp counter for emit_typed_delete

    // --- Per-type container drop glue (lifetimes.md "Value lifecycle") ---
    // Container drops (List/Map) are factored into per-type `roxy_drop__<T>(void*)`
    // functions — the AOT analogue of a struct's `$$delete`, so the inline element
    // loop isn't duplicated at every Delete site. request_container_drop_glue()
    // lazily emits a forward decl + definition (recursing for nested containers)
    // the first time a container type is dropped, and returns the function name;
    // emit_typed_delete routes container Deletes through the call. Gated by
    // Type::needs_drop()/is_trivial(). The decls/defs buffers are spliced into the
    // output before the function bodies.
    String request_container_drop_glue(Type* container);
    void emit_container_drop_body(Type* container, StringView self_var, String& out);
    void append_type_mangle(Type* type, String& out);
    String m_drop_glue_decls;             // `static void roxy_drop__T(void*);` lines
    String m_drop_glue_defs;              // the glue function definitions
    tsl::robin_set<Type*> m_drop_glue_seen;  // container types already emitted

    // The C return type for a function. Normally `func->return_type`, but a
    // method returning a closure has its IR return_type left unset (the VM
    // returns by register regardless); fall back to the type of the actual
    // `Return` value so the prototype and call sites agree.
    Type* effective_return_type(const IRFunction* func);

    // Function emission
    void emit_function_prototype(const IRFunction* func, String& out);
    void emit_function(const IRFunction* func, String& out);
    void emit_block(const IRBlock* block, const IRFunction* func, String& out);
    void emit_instruction(const IRInst* inst, String& out);
    void emit_terminator(const IRBlock* block, const IRFunction* func, String& out);

    // Block argument helpers
    void emit_block_arg_declarations(const IRFunction* func, String& out);
    void emit_block_arg_assignments(const IRFunction* func, const JumpTarget& target, String& out);
    // Emit one block-arg RHS value, casting a null to the destination param's
    // pointer type (`block_arg = (T*)nullptr`) since `void*` → `T*` is ill-formed.
    void emit_block_arg_value(const IRFunction* func, const JumpTarget& target, u32 i, String& out);

    // Value helpers
    void emit_value(ValueId id, String& out);

    // Per-function state helpers
    Type* get_value_type(ValueId id);
    bool is_stack_alloc_value(ValueId id);
    bool is_pointer_value(ValueId id);

    // Field access helper: emit v0->field or v0.field depending on whether v0 is a pointer
    void emit_field_access(ValueId object, StringView field_name, String& out);

    // Collect type info for all SSA values in a function
    void collect_value_types(const IRFunction* func);

    // Lookup a function by name in the current module
    const IRFunction* find_function(StringView name);

    // Check if a StackAlloc value is for a non-struct type (scalar out/inout)
    bool is_scalar_stack_alloc(ValueId id);

    // Phase 3: Runtime library support
    void emit_runtime_include(String& out);
    void emit_type_id_defines(const IRModule* module, String& out);
    void emit_escaped_string(StringView str, String& out);
    void emit_native_call(const IRInst* inst, String& out);

    // Phase 4 step 4: pre-scan IR for `CallNative` ops that resolve through
    // `CEmitterConfig::native_registry`. Each unique user native produces an
    // `extern` declaration in the source preamble, taking the C++ symbol name
    // from `NativeFunctionEntry::aot_symbol_name` and the type signature from
    // the IR call site. Built-in natives (those mapped by the static table in
    // `emit_native_call`) are skipped — their C declarations come from
    // `roxy_rt.h`.
    struct ExternNativeDecl {
        StringView aot_symbol_name;
        Type* return_type;
        Vector<Type*> arg_types;
    };
    void collect_extern_native_decls(const IRModule* module);
    void emit_extern_native_decls(String& out);
    // Returns nonzero if `name` is mapped by the static built-in table or a
    // pattern-based match in `emit_native_call`. Pre-scan uses this to skip
    // declaring built-ins.
    bool is_static_mapped_native(StringView name);

    // Header emission helpers
    bool is_pub_struct(Type* struct_type) const;
    bool is_pub_enum(Type* enum_type) const;
    void emit_pub_struct_definitions(const IRModule* module, String& out);
    void emit_inline_method_wrapper(Type* struct_type, const MethodInfo& method,
                                    const IRFunction* func, String& out);
    void emit_pub_make_factories(const IRModule* module, String& out);
    void emit_make_factory(Type* struct_type, u32 type_id,
                           const IRFunction* ctor, const IRFunction* dtor,
                           String& out);
    const IRFunction* find_function_by_mangled(StringView mangled);

    // Configuration
    CEmitterConfig m_config;
    BumpAllocator& m_alloc;

    // Module-level state (set during emit_source)
    const IRModule* m_module = nullptr;

    // Per-function state
    tsl::robin_map<u32, Type*> m_value_types;         // ValueId.id -> Type*
    tsl::robin_set<u32> m_stack_alloc_values;          // Tracks StackAlloc result ValueIds
    tsl::robin_set<u32> m_pointer_values;              // Values that are struct pointers (StackAlloc, struct params, GetFieldAddr)
    tsl::robin_map<u32, i64> m_const_int_values;       // ValueId.id -> ConstInt value (used at call sites that need compile-time-known indices, e.g. roxy_map_alloc's hash_fn_index/eq_fn_index args)

    // User-registered natives referenced by CallNative ops in this module.
    // Populated by `collect_extern_native_decls()` before function bodies are
    // emitted; consumed by `emit_extern_native_decls()` to produce the source
    // preamble's `extern` block.
    tsl::robin_map<StringView, ExternNativeDecl> m_extern_native_decls;

    // Last source line emitted via a `#line` directive inside the current
    // function body. Reset to the function's `source_line` at body start;
    // `emit_instruction` writes a fresh `#line` directive whenever the
    // current instruction's `source_line` differs (and is non-zero).
    u32 m_last_emitted_source_line = 0;

    // --- Exception handling (checked-return model) ---
    //
    // A "try group" bundles all `IRExceptionHandler`s that share a try entry
    // (the typed catches plus the optional `finally.catch` re-throw landing pad).
    // Each group emits one `__dispatch_<id>` label: it runs the try-body cleanup,
    // then a type_id if/else chain routing to a matching catch block, the
    // catch-all/`finally.catch` block, or the next-outer dispatch / `__unwind`.
    struct TryGroup {
        BlockId try_entry;                  // shared try entry block
        tsl::robin_set<u32> body_blocks;    // try_body_blocks as a membership set
        Vector<u32> handler_indices;        // indices into func->exception_handlers, in order
        u32 outer_group = UINT32_MAX;       // innermost enclosing group, or none
    };

    bool m_module_uses_exceptions = false;  // any function throws / has handlers
    Vector<TryGroup> m_try_groups;          // per-function try groups
    tsl::robin_map<u32, u32> m_block_to_group;  // block id -> innermost group index
    tsl::robin_set<u32> m_cleanup_values;   // cleanup_info value ids (zero-init + null-after-delete)
    bool m_func_needs_unwind = false;       // emit a `__unwind` label for this function
    u32 m_cur_block_id = 0;                 // block currently being emitted (for throw/call routing)

    bool module_uses_exceptions(const IRModule* module);
    void compute_exception_routing(const IRFunction* func);
    // Emit the goto that routes a throw / pending-after-call in `block_id` to its
    // innermost enclosing dispatch label, or to `__unwind`.
    void emit_exception_route(u32 block_id, String& out);
    // Emit null-guarded, LIFO cleanup for owned locals / borrows. `body_group >= 0`
    // restricts to records created inside that try group's body (dispatch path);
    // `body_group < 0` fires all records (unwind path).
    void emit_cleanup_records(const IRFunction* func, i32 body_group, String& out);
    // Emit the dispatch labels (one per try group) and the `__unwind` label.
    void emit_exception_labels(const IRFunction* func, String& out);
    // Emit the `return …;` an unwinding function uses (value ignored by callers).
    void emit_unwind_return(const IRFunction* func, String& out);

    // --- Closures ---
    //
    // The VM dispatches `CALL_INDIRECT` through a function-table index stored in
    // the env's first `__call_idx` slot. AOT has no such table, so we build our
    // own: each distinct lifted call function gets an index; `g_closure_fns[idx]`
    // holds its pointer and `Closure` stores `idx` into the env's `__call_idx`.
    tsl::robin_map<StringView, u32> m_closure_fn_index;  // call_function_name -> index
    Vector<StringView> m_closure_fns;                    // index -> call_function_name
    Vector<StringView> m_closure_env_names;              // index -> env_struct_name (parallel)

    void collect_closure_dispatch(const IRModule* module);
    // Emit `g_closure_fns[]` and the type-erased `__closure_delete` dispatcher.
    void emit_closure_dispatch(String& out);
    // Find a struct type (e.g. a closure env) by name in module->struct_types.
    Type* find_struct_type(StringView name);
};

} // namespace rx
