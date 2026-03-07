#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/string.hpp"
#include "roxy/core/string_view.hpp"
#include "roxy/core/vector.hpp"
#include "roxy/core/bump_allocator.hpp"
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

    // Collect type info for all SSA values in a function
    void collect_value_types(const IRFunction* func);

    // Configuration
    CEmitterConfig m_config;
    BumpAllocator& m_alloc;

    // Per-function state: maps ValueId.id -> Type*
    tsl::robin_map<u32, Type*> m_value_types;
};

} // namespace rx
