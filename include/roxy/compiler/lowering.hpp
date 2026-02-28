#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/vector.hpp"
#include "roxy/compiler/ssa_ir.hpp"
#include "roxy/compiler/symbol_table.hpp"
#include "roxy/vm/bytecode.hpp"

#include "roxy/core/tsl/robin_map.h"

namespace rx {

// 16-bit signed immediate range for bytecode encoding
constexpr i64 IMM16_MIN = -32768;
constexpr i64 IMM16_MAX = 32767;

class NativeRegistry;
class TypeEnv;

// Liveness data for register allocation
struct LiveRange {
    u32 def_point;       // program point where value is defined
    u32 last_use_point;  // latest program point where value is read
};

// Active allocation entry for free-list register allocator
struct ActiveAlloc {
    u32 last_use;  // when this value dies
    u8 reg;        // its allocated register
};

// BytecodeBuilder lowers SSA IR to bytecode
class BytecodeBuilder {
public:
    BytecodeBuilder();

    // Set native registry (needed for copy constructor emission)
    void set_registry(NativeRegistry* registry) { m_registry = registry; }

    // Set type environment (needed for exception handler type resolution)
    void set_type_env(TypeEnv* type_env) { m_type_env = type_env; }

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
    u8 bump_register();  // Allocate next fresh register with bounds check

    // Register spilling
    void spill_furthest();
    u8 get_result_register(ValueId value);
    u8 ensure_in_register(ValueId value, u8 scratch_index);
    void spill_if_needed(ValueId value, u8 reg);

    // Liveness analysis
    void compute_liveness(IRFunction* ir_func);

    // Free-list register allocation support
    void expire_before(u32 current_point);

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

    // Get the number of registers needed for a value of the given type
    // Returns 2 for weak refs (128-bit), struct-based counts for structs, 1 for everything else
    u32 get_value_reg_count(Type* type) const;

    // Current function being built
    BCFunction* m_current_func;
    IRFunction* m_current_ir_func;

    // Register allocation map: ValueId.id -> register
    tsl::robin_map<u32, u8> m_value_to_reg;
    u16 m_next_reg;  // u16 to prevent silent wraparound; capped at 255

    // Liveness data (computed per function)
    Vector<LiveRange> m_live_ranges;
    Vector<bool> m_value_same_block;    // true if value's def and last use are in the same block

    // Free-list allocator state
    Vector<u8> m_free_regs;            // pool of available register numbers
    Vector<ActiveAlloc> m_active;      // sorted by last_use ascending

    // Register spilling state
    tsl::robin_map<u32, u32> m_spill_slots;   // ValueId.id -> spill stack slot offset
    tsl::robin_map<u8, u32> m_reg_to_value;   // reverse map: register -> ValueId.id
    bool m_has_spilling;
    u8 m_scratch_regs[2];                     // two scratch registers for reload/spill

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

    // Type environment for exception type resolution
    TypeEnv* m_type_env = nullptr;

    // Error state
    bool m_has_error;
    const char* m_error;
};

}
