#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/vector.hpp"
#include "roxy/compiler/ssa_ir.hpp"
#include "roxy/compiler/symbol_table.hpp"
#include "roxy/vm/bytecode.hpp"

#include "roxy/core/tsl/robin_map.h"
#include "roxy/core/tsl/robin_set.h"

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

    // Grow the register window to at least `needed_regs` for a call's
    // argument/return block. Stops if bump_register hits the 255-register cap
    // (which reports an error) so a call needing more than 255 registers fails
    // cleanly instead of looping forever.
    void ensure_register_window(u16 needed_regs);

    // Register spilling
    void spill_furthest();
    u8 get_result_register(ValueId value);
    u8 ensure_in_register(ValueId value, u8 scratch_index);
    void spill_if_needed(ValueId value, u8 reg);

    // Liveness analysis
    void compute_liveness(IRFunction* ir_func);

    // Pre-pass: identifies Const{Int,F,D} SSA values whose every use lies in an
    // RK-eligible operand position (RHS of any RK op, or either side of a
    // commutative RK op). Such constants don't need a register or LOAD
    // instruction — the RK opcode reads them directly from the constant pool.
    // Populates m_const_skip_load.
    void compute_const_use_modes(IRFunction* ir_func);

    // Free-list register allocation support
    void expire_before(u32 current_point);

    // Constant pool management
    u16 add_constant(const BCConstant& c);
    u16 add_int_constant(i64 value);
    u16 add_float_constant(f64 value);
    u16 add_string_constant(const char* data, u32 length);

    // RK (register-or-constant) helpers — return existing pool index or append.
    // i32 result so caller can detect "would overflow u8 RK index" via < 0 check
    // is unnecessary; use the boolean return from try_emit_rk_binary.
    i32 get_or_add_int_constant(i64 value);
    i32 get_or_add_float_constant(f64 value);

    // Map IROp binary op to its RK opcode variant. Returns Opcode::NOP if the
    // op has no RK variant.
    Opcode rk_opcode_for(IROp op) const;
    static bool is_commutative_binary(IROp op);

    // If `inst`'s right (or, for commutative ops, either) operand is a constant
    // SSA value AND fits in the 8-bit RK constant-pool index, emit the RK form
    // and return true. Otherwise emit nothing and return false (caller falls
    // back to the non-RK encoding).
    bool try_emit_rk_binary(IRInst* inst, u8 dst);

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

    // Bytecode PCs of integer-compare instructions whose SSA result is live past
    // this block's terminator (i.e. !m_value_same_block). These must NOT be fused
    // with the following JMP_IF/JMP_IF_NOT: the fusion drops the register write,
    // so a later block's read of the same SSA value (e.g. the second `if (bool)`
    // after a loop) would see an uninitialized/stale register.
    tsl::robin_set<u32> m_unfusable_cmp_pcs;

    // Nullify positions: tracks where ownership was transferred for each value.
    // Used to narrow cleanup record scopes (Nullify is a compile-time annotation,
    // not a runtime instruction).
    tsl::robin_map<u32, u32> m_nullify_pcs;  // ValueId.id -> PC where ownership transferred
    // ValueId.id -> PC of its RefInc. Used to narrow a call-site receiver
    // borrow's cleanup-record scope_start to the RefInc (lifetimes.md §4/§6):
    // the block-derived start would wrongly cover earlier-in-block argument
    // evaluation, so a throw there would RefDec a not-yet-initialized register.
    tsl::robin_map<u32, u32> m_ref_inc_pcs;

    // Patch jump offsets after all blocks are emitted
    void patch_jumps();

    // Signed branch offset from `from_idx` to `to_idx` (PC-relative, accounting
    // for the +1 advance). Reports an error and returns 0 if the distance does
    // not fit the i16 AOFF field, rather than silently truncating to a wrong
    // target — a function with >32K code words between branch and target.
    i16 branch_offset(u32 from_idx, u32 to_idx);

    // Fuse adjacent compare + conditional branch into single two-word instruction
    void fuse_compare_branch();

    // Get opcode for IR operation
    Opcode get_opcode(IROp op) const;

    // Emit cast bytecode based on source and target types
    void emit_cast_bytecode(u8 dst, u8 src, Type* source_type, Type* target_type);

    // Build delete descriptor tree for noncopyable types.
    // Recursively walks the type and appends BCDeleteDesc entries to
    // m_current_func->delete_descs. Returns the index of the root entry.
    // Memoized per function via m_delete_desc_cache so recursive types
    // (e.g. struct Node { next: uniq Node; }) produce a finite, self-
    // referential descriptor instead of looping forever at compile time.
    u16 build_delete_desc(Type* type);

    // True if `struct_type`'s owned-field cleanup can be expressed as a
    // descriptor-driven STRUCT delete desc (kind 5/6) rather than a bytecode
    // destructor. Requires a synthetic (non-user) default destructor and no
    // parent, so the whole cleanup is pure data and avoids interpreter
    // re-entry during destruction. Tagged-union (when-clause) structs qualify.
    bool is_descriptor_eligible_struct(Type* struct_type) const;

    // Append the field-cleanup actions for an eligible struct (regular owned
    // fields + discriminant-guarded variant fields) to
    // m_current_func->struct_field_deletes, returning [start, count).
    void build_struct_field_deletes(Type* struct_type, u16& out_start, u16& out_count);

    // True if the struct type has a default (empty-name) destructor.
    bool struct_has_default_destructor(Type* struct_type) const;

    // Resolve the bytecode function index of a struct's default destructor
    // ("StructName$$delete"), or 0 if not found.
    u16 lookup_destructor_index(Type* struct_type) const;

    // Per-function memoization for build_delete_desc: Type* -> descriptor index.
    // Cleared at the start of each function. Reservation-before-recursion lets
    // self-referential structs reference their own (in-progress) descriptor.
    tsl::robin_map<Type*, u16> m_delete_desc_cache;

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

    // Const SSA values whose LOAD_INT/LOAD_CONST emission can be skipped
    // because every use is RK-eligible. Populated by compute_const_use_modes().
    tsl::robin_set<u32> m_const_skip_load;

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
