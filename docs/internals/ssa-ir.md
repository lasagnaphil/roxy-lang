# SSA IR

Roxy uses SSA (Static Single Assignment) IR with block arguments instead of phi nodes.

## Block Arguments vs Phi Nodes

Traditional SSA uses phi nodes at block entry:
```
loop:
    sum = phi [0, entry], [sum2, body]
    i = phi [1, entry], [i2, body]
```

Roxy uses block arguments instead:
```
entry:
    goto loop(0, 1)              // initial values

loop(sum, i):                    // block parameters
    t0 = i <= n
    if t0 goto body else exit(sum)

body:
    sum2 = sum + i
    i2 = i + 1
    goto loop(sum2, i2)          // pass values to successor

exit(result):
    return result
```

**Rationale:** Cleaner dataflow representation, easier lowering to bytecode.

## IR Structure

```cpp
struct ValueId { u32 id; };
struct BlockId { u32 id; };

enum class IROp {
    // Constants (6)
    ConstNull, ConstBool, ConstInt, ConstF, ConstD, ConstString,

    // Arithmetic - integer (6)
    AddI, SubI, MulI, DivI, ModI, NegI,

    // Arithmetic - f32 (5)
    AddF, SubF, MulF, DivF, NegF,

    // Arithmetic - f64 (5)
    AddD, SubD, MulD, DivD, NegD,

    // Comparisons - integer (6)
    EqI, NeI, LtI, LeI, GtI, GeI,

    // Comparisons - f32 (6)
    EqF, NeF, LtF, LeF, GtF, GeF,

    // Comparisons - f64 (6)
    EqD, NeD, LtD, LeD, GtD, GeD,

    // Logical (3)
    Not, And, Or,

    // Bitwise (3)
    BitAnd, BitOr, BitNot,

    // Conversions (4)
    I2F, F2I, I2B, B2I,

    // Memory (4)
    StackAlloc, GetField, GetFieldAddr, SetField,

    // Reference counting (3)
    RefInc, RefDec, WeakCheck,

    // Object lifecycle (2)
    New, Delete,

    // Functions (2)
    Call, CallNative,

    // Structs (1)
    StructCopy,

    // Pointers (3)
    LoadPtr, StorePtr, VarAddr,

    // Meta (2)
    BlockArg, Copy,

    // Casting (1)
    Cast,
};
// Total: 54 IR operations

struct IRInst {
    IROp op;
    ValueId result;
    Type* type;
    
    union {
        ConstData const_data;
        BinaryData binary;
        UnaryData unary;
        CallData call;
        FieldData field;
        IndexData index;
        StackAllocData stack_alloc;
        // ...
    };
};

struct IRBlock {
    BlockId id;
    Vector<BlockParam> params;      // Block arguments
    Vector<IRInst*> instructions;
    Terminator terminator;
};

struct IRFunction {
    StringView name;
    Vector<BlockParam> params;      // Function parameters
    Type* return_type;
    Vector<IRBlock*> blocks;
    u32 next_value_id;
};
```

## Terminators

Each block ends with a terminator:

```cpp
enum class TerminatorKind {
    None,       // Block not yet terminated
    Goto,       // Unconditional jump
    Branch,     // Conditional jump
    Return,     // Function return
};

struct Terminator {
    TerminatorKind kind;
    
    // For Goto
    BlockId target;
    Vector<BlockArgPair> args;
    
    // For Branch
    ValueId condition;
    BlockId then_block;
    BlockId else_block;
    Vector<BlockArgPair> then_args;
    Vector<BlockArgPair> else_args;
    
    // For Return
    ValueId return_value;
};
```

## Lowering to Bytecode

Block arguments become MOV instructions at jump sites:

```
entry:
    LOAD_INT  R1, 0              // sum = 0
    LOAD_INT  R2, 1              // i = 1

loop:
    LE_I      R3, R2, R0         // t0 = i <= n
    JMP_IF_NOT R3, exit

body:
    ADD_I     R1, R1, R2         // sum = sum + i
    ADD_I     R2, R2, 1          // i = i + 1
    JMP       loop

exit:
    MOV       R0, R1             // result = sum
    RET
```

