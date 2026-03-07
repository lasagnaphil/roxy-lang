#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/string.hpp"
#include "roxy/core/string_view.hpp"
#include "roxy/core/vector.hpp"
#include "roxy/core/bump_allocator.hpp"
#include "roxy/core/tsl/robin_set.h"
#include "roxy/compiler/ssa_ir.hpp"

namespace rx {

struct CEmitterConfig {
    Vector<String> native_include_paths;
    bool emit_main_entry = true;
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
};

} // namespace rx
