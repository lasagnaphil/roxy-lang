#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/span.hpp"
#include "roxy/core/string.hpp"
#include "roxy/core/string_view.hpp"
#include "roxy/core/vector.hpp"
#include "roxy/core/bump_allocator.hpp"
#include "roxy/compiler/types.hpp"

namespace rx {

// Forward declarations
struct IRValue;
struct IRInst;
struct IRBlock;
struct IRFunction;
struct IRModule;

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
    GetField,       // obj.field (load value)
    GetFieldAddr,   // &obj.field (compute address, for nested struct access)
    SetField,       // obj.field = value
    // Reference operations
    RefInc,         // increment ref count
    RefDec,         // decrement ref count
    WeakCheck,      // check if weak ref is valid
    WeakCreate,     // create weak ref from pointer (extracts generation)

    // Object lifecycle
    New,            // allocate new object
    Delete,         // deallocate object

    // Function call
    Call,           // call function
    CallNative,     // call native function
    CallExternal,   // call function in another module

    // Block argument (phi-like)
    BlockArg,       // Block parameter - receives value from predecessor

    // Copy (for SSA value passing)
    Copy,           // Simple copy of a value

    // Struct operations
    StructCopy,     // Memory-to-memory struct copy

    // Pointer operations (for out/inout parameters)
    LoadPtr,        // Load value through pointer: dst = *ptr
    StorePtr,       // Store value through pointer: *ptr = val
    VarAddr,        // Get address of local variable

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
    u8 native_index;  // For CallNative: index into module's native_functions
};

// External call instruction data (for cross-module calls)
struct CallExternalData {
    StringView module_name;
    StringView func_name;
    Span<ValueId> args;
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

// Variable address data
struct VarAddrData {
    StringView name;  // Name of the local variable
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
        FieldData field;                // For GetField/SetField
        NewData new_data;               // For New
        StackAllocData stack_alloc;     // For StackAlloc
        StructCopyData struct_copy;     // For StructCopy
        LoadPtrData load_ptr;           // For LoadPtr
        StorePtrData store_ptr;         // For StorePtr
        VarAddrData var_addr;           // For VarAddr
        CastData cast;                  // For Cast
        u32 block_arg_index;            // For BlockArg (parameter index)
    };

    // For SetField, the value being stored
    ValueId store_value;

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
};

// Finally handler metadata for IR
struct IRFinallyInfo {
    BlockId try_entry;
    BlockId try_exit;
    BlockId finally_block;
    BlockId finally_end_block;  // Block after finally (for normal flow)
};

// Cleanup info for owned locals (used to generate bytecode cleanup records)
struct IRCleanupInfo {
    ValueId value;        // The SSA value to clean up
    Type* type;           // Determines cleanup kind (uniq, struct w/ dtor, list, map)
    BlockId start_block;  // First block where variable is live
    BlockId end_block;    // Last block where variable is live (scope exit)
};

// IR Function
struct IRFunction {
    StringView name;
    Type* return_type;
    Vector<BlockParam> params;          // Function parameters (become entry block args)
    Vector<bool> param_is_ptr;          // True if param is a pointer (out/inout parameter)
    Vector<IRBlock*> blocks;            // Basic blocks, blocks[0] is entry
    u32 next_value_id;                  // Counter for generating unique value IDs

    // Exception handling metadata
    Vector<IRExceptionHandler> exception_handlers;
    Vector<IRFinallyInfo> finally_handlers;

    // Cleanup records for owned locals (for exception-path cleanup)
    Vector<IRCleanupInfo> cleanup_info;

    // Coroutine metadata (set by IR builder for functions returning Coro<T>)
    bool is_coroutine = false;
    Type* coro_yield_type = nullptr;     // T in Coro<T>
    Type* coro_struct_type = nullptr;    // The synthetic struct holding coroutine state
    Type* coro_type = nullptr;           // The Coro<T> type itself

    IRFunction() : return_type(nullptr), next_value_id(0) {}

    ValueId new_value() {
        return ValueId{next_value_id++};
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

// IR Module - collection of functions
struct IRModule {
    Vector<IRFunction*> functions;
    StringView name;
};

// String representations for debugging
const char* ir_op_to_string(IROp op);
void ir_inst_to_string(const IRInst* inst, String& out);
void ir_block_to_string(const IRBlock* block, String& out);
void ir_function_to_string(const IRFunction* func, String& out);
void ir_module_to_string(const IRModule* module, String& out);

}