## BytecodeBuilder

```cpp
class BytecodeBuilder {
public:
    BCModule* build(IRModule* ir_module);
    BCFunction* build_function(IRFunction* ir_func);

private:
    // Register allocation: ValueId -> register number
    u8 allocate_register(ValueId value);
    u8 get_register(ValueId value);

    // Register spilling (when >255 simultaneously-live values)
    void spill_furthest();
    u8 get_result_register(ValueId value);
    u8 ensure_in_register(ValueId value, u8 scratch_index);
    void spill_if_needed(ValueId value, u8 reg);

    // Liveness analysis
    void compute_liveness(IRFunction* ir_func);
    void expire_before(u32 current_point);

    // Constant pool management
    u16 add_constant(const BCConstant& c);

    // Block lowering
    void lower_instruction(IRInst* inst);
    void lower_terminator(IRBlock* block);

    // Block argument handling
    void emit_block_args(const JumpTarget& target);

    // Two-pass jump resolution
    void patch_jumps();
};
```

Key lowering decisions:
- **Two-pass emission**: First pass records block offsets, second pass patches jump targets
- **Constant pool**: Values that don't fit in 16-bit immediate go to constant pool
- **Block arguments**: Lowered to MOV instructions before each jump

### Register Allocation

The register file uses 8-bit indices (0–254, with 0xFF as sentinel), giving a hard cap of 255 registers per function. The allocator uses liveness-based allocation with register reuse:

1. **Liveness analysis** (`compute_liveness`): Computes def/last-use intervals for all SSA values over a linear program-point numbering. Five passes handle definition points, operand uses, block-param extensions for parallel assignment safety, loop back-edge extensions (fixed-point iteration for nested loops), and same-block classification.

2. **Free-list allocation** (`allocate_register`): Values whose def and last-use are within the same block can reuse freed registers from a free list. Cross-block values (including block params) always get fresh registers to preserve zero-initialization semantics for partially-defined values (e.g., AND/OR short-circuit patterns).

3. **Active set expiry** (`expire_before`): Before each allocation point, values whose last-use has passed are expired and their registers returned to the free list, sorted by last-use ascending.

4. **Pre-colored parameters**: Function parameters are assigned to registers R0, R1, ... before the main allocation loop. Call results use bump allocation to guarantee contiguous register blocks for the calling convention.

### Register Spilling

When register pressure exceeds 255, spilling evicts long-lived values to the local stack using a furthest-first eviction strategy:

1. **Trigger**: When `allocate_register()` finds both the bump pointer at the limit and the free list empty, it calls `spill_furthest()`.

2. **Scratch register setup** (first spill only): Two dedicated scratch registers are permanently reserved by evicting the two values with the furthest `last_use_point` from the active set. These scratch registers handle all subsequent reload/spill operations during emission.

3. **Furthest-first eviction**: Each spill evicts the active value with the latest `last_use_point`, freeing its register for the caller.

4. **Emission**: During bytecode emission, spill-aware methods replace the normal register lookup:
   - `get_result_register(value)` — returns the value's register, or scratch[0] for spilled destinations
   - `ensure_in_register(value, scratch_index)` — returns the value's register, or emits `RELOAD_REG` into the specified scratch register for spilled operands
   - `spill_if_needed(value, reg)` — emits `SPILL_REG` after writing a spilled value's result

**Zero overhead**: Functions that don't trigger spilling never reserve scratch registers and never emit `SPILL_REG`/`RELOAD_REG` instructions.

## Files

- `include/roxy/compiler/ssa_ir.hpp` - IR data structures
- `src/roxy/compiler/ssa_ir.cpp` - IR utilities and printing
- `include/roxy/compiler/ir_builder.hpp` - AST to IR conversion
- `src/roxy/compiler/ir_builder.cpp` - IR builder implementation
- `include/roxy/compiler/lowering.hpp` - IR to bytecode conversion
- `src/roxy/compiler/lowering.cpp` - Lowering implementation
