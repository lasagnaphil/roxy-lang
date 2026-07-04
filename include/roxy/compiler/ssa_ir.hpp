#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/span.hpp"
#include "roxy/core/string.hpp"
#include "roxy/core/string_view.hpp"
#include "roxy/core/vector.hpp"
#include "roxy/core/bump_allocator.hpp"
#include "roxy/core/tsl/robin_map.h"
#include "roxy/compiler/types.hpp"

namespace rx {

// Forward declarations
struct IRValue;
struct IRInst;
struct IRBlock;
struct IRFunction;
struct IRModule;
struct Expr;   // AST node (compiler/ast.hpp); IRGlobal holds a global's initializer

// Value ID - uniquely identifies an SSA value within a function
struct ValueId {
    u32 id;

    bool operator==(ValueId other) const { return id == other.id; }
    bool operator!=(ValueId other) const { return id != other.id; }
    bool is_valid() const { return id != UINT32_MAX; }

    static ValueId invalid() { return ValueId{UINT32_MAX}; }
};

// Block ID - uniquely identifies a block within a function
struct BlockId {
    u32 id;

    bool operator==(BlockId other) const { return id == other.id; }
    bool operator!=(BlockId other) const { return id != other.id; }
    bool is_valid() const { return id != UINT32_MAX; }

    static BlockId invalid() { return BlockId{UINT32_MAX}; }
};

// IR instruction opcodes
enum class IROp : u8 {
    // Constants
    ConstNull,      // null constant
    ConstBool,      // boolean constant
    ConstInt,       // integer constant
    ConstF,         // f32 constant (stores f32 bits as u32)
    ConstD,         // f64 constant (stores f64 bits)
    ConstString,    // string constant

    // Arithmetic (integer)
    AddI,
    SubI,
    MulI,
    DivI,
    ModI,
    NegI,

    // Arithmetic (f32)
    AddF,
    SubF,
    MulF,
    DivF,
    NegF,

    // Arithmetic (f64)
    AddD,
    SubD,
    MulD,
    DivD,
    NegD,

    // Comparison (integer)
    EqI,
    NeI,
    LtI,
    LeI,
    GtI,
    GeI,

    // Comparison (f32)
    EqF,
    NeF,
    LtF,
    LeF,
    GtF,
    GeF,

    // Comparison (f64)
    EqD,
    NeD,
    LtD,
    LeD,
    GtD,
    GeD,

    // Logical
    Not,
    And,
    Or,

    // Bitwise
    BitAnd,
    BitOr,
    BitXor,
    BitNot,
    Shl,
    Shr,

    // Type conversions
    I_TO_F64,       // int to f64
    F64_TO_I,       // f64 to int
    I_TO_B,         // int to bool
    B_TO_I,         // bool to int

    // Memory operations
    StackAlloc,     // allocate slots on local stack, returns pointer
    GlobalAddr,     // &globals[slot_offset] (address of a module-global slot)
    GetField,       // obj.field (load value)
    GetFieldAddr,   // &obj.field (compute address, for nested struct access)
    SetField,       // obj.field = value
    // Reference operations
    RefInc,         // increment ref count
    RefDec,         // decrement ref count
    StrRetain,      // retain a reference-counted string (owner++; no-op if immortal)
    StrRelease,     // release a reference-counted string (owner--; free at zero)
    WeakCheck,      // check if weak ref is valid
    WeakCreate,     // create weak ref from pointer (extracts generation)

    // Object lifecycle
    New,            // allocate new object
    Delete,         // deallocate object

    // Closure construction: allocate env struct, store call_idx in first u32 field,
    // store capture values in subsequent fields. Result is the env pointer typed as
    // Function<sig>. Lowering expands to NEW_OBJ + a sequence of SetField writes.
    Closure,

    // Trap if `unary` (a pointer) is not owned by the slab allocator. Used for
    // closure captures of `self` (ref or weak) where the receiver might be
    // stack-allocated; copyable struct methods can't statically prove heap
    // residence so we check at construction time.
    AssertHeap,

    // Function call
    Call,           // call function
    CallNative,     // call native function
    CallExternal,   // call function in another module
    CallIndirect,   // call closure: read __call_idx from env's first u32 field, dispatch with env as first arg

    // Container indexing (List/Map)
    IndexGet,       // container[index] — for List/Map
    IndexSet,       // container[index] = value — for List/Map
    IndexAddr,      // &container[index] — element lvalue address (out/inout args; lifetimes.md "Container element lvalues")
    ContainerPin,   // pin a container for a call (borrow_count++) — blocks realloc/free while an element is borrowed
    ContainerUnpin, // unpin a container after a call (borrow_count--)

    // Block argument (phi-like)
    BlockArg,       // Block parameter - receives value from predecessor

