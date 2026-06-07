# Virtual Machine

The Roxy VM is a register-based virtual machine. It executes bytecode against a **shared register file** (windowed per call) plus a **separate 4-byte local stack** for struct data, with a **call-frame stack** tracking active calls.

## VM State

`RoxyVM` holds the loaded `BCModule`, the shared register file (`u64* register_file` with `register_top` marking the current allocation top), the local stack (`u32* local_stack` of 4-byte slots with `local_stack_top`), the `Vector<CallFrame> call_stack`, a `running` flag, and an `error` string.

Each `CallFrame` records its `BCFunction*`, the saved program counter (`pc`), a `registers` window into `register_file`, the caller's `return_reg`, and `local_stack_base` (its base slot in the local stack). See `include/roxy/vm/vm.hpp` for the full structs.

## VM API

```cpp
bool  vm_init(RoxyVM* vm, const VMConfig& config = VMConfig());
void  vm_destroy(RoxyVM* vm);
bool  vm_load_module(RoxyVM* vm, BCModule* module);
bool  vm_call(RoxyVM* vm, StringView func_name, Span<Value> args);
Value vm_get_result(RoxyVM* vm);
```

## Value Representation

`Value` is a tagged union used for the **public API and native function interface** — not the runtime register format. The tag is one of `Null, Bool, Int, Float, Ptr, Weak`, and the union carries the corresponding payload (a `Weak` value also stores a `u32 generation`).

```cpp
struct Value {
    enum Type : u8 { Null, Bool, Int, Float, Ptr, Weak };
    Type type;
    union {
        bool  as_bool;
        i64   as_int;
        f64   as_float;
        void* as_ptr;
        struct { void* ptr; u32 generation; } as_weak;
    };
    // make_null/make_bool/make_int/make_float/make_ptr/make_weak factories
    // is_null / is_truthy / is_weak_valid
    // as_u64 / from_u64 / float_from_u64  — untyped register conversion
};
```

**At runtime, VM registers are untyped `u64` slots.** The `Value` struct exists only at the public/native boundary; type information is otherwise preserved only for debugging. See `include/roxy/vm/value.hpp`.

## Interpreter Loop

`interpret()` is a switch-based dispatch loop (chosen for portability — works with MSVC/clang-cl on Windows). It decodes each instruction's opcode and operands, then dispatches on the opcode. The hot frame state (`pc`, `registers`, `func`) is cached in locals and refreshed on call/return.

Call and return manage both stacks:

- **`CALL`** saves the caller's PC, allocates the callee's register window by bumping `register_top`, allocates an aligned local-stack frame (rounded up to 4 slots) by bumping `local_stack_top`, copies arguments into the callee window, and pushes a new `CallFrame`.
- **`RET`** writes the result into the caller's `return_reg`, pops the local-stack frame back to `local_stack_base`, pops the `CallFrame`, and lowers `register_top`. When the call stack empties, the result lands in register 0 and the loop stops.

Other notes:

- Division by zero and array bounds violations are checked; on failure the handler sets `vm->error` and reports an error.
- `SPILL_REG` / `RELOAD_REG` move register values to/from the local stack for functions that exceed the 255-register limit (see `docs/internals/bytecode.md`).

## Files

| File | Purpose |
|---|---|
| `include/roxy/vm/vm.hpp` | VM state and API declarations |
| `src/roxy/vm/vm.cpp` | VM initialization and execution |
| `include/roxy/vm/value.hpp` | Value representation |
| `src/roxy/vm/value.cpp` | Value operations |
| `include/roxy/vm/interpreter.hpp` | Interpreter declarations |
| `src/roxy/vm/interpreter.cpp` | Interpreter loop implementation |
