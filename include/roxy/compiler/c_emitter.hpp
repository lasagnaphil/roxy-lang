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

    // Function emission
    void emit_function_prototype(const IRFunction* func, String& out);
    void emit_function(const IRFunction* func, String& out);
    void emit_block(const IRBlock* block, const IRFunction* func, String& out);
    void emit_instruction(const IRInst* inst, String& out);
    void emit_terminator(const IRBlock* block, const IRFunction* func, String& out);

    // Block argument helpers
    void emit_block_arg_declarations(const IRFunction* func, String& out);
    void emit_block_arg_assignments(const JumpTarget& target, String& out);

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
    tsl::robin_map<u32, ValueId> m_var_name_to_value;  // BlockParam hash -> ValueId (for VarAddr)
    tsl::robin_map<u32, i64> m_const_int_values;       // ValueId.id -> ConstInt value (used at call sites that need compile-time-known indices, e.g. roxy_map_alloc's hash_fn_index/eq_fn_index args)

    // User-registered natives referenced by CallNative ops in this module.
    // Populated by `collect_extern_native_decls()` before function bodies are
    // emitted; consumed by `emit_extern_native_decls()` to produce the source
    // preamble's `extern` block.
    tsl::robin_map<StringView, ExternNativeDecl> m_extern_native_decls;
};

} // namespace rx