    // Copy (for SSA value passing)
    Copy,           // Simple copy of a value

    // Struct operations
    StructCopy,     // Memory-to-memory struct copy

    // Pointer operations (for out/inout parameters)
    LoadPtr,        // Load value through pointer: dst = *ptr
    StorePtr,       // Store value through pointer: *ptr = val

    // Type casting
    Cast,           // Generic cast - uses source_type in CastData

    // Cleanup support
    Nullify,        // Zero the register of a specified value (for move null-ification).
                    // unary = the value whose register should be zeroed.
                    // Used to invalidate cleanup records after ownership transfer.

    // Exception handling
    Throw,          // Throw exception: unary = exception object pointer. Block terminator.

    // Coroutine
    Yield,          // Yield a value from coroutine: unary = yielded value. Block terminator.
};

// Constant data for ConstXxx instructions
struct ConstData {
    union {
        bool bool_val;
        i64 int_val;
        f32 f32_val;       // For ConstF
        f64 f64_val;       // For ConstD
        StringView string_val;
    };

    ConstData() : int_val(0) {}
};

// Call instruction data
struct CallData {
    StringView func_name;
    Span<ValueId> args;
    u32 native_index;  // For CallNative: index into module's native_functions
};

// External call instruction data (for cross-module calls)
struct CallExternalData {
    StringView module_name;
    StringView func_name;
    Span<ValueId> args;
};

// Indirect call data (for closures): callee is the env pointer (typed as Function<sig>);
// the runtime loads __call_idx from the env's first u32 field and dispatches with the
// env pointer prepended as the first argument.
struct CallIndirectData {
    ValueId callee;           // SSA value: env pointer (Function<sig>-typed)
    Span<ValueId> args;       // Explicit arguments (do NOT include the env pointer)
};

// Closure construction data: lowering expands to NEW_OBJ(env_struct_name) plus a
// sequence of SetField writes — first __call_idx (resolved at lowering time from
// call_function_name) then any captured values (in declaration order).
struct ClosureData {
    StringView env_struct_name;
    StringView call_function_name;
    Span<ValueId> captures;
};

// Field access data
struct FieldData {
    ValueId object;
    StringView field_name;
    u32 slot_offset;   // Offset in u32 slots from struct start
    u32 slot_count;    // Number of u32 slots (1 for 32-bit, 2 for 64-bit)
};

// New object data
struct NewData {
    StringView type_name;
    Span<ValueId> args;
};

// Stack allocation data
struct StackAllocData {
    u32 slot_count;     // Number of u32 slots to allocate
};

// Global address data (for GlobalAddr)
struct GlobalData {
    u32 slot_offset;    // Offset into the module's global slot array
};

// Struct copy data
struct StructCopyData {
    ValueId dest_ptr;
    ValueId source_ptr;
    u32 slot_count;
};

// Load through pointer data
struct LoadPtrData {
    ValueId ptr;
    u32 slot_count;  // Number of slots to load (1 or 2 for primitives)
};

// Store through pointer data
struct StorePtrData {
    ValueId ptr;
    ValueId value;
    u32 slot_count;  // Number of slots to store (1 or 2 for primitives)
};

// Container kind for IndexGet/IndexSet
enum class ContainerKind : u8 { List, Map };

// Index access data (for IndexGet/IndexSet)
struct IndexData {
    ValueId container;
    ValueId index;       // index for List, key for Map
    ValueId value;       // only used by IndexSet
    ContainerKind kind;
};

// Cast data
struct CastData {
    ValueId source;
    Type* source_type;  // Source type for determining conversion strategy
};

// IR Instruction - represents a single operation that produces a value
struct IRInst {
    IROp op;
    ValueId result;     // The SSA value this instruction produces
    Type* type;         // Result type

    // 1-indexed source line of the AST node that produced this instruction.
    // Populated by `IRBuilder::emit_inst` from `m_current_source_line`,
    // which `gen_stmt`/`gen_decl` set at each statement boundary. Used by
    // the C backend to emit per-statement `#line` directives so debuggers
    // attribute IR-generated C code to the right Roxy source line. 0 =
    // unknown / synthesized (built-ins, coroutine lowering, etc.).
    u32 source_line = 0;

    // Operands (usage depends on op)
    union {
        ConstData const_data;           // For Const* ops
        struct {
            ValueId left;
            ValueId right;
        } binary;                       // For binary ops
        ValueId unary;                  // For unary ops
        CallData call;                  // For Call/CallNative
        CallExternalData call_external; // For CallExternal
        CallIndirectData call_indirect; // For CallIndirect
        ClosureData closure;            // For Closure
        FieldData field;                // For GetField/SetField
        NewData new_data;               // For New
        StackAllocData stack_alloc;     // For StackAlloc
        GlobalData global_data;         // For GlobalAddr
        StructCopyData struct_copy;     // For StructCopy
        LoadPtrData load_ptr;           // For LoadPtr
        StorePtrData store_ptr;         // For StorePtr
        CastData cast;                  // For Cast
        IndexData index_data;           // For IndexGet/IndexSet
        u32 block_arg_index;            // For BlockArg (parameter index)
    };

