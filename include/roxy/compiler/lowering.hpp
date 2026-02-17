#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/vector.hpp"
#include "roxy/compiler/ssa_ir.hpp"
#include "roxy/compiler/symbol_table.hpp"
#include "roxy/vm/bytecode.hpp"

#include "roxy/core/tsl/robin_map.h"

namespace rx {

class NativeRegistry;

// BytecodeBuilder lowers SSA IR to bytecode
class BytecodeBuilder {
public:
    BytecodeBuilder();

    // Set native registry (needed for copy constructor emission)
    void set_registry(NativeRegistry* registry) { m_registry = registry; }

    // Build bytecode module from IR module
    // Returns nullptr if an internal error occurred
    BCModule* build(IRModule* ir_module);

    // Build bytecode for a single function
    BCFunction* build_function(IRFunction* ir_func);

    // Error reporting
    bool has_error() const { return m_has_error; }
    const char* error() const { return m_error; }

private:
    // Report an internal compiler error
    void report_error(const char* message);
    // Register allocation - maps ValueId to register number
    u8 allocate_register(ValueId value);
    u8 get_register(ValueId value);
    bool has_register(ValueId value) const;

    // Constant pool management
    u16 add_constant(const BCConstant& c);
    u16 add_int_constant(i64 value);
    u16 add_float_constant(f64 value);
    u16 add_string_constant(const char* data, u32 length);

    // Block lowering
    void lower_block(IRBlock* block);
    void lower_instruction(IRInst* inst);
    void lower_terminator(IRBlock* block);

    // Block argument handling at jump sites
    void emit_block_args(const JumpTarget& target);

    // Emit bytecode instruction
    void emit(u32 instr);
    void emit_abc(Opcode op, u8 a, u8 b, u8 c);
    void emit_abi(Opcode op, u8 a, u16 imm);
    void emit_aoff(Opcode op, u8 a, i16 offset);

    // Jump patching
    struct JumpPatch {
        u32 instruction_index;  // Index of instruction to patch
        BlockId target_block;   // Target block
    };
    Vector<JumpPatch> m_jump_patches;

    // Block offsets for jump resolution
    tsl::robin_map<u32, u32> m_block_offsets;  // BlockId.id -> code offset

    // Patch jump offsets after all blocks are emitted
    void patch_jumps();

    // Get opcode for IR operation
    Opcode get_opcode(IROp op) const;

    // Emit cast bytecode based on source and target types
    void emit_cast_bytecode(u8 dst, u8 src, Type* source_type, Type* target_type);

    // Check if type is a struct that should be passed by reference (>4 slots)
    bool is_large_struct(Type* type) const;

    // Get slot count for a struct type, or 0 if not a struct
    u32 get_struct_slot_count(Type* type) const;

    // Current function being built
    BCFunction* m_current_func;
    IRFunction* m_current_ir_func;

    // Register allocation map: ValueId.id -> register
    tsl::robin_map<u32, u8> m_value_to_reg;
    u8 m_next_reg;

    // Local stack allocation: maps ValueId.id -> stack slot offset
    tsl::robin_map<u32, u32> m_value_to_stack_slot;

    // Variable name to stack slot mapping (for VarAddr on local variables)
    tsl::robin_map<StringView, u32> m_var_name_to_stack_slot;

    u32 m_next_stack_slot;

    // Type information: maps ValueId.id -> Type*
    tsl::robin_map<u32, Type*> m_value_types;

    // Current module
    BCModule* m_module;
    IRModule* m_ir_module;

    // Function name to index mapping
    tsl::robin_map<StringView, u32> m_func_indices;

    // Type name to index mapping (for heap allocation)
    tsl::robin_map<StringView, u16> m_type_indices;

    // Native registry for copy constructor lookup
    NativeRegistry* m_registry = nullptr;

    // Error state
    bool m_has_error;
    const char* m_error;
};

}
