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
    // Constants
    ConstNull, ConstBool, ConstInt, ConstFloat, ConstString,
    
    // Arithmetic (integer)
    AddI, SubI, MulI, DivI, ModI, NegI,
    
    // Arithmetic (float)
    AddF, SubF, MulF, DivF, NegF,
    
    // Comparisons
    EqI, NeI, LtI, LeI, GtI, GeI,
    EqF, NeF, LtF, LeF, GtF, GeF,
    
    // Logical
    Not, And, Or,
    
    // Conversions
    I2F, F2I,
    
    // Memory
    StackAlloc, GetField, SetField,
    GetIndex, SetIndex,
    
    // Functions
    Call, CallNative,
    
    // Block arguments
    BlockArg,
};

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

    // Constant pool management
    u16 add_constant(const BCConstant& c);

    // Block lowering
    void lower_block(IRBlock* block);
    void lower_instruction(IRInst* inst);
    void lower_terminator(IRBlock* block);

    // Block argument handling
    void emit_block_args(const JumpTarget& target);

    // Two-pass jump resolution
    void patch_jumps();
};
```

Key lowering decisions:
- **Simple register allocation**: Uses SSA value ID as register number (max 255 values per function)
- **Two-pass emission**: First pass records block offsets, second pass patches jump targets
- **Constant pool**: Values that don't fit in 16-bit immediate go to constant pool
- **Block arguments**: Lowered to MOV instructions before each jump

## Files

- `include/roxy/compiler/ssa_ir.hpp` - IR data structures
- `src/roxy/compiler/ssa_ir.cpp` - IR utilities and printing
- `include/roxy/compiler/ir_builder.hpp` - AST to IR conversion
- `src/roxy/compiler/ir_builder.cpp` - IR builder implementation
- `include/roxy/compiler/lowering.hpp` - IR to bytecode conversion
- `src/roxy/compiler/lowering.cpp` - Lowering implementation