    // For SetField, the value being stored
    ValueId store_value;

    // Pinned `Copy`: copy propagation must NOT collapse this Copy into its
    // source. Used by call-site receiver borrows (lifetimes.md "Counting mechanics" / "Promotion") to mint a
    // distinct SSA value/register for a heap receiver, so the borrow's RefDec +
    // Nullify cleanup can't collide with the owned-local's own Delete record
    // (which shares the receiver's value). Only ever set on IROp::Copy.
    bool no_copy_prop = false;

    IRInst() : op(IROp::ConstNull), result(ValueId::invalid()), type(nullptr), store_value(ValueId::invalid()) {
        const_data = ConstData();
    }
    ~IRInst() {}
};

// Terminator kinds
enum class TerminatorKind : u8 {
    None,           // Block not yet terminated
    Goto,           // Unconditional jump
    Branch,         // Conditional branch
    Return,         // Function return
    Unreachable,    // Unreachable code
};

// Block argument passed to successor
struct BlockArgPair {
    ValueId value;  // The value being passed
};

// Goto/Branch target with block arguments
struct JumpTarget {
    BlockId block;
    Span<BlockArgPair> args;  // Values passed as block arguments
};

// Block terminator
struct Terminator {
    TerminatorKind kind;

    union {
        JumpTarget goto_target;              // For Goto
        struct {
            ValueId condition;
            JumpTarget then_target;
            JumpTarget else_target;
        } branch;                            // For Branch
        ValueId return_value;                // For Return (invalid if void)
    };

    Terminator() : kind(TerminatorKind::None) {}
};

// Block parameter (receives value from predecessors)
struct BlockParam {
    ValueId value;      // The SSA value representing this parameter
    Type* type;
    StringView name;    // Optional debug name
};

// Basic block with block arguments
struct IRBlock {
    BlockId id;
    StringView name;                    // Optional debug name (e.g., "entry", "loop", "exit")
    Vector<BlockParam> params;          // Block parameters (block arguments)
    Vector<IRInst*> instructions;       // Non-terminating instructions
    Terminator terminator;              // Block terminator

    // Predecessors (computed during analysis)
    Vector<BlockId> predecessors;

    IRBlock() : id(BlockId::invalid()) {}
};

// Exception handler metadata for IR
struct IRExceptionHandler {
    BlockId try_entry;       // First block of try body
    BlockId try_exit;        // Last block of try body (inclusive)
    BlockId handler_block;   // Catch handler entry block
    u32 type_id = 0;         // Concrete type_id to match (0 = catch-all)
    StringView type_name = StringView(nullptr, 0);  // Struct name for the catch type (empty for catch-all)
    // Every IR block that belongs to the try body. Populated in creation order
    // in gen_try_stmt (before RPO reorder), then remapped by reorder_blocks_rpo.
    // Lowering turns this set into one BCExceptionHandler per contiguous run
    // of layout positions — without this, a try body containing a loop (whose
    // body block lands after the layout fall-through in RPO order) falls
    // outside a single [try_start_pc, try_end_pc) window.
    Vector<BlockId> try_body_blocks;
};

// Finally handler metadata for IR
struct IRFinallyInfo {
    BlockId try_entry;
    BlockId try_exit;
    BlockId finally_block;
    BlockId finally_end_block;  // Block after finally (for normal flow)
};

// Cleanup action for an exception-path cleanup record.
//   Delete — run the type's descriptor-driven destruction (uniq/list/map/...).
//   RefDec — decrement a `ref` borrow's count (constraint-reference model).
//   Unpin  — decrement a container's element-borrow pin (lifetimes.md "Container element lvalues").
enum class IRCleanupKind : u8 { Delete, RefDec, Unpin, StrRelease };

// Cleanup info for owned locals and ref borrows (used to generate bytecode
// cleanup records for the exception-unwind path).
struct IRCleanupInfo {
    ValueId value;        // The SSA value to clean up
    Type* type;           // Determines cleanup kind (uniq, struct w/ dtor, list, map)
    BlockId start_block;  // First block where variable is live
    BlockId end_block;    // Last block where variable is live (scope exit)
    IRCleanupKind kind = IRCleanupKind::Delete;  // Delete owned value vs RefDec a borrow
    // Ref *parameters* are borrows live for the whole function; their cleanup
    // record spans [0, code.size()) and their register is pinned to the final
    // point (the block-derived scope is unreliable when every path returns or
    // throws). Ref *locals* and owned locals are block-scoped (false).
    bool whole_function_scope = false;
    // Call-site receiver borrow (lifetimes.md "Counting mechanics" / "Promotion"): a heap receiver counted
    // for one method call's duration. The borrow lives only [RefInc, RefDec)
    // around a single call, so lowering narrows scope_start to the RefInc's PC
    // (via m_ref_inc_pcs) in addition to the Nullify end-narrowing — the
    // block-derived start would wrongly cover earlier-in-block argument
    // evaluation, firing a RefDec on a not-yet-initialized register on throw.
    bool call_borrow = false;
};

// IR Function
struct IRFunction {
    StringView name;
    Type* return_type;
    Vector<BlockParam> params;          // Function parameters (become entry block args)
    Vector<bool> param_is_ptr;          // True if param is a pointer (out/inout parameter)
    Vector<IRBlock*> blocks;            // Basic blocks, blocks[0] is entry
    u32 next_value_id;                  // Counter for generating unique value IDs

