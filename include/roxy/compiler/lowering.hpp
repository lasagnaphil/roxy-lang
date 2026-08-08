#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/vector.hpp"
#include "roxy/compiler/ssa_ir.hpp"
#include "roxy/compiler/symbol_table.hpp"
#include "roxy/vm/bytecode.hpp"

#include "roxy/core/tsl/robin_map.h"
#include "roxy/core/tsl/robin_set.h"

#include <bit>

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
    // Insert a register into the active set, sorted by last_use (expiry order).
    void insert_active(u8 reg, u32 last_use);
    // True if the value is produced by a Call/CallNative/CallExternal/
    // CallIndirect — such values are never spillable (the call's argument
    // window is anchored at the result register).
    bool is_call_result(u32 value_id) const;
    // Allocate a call's dst register(s) plus its contiguous argument/return
    // window. Fast path bumps at the frame top (historical layout); when that
    // would exceed the 255-register frame limit, compacts the window into dead
    // register space just above the live values, spilling furthest-living
    // values if needed.
    void reserve_call_window(IRInst* inst, u32 extra_regs_for_return, u32 total_arg_regs);

    // Grow the register window to at least `needed_regs` for a call's
    // argument/return block. Stops if bump_register hits the 255-register cap
    // (which reports an error) so a call needing more than 255 registers fails
    // cleanly instead of looping forever.
    void ensure_register_window(u16 needed_regs);

    // Register spilling
    void spill_furthest();
    u8 get_result_register(ValueId value);
    u8 ensure_in_register(ValueId value, u8 scratch_index);
    // Emit the LOAD_INT/LOAD_CONST sequence for a Const{Int,F,D} definition
    // into `dst`. Shared by the normal const lowering path and the on-demand
    // materialization of skip-load constants in ensure_in_register.
    void emit_const_load(IRInst* const_def, u8 dst);
    void spill_if_needed(ValueId value, u8 reg);
    // Re-canonicalize a u32 result to zero-extended form (TRUNC_U 32). u32 values
    // must stay zero-extended so the 64-bit unsigned ops (LT_U/DIV_U/USHR) are
    // correct: 64-bit arithmetic can dirty bits >= 32, and GET_FIELD sign-extends
    // 1-slot loads. A no-op on already-canonical values, so callers may invoke it
    // after any u32-typed producer. Only emits when reg is a real register (not a
    // spill sentinel); call before spill_if_needed so the canonical value spills.
    void canonicalize_u32(IRInst* inst, u8 reg);

    // Liveness analysis
    void compute_liveness(IRFunction* ir_func);

    // Pre-pass: identifies Const{Int,F,D} SSA values whose every use lies in an
    // RK-eligible operand position (RHS of any RK op, or either side of a
    // commutative RK op). Such constants don't need a register or LOAD
    // instruction — the RK opcode reads them directly from the constant pool.
    // Populates m_requires_register (queried via is_skip_load_const()).
    void compute_const_use_modes(IRFunction* ir_func);

    // Free-list register allocation support
    void expire_before(u32 current_point);

    // Constant pool management
    u16 add_constant(const BCConstant& c);

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
    void lower_instruction(IRInst* inst);
    void lower_terminator(IRBlock* block);

    // Copy a call's arguments into its contiguous window starting at
    // first_arg_reg. callee_func supplies out/inout pointer-parameter info
    // (nullptr for natives and indirect calls, which have none). When
    // structs_in_registers is true, small-struct arguments (1-4 slots) are
    // packed into consecutive registers via STRUCT_LOAD_REGS (the CALL /
    // CALL_INDIRECT convention); natives instead take every struct by pointer.
    void emit_call_args(Span<ValueId> args, IRFunction* callee_func, u8 first_arg_reg,
                        bool structs_in_registers);

    // After a call whose small-struct return (1-4 slots) arrived packed in
    // consecutive registers at dst: store the packed slots to fresh stack
    // storage and leave the struct's address in dst, like every other struct
    // value. No-op for non-struct and large-struct (by-pointer) returns.
    void emit_small_struct_return_unpack(u8 dst, u32 ret_slot_count);

    // Shared lowering for Call and CallExternal — after static linking both
    // dispatch through the merged module's function table with the same
    // two-word CALL encoding.
    void lower_direct_call(IRInst* inst, u8 dst, StringView func_name, Span<ValueId> args,
                           const char* not_found_error);

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

    // Block offsets for jump resolution. BlockId.id is dense [0, blocks.size())
    // after reorder_blocks_rpo (which renumbers ids to their RPO index), so a
    // direct-indexed vector with a sentinel beats a hashed map here — the same
    // transformation that gave the ValueId side tables their win (dbd74a6).
    // See OPTIMIZATION.md §3.3.
    static constexpr u32 NO_OFFSET = 0xFFFFFFFFu;
    Vector<u32> m_block_offsets;  // BlockId.id -> code offset (NO_OFFSET = unset)

    // Code offset recorded for a block, or NO_OFFSET if the id is out of range
    // (e.g. a one-past-the-last probe) or the block was never emitted.
    u32 block_offset(u32 block_id) const {
        return block_id < m_block_offsets.size() ? m_block_offsets[block_id] : NO_OFFSET;
    }

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
    // borrow's cleanup-record scope_start to the RefInc (lifetimes.md "Counting mechanics" / "Promotion"):
    // the block-derived start would wrongly cover earlier-in-block argument
    // evaluation, so a throw there would RefDec a not-yet-initialized register.
    tsl::robin_map<u32, u32> m_ref_inc_pcs;
    // ValueId-indexed: the PC just past the instruction that produces each value,
    // i.e. the first PC at which its register actually holds it. Cleanup records
    // start no earlier than this; see the narrowing in build_cleanup_records.
    // A dense side table, not a hash map, because it is written for *every*
    // result-producing instruction and read only a handful of times per function
    // — the same reason m_value_to_reg is one (see profiling.md).
    static constexpr u32 NO_READY_PC = UINT32_MAX;
    Vector<u32> m_value_ready_pcs;

    // Exception-unwind coverage for block-scoped cleanup records: for each
    // cleanup_info entry, the sorted set of block ids (== layout positions after
    // RPO reorder) where the record's value is owned on the way to a potential
    // throw — every block reachable from its start block without passing an
    // ownership-ending kill (Nullify, or the record's own cleanup op). This can
    // exceed the record's single [start, end) PC interval: RPO lays a
    // throw-terminated branch out *after* the scope's normal-exit block, so the
    // interval truncated at the Nullify misses it and the value leaks
    // (lifetimes.md "Applying the model"). The coverage feeds two consumers:
    // liveness Pass 5b pins the value's register across all covered blocks, and
    // build() emits extension records (BCCleanupRecord::is_extension) for
    // covered PC runs outside the main interval. Empty for records the
    // machinery doesn't apply to (call borrows, Unpin, whole-function refs).
    Vector<Vector<u32>> m_cleanup_covered_blocks;
    void compute_cleanup_coverage(IRFunction* ir_func);

    // Ownership-ending PCs per tracked cleanup value: every Nullify annotation
    // plus every lowered Delete/StrRelease/RefDec on the value (PC just past the
    // releasing instruction, matching m_nullify_pcs' convention). Keys are
    // pre-seeded by compute_cleanup_coverage for eligible record values only, so
    // the hot StrRelease/RefDec lowering paths append nothing for untracked
    // values. Used to truncate an extension run at the kill inside its block —
    // unlike m_nullify_pcs, which keeps only the last PC for the legacy
    // single-interval narrowing.
    tsl::robin_map<u32, Vector<u32>> m_cleanup_kill_pcs;

    // Append the current PC (just past the last emitted instruction) to the
    // value's kill list, if a cleanup record tracks the value.
    void record_cleanup_kill_pc(ValueId value) {
        if (!value.is_valid()) return;
        auto it = m_cleanup_kill_pcs.find(value.id);
        if (it != m_cleanup_kill_pcs.end()) {
            it.value().push_back(static_cast<u32>(m_current_func->code.size()));
        }
    }

    // Consumption boundaries within the current block: ValueId -> PC just past
    // the last CALL-family instruction that used the value as an argument.
    // Cleared at each block start. A moved argument's ownership transfers at
    // the call, and a throw escaping the callee surfaces at exactly this PC
    // (the word after the call instruction) — but the move's Nullify is only
    // lowered after the call's return-value materialization, several
    // instructions later. Anchoring the kill at the Nullify's own PC would
    // leave that gap covered, so an escaping throw would free a value the
    // callee already owned (and freed). Tracked values only.
    tsl::robin_map<u32, u32> m_call_use_pcs;
    u32 m_block_start_pc = 0;

    // Note a call-argument use of a tracked value; call right after emitting
    // the call instruction words (before return materialization).
    void note_call_use(ValueId value) {
        if (!value.is_valid()) return;
        if (m_cleanup_kill_pcs.find(value.id) != m_cleanup_kill_pcs.end()) {
            m_call_use_pcs[value.id] = static_cast<u32>(m_current_func->code.size());
        }
    }

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
    // referential descriptor instead of looping forever at compile time. The
    // *kind* of drop is decided by the shared compute_drop_plan (types.hpp); this
    // lowers that plan to a BCDeleteDesc. lifetimes.md "Value lifecycle".
    u16 build_delete_desc(Type* type);

    // Append the field-cleanup actions for an eligible struct (regular owned
    // fields + discriminant-guarded variant fields) to
    // m_current_func->struct_field_deletes, returning [start, count).
    void build_struct_field_deletes(Type* struct_type, u16& out_start, u16& out_count);

    // Resolve the bytecode function index of a struct's default destructor
    // ("StructName$$delete"), or 0 if not found.
    u16 lookup_destructor_index(Type* struct_type) const;

    // Per-function memoization for build_delete_desc: Type* -> descriptor index.
    // Cleared at the start of each function. Reservation-before-recursion lets
    // self-referential structs reference their own (in-progress) descriptor.
    tsl::robin_map<Type*, u16> m_delete_desc_cache;

    // Get slot count for a struct type, or 0 if not a struct
    u32 get_struct_slot_count(Type* type) const;

    // Get the number of registers needed for a value of the given type
    // Returns 2 for weak refs (128-bit), struct-based counts for structs, 1 for everything else
    u32 get_value_reg_count(Type* type) const;

    // Registers a call window reserves above dst for a multi-register return:
    // 2 for weak refs, the packed slot-pair count for small structs (1-4
    // slots), 0 when the return fits in dst alone (scalars, and large structs
    // returned through a hidden pointer).
    u32 call_return_extra_regs(Type* type) const;

    // Current function being built
    BCFunction* m_current_func;
    IRFunction* m_current_ir_func;

    // ValueId.id -> register, as a dense side table (NO_REG = not in a register).
    // ValueIds are contiguous in [0, next_value_id), so a flat vector is a direct
    // O(1) index with no hashing and a cheap per-function refill — far cheaper than
    // a robin_map on this hot path (register allocation; see profiling.md). Stored
    // as u16 so registers 0..255 all fit alongside the out-of-band NO_REG sentinel.
    static constexpr u16 NO_REG = 0xFFFF;
    Vector<u16> m_value_to_reg;
    u16 m_next_reg;  // u16 to prevent silent wraparound; capped at 255

    // Liveness data (computed per function)
    Vector<LiveRange> m_live_ranges;
    Vector<bool> m_value_same_block;    // true if value's def and last use are in the same block

    // Dense ValueId-indexed flag: true if the value has at least one use that
    // requires a register. Built by compute_const_use_modes(). A numeric Const*
    // whose flag is false is skip-load eligible (no LOAD, no register) — see
    // is_skip_load_const(). Direct-indexed vector, not a robin_set, since the
    // ids are dense and it's probed twice per result-producing instruction (§3.8).
    Vector<bool> m_requires_register;

    // A Const{Int,F,D} SSA value is skip-load eligible iff no use requires a
    // register. Derived from m_requires_register (built by compute_const_use_modes).
    bool is_skip_load_const(const IRInst* inst) const {
        if (!inst->result.is_valid() || inst->result.id >= m_requires_register.size())
            return false;
        switch (inst->op) {
            case IROp::ConstInt:
            case IROp::ConstF:
            case IROp::ConstD:
                return !m_requires_register[inst->result.id];
            default:
                return false;
        }
    }

    // Free-register set: a 256-bit mask (register r is free iff bit r is set).
    // Replaces a linear-min-scanned Vector<u8> — "smallest free register" is now
    // a count-trailing-zeros over 4 words instead of a full scan, and freeing a
    // register is a single bit-set. See §4.2. No duplicates possible (set
    // semantics), matching the old invariant that a free register appears once.
    u64 m_free_mask[4];

    void free_regs_reset() { m_free_mask[0] = m_free_mask[1] = m_free_mask[2] = m_free_mask[3] = 0; }
    void free_reg_add(u8 r) { m_free_mask[r >> 6] |= (u64(1) << (r & 63)); }
    bool free_regs_empty() const {
        return (m_free_mask[0] | m_free_mask[1] | m_free_mask[2] | m_free_mask[3]) == 0;
    }
    // Take the lowest free register; caller must ensure !free_regs_empty().
    u8 free_reg_take_min() {
        for (u32 w = 0; w < 4; w++) {
            if (m_free_mask[w] != 0) {
                u32 bit = static_cast<u32>(std::countr_zero(m_free_mask[w]));
                m_free_mask[w] &= m_free_mask[w] - 1;  // clear lowest set bit
                return static_cast<u8>(w * 64 + bit);
            }
        }
        return 0xFF;  // unreachable when non-empty
    }
    void free_reg_clear_range(u8 base, u32 count) {
        for (u32 r = base; r < static_cast<u32>(base) + count; r++) {
            m_free_mask[r >> 6] &= ~(u64(1) << (r & 63));
        }
    }

    Vector<ActiveAlloc> m_active;      // sorted by last_use ascending

    // Register spilling state
    tsl::robin_map<u32, u32> m_spill_slots;   // ValueId.id -> spill stack slot offset
    // Reverse map register -> ValueId.id, as a fixed 256-entry table (NO_VALUE =
    // register free). Only 256 possible register keys, so an array beats a map.
    static constexpr u32 NO_VALUE = UINT32_MAX;  // == ValueId::invalid().id
    u32 m_reg_to_value[256];
    bool m_has_spilling;
    u8 m_scratch_regs[2];                     // two scratch registers for reload/spill

    u32 m_next_stack_slot;

    // ValueId.id -> Type*, as a dense side table (nullptr = unknown). ValueIds are
    // contiguous, so a flat vector replaces the robin_map here too.
    Vector<Type*> m_value_types;

    // Type of a value, or nullptr if unknown / out of range.
    Type* value_type_of(u32 id) const {
        return id < m_value_types.size() ? m_value_types[id] : nullptr;
    }

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
