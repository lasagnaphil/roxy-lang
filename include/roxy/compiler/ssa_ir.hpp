#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/span.hpp"
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
    ConstFloat,     // float constant
    ConstString,    // string constant

    // Arithmetic (integer)
    AddI,
    SubI,
    MulI,
    DivI,
    ModI,
    NegI,

    // Arithmetic (float)
    AddF,
    SubF,
    MulF,
    DivF,
    NegF,

    // Comparison (integer)
    EqI,
    NeI,
    LtI,
    LeI,
    GtI,
    GeI,

    // Comparison (float)
    EqF,
    NeF,
    LtF,
    LeF,
    GtF,
    GeF,

    // Logical
    Not,
    And,
    Or,

    // Bitwise
    BitAnd,
    BitOr,
    BitNot,

    // Type conversions
    I2F,            // int to float
    F2I,            // float to int
    I2B,            // int to bool
    B2I,            // bool to int

    // Memory operations
    StackAlloc,     // allocate slots on local stack, returns pointer
    GetField,       // obj.field (load value)
    GetFieldAddr,   // &obj.field (compute address, for nested struct access)
    SetField,       // obj.field = value
    GetIndex,       // arr[i]
    SetIndex,       // arr[i] = value

    // Reference operations
    RefInc,         // increment ref count
    RefDec,         // decrement ref count
    WeakCheck,      // check if weak ref is valid

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
};

// Constant data for ConstXxx instructions
struct ConstData {
    union {
        bool bool_val;
        i64 int_val;
        f64 float_val;
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

// Index access data
struct IndexData {
    ValueId object;
    ValueId index;
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
        IndexData index;                // For GetIndex/SetIndex
        NewData new_data;               // For New
        StackAllocData stack_alloc;     // For StackAlloc
        StructCopyData struct_copy;     // For StructCopy
        LoadPtrData load_ptr;           // For LoadPtr
        StorePtrData store_ptr;         // For StorePtr
        VarAddrData var_addr;           // For VarAddr
        u32 block_arg_index;            // For BlockArg (parameter index)
    };

    // For SetField/SetIndex, the value being stored
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

// IR Function
struct IRFunction {
    StringView name;
    Type* return_type;
    Vector<BlockParam> params;          // Function parameters (become entry block args)
    Vector<bool> param_is_ptr;          // True if param is a pointer (out/inout parameter)
    Vector<IRBlock*> blocks;            // Basic blocks, blocks[0] is entry
    u32 next_value_id;                  // Counter for generating unique value IDs

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
};

// IR Module - collection of functions
struct IRModule {
    Vector<IRFunction*> functions;
    StringView name;
};

// String representations for debugging
const char* ir_op_to_string(IROp op);
void ir_inst_to_string(const IRInst* inst, Vector<char>& out);
void ir_block_to_string(const IRBlock* block, Vector<char>& out);
void ir_function_to_string(const IRFunction* func, Vector<char>& out);
void ir_module_to_string(const IRModule* module, Vector<char>& out);

}