    // Defining instruction for each ValueId (indexed by id). Slot is nullptr for
    // ValueIds that don't correspond to an emitted IRInst* (function params, block
    // params). Populated by new_value() (nullptr) and emit_inst() (real pointer).
    // Used by Phase 1 fold/simplify and reused by later optimization phases.
    Vector<IRInst*> values_by_id;

    // Exception handling metadata
    Vector<IRExceptionHandler> exception_handlers;
    Vector<IRFinallyInfo> finally_handlers;

    // Cleanup records for owned locals (for exception-path cleanup)
    Vector<IRCleanupInfo> cleanup_info;

    // True for `pub` declarations; consulted by the C backend to decide what
    // belongs in the generated public header. Synthesized default ctors/dtors
    // inherit visibility from their struct decl.
    bool is_pub = false;

    // 1-indexed source line where this function's declaration begins.
    // Populated by IRBuilder from the AST decl's `loc.line`; used by the C
    // backend to emit `#line N "<source>"` directives at the start of each
    // generated function body so debuggers attribute the body to the
    // original Roxy source. 0 = unknown / synthesized function.
    u32 source_line = 0;

    // Coroutine metadata (set by IR builder for functions returning Coro<T>)
    bool is_coroutine = false;
    Type* coro_yield_type = nullptr;     // T in Coro<T>
    Type* coro_struct_type = nullptr;    // The synthetic struct holding coroutine state
    Type* coro_type = nullptr;           // The Coro<T> type itself

    IRFunction() : return_type(nullptr), next_value_id(0) {}

    ValueId new_value() {
        ValueId v{next_value_id++};
        values_by_id.push_back(nullptr);
        return v;
    }

    IRInst* inst_for(ValueId v) const {
        if (!v.is_valid() || v.id >= values_by_id.size()) return nullptr;
        return values_by_id[v.id];
    }

    // Check if this function returns a large struct (>4 slots, >16 bytes)
    // Large structs are returned via hidden output pointer
    bool returns_large_struct() const {
        return return_type && return_type->is_struct() &&
               return_type->struct_info.slot_count > 4;
    }

    /// Reorder blocks to reverse postorder and remap all BlockId references.
    /// Entry block is always first in RPO. Preserves BlockId.id == array index.
    void reorder_blocks_rpo();
};

// A module-level global variable: persistent storage in the VM's global slot
// array, initialized by `__module_init` before `main` and (for noncopyable
// types) torn down by `__module_shutdown` at VM destroy.
struct IRGlobal {
    StringView name;
    Type* type;
    u32 slot_offset;     // Offset into the module's global slot array
    u32 slot_count;      // Slots this global occupies
    Expr* initializer;   // Initializer expression (may be null)
};

// IR Module - collection of functions
struct IRModule {
    Vector<IRFunction*> functions;
    StringView name;

    // Module-level globals + total slot count for the VM's global array.
    Vector<IRGlobal> globals;
    u32 global_slot_count = 0;

    // Type lists for C backend code generation
    Vector<Type*> struct_types;  // All struct types (incl. monomorphized generics)
    Vector<Type*> enum_types;    // All enum types

    // Cleanup wrapper functions for noncopyable List/Map types in coroutines.
    // Key: interned Type* (List or Map), Value: wrapper function name.
    // Generated by IRBuilder, consumed by coroutine lowering's generate_coro_destructor.
    tsl::robin_map<Type*, StringView> cleanup_wrappers;
};

// String representations for debugging
const char* ir_op_to_string(IROp op);
void ir_inst_to_string(const IRInst* inst, String& out);
void ir_block_to_string(const IRBlock* block, String& out);
void ir_function_to_string(const IRFunction* func, String& out);
void ir_module_to_string(const IRModule* module, String& out);

}
