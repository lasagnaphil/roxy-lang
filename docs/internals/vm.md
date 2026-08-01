# Virtual Machine

The Roxy VM is a register-based virtual machine. It executes bytecode against a **shared register file** (windowed per call) plus a **separate 4-byte local stack** for struct data, with a **call-frame stack** tracking active calls.

## VM State

`RoxyVM` holds:

- **`roxy_ctx ctx`** — embedded **first**, so a `RoxyVM*` can be reinterpreted as a `roxy_ctx*`. The interpreter points the thread-local context at it on every public entry; natives and runtime helpers reach it via `roxy_get_ctx()` (see [c-backend.md](c-backend.md)).
- **Register file** — `u64* register_file` with `register_top` marking the current allocation top.
- **Local stack** — `u32* local_stack` of 4-byte slots with `local_stack_top`, for struct data.
- **Global slots** — `u32* global_slots`, module-level global storage; see [globals.md](globals.md).
- **Call stack** — a pre-allocated `CallFrame[]` with `call_stack_size` / `call_stack_capacity` (no `Vector` push/capacity check on the call path).
- **Heap** — `SlabAllocator` (plugged into `ctx` through a `roxy_allocator` vtable) and the `StringInternTable`.
- **Dispatch side-tables** — `map_dispatch` (per-map `Hash`/`Eq` bytecode indices for `Map<Struct, V>`) and `closure_env_dtors` (env `type_id` → destructor index).
- **Exception state** — the in-flight exception pointer, its `type_id`, and its `message()` function index.
- **`running` flag and `error` string.**

Each `CallFrame` records its `BCFunction*`, the saved program counter (`pc`), a `registers` window into `register_file`, the caller's `return_reg`, and `local_stack_base` (its base slot in the local stack). See `include/roxy/vm/vm.hpp` for the full structs.

## VM API

```cpp
bool  vm_init(RoxyVM* vm, const VMConfig& config = VMConfig());
void  vm_destroy(RoxyVM* vm);
bool  vm_load_module(RoxyVM* vm, BCModule* module);
bool  vm_call(RoxyVM* vm, StringView func_name, Span<Value> args);
bool  vm_call_index(RoxyVM* vm, u32 func_index, Span<Value> args);
Value vm_get_result(RoxyVM* vm);
const char* vm_get_error(RoxyVM* vm);
void  vm_clear_error(RoxyVM* vm);
void  vm_register_native(RoxyVM* vm, StringView name, NativeFunction func, u32 param_count);
```

`VMConfig` sets `register_file_size` (default 65536 slots), `local_stack_size` (262144 4-byte slots = 1 MB), and `max_call_depth` (1024).

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

`interpret()` uses **computed-goto (threaded) dispatch** under GCC/Clang: each handler ends in `DISPATCH()`, which fetches the next instruction and jumps through a 256-entry `dispatch_table` of label addresses, giving the branch predictor a distinct indirect-branch site per opcode. A `switch`-based fallback is kept behind a `#if defined(__GNUC__) || defined(__clang__)` guard for MSVC. The hot frame state (`pc`, `registers`, `func`) is cached in locals and refreshed on call/return. See [vm-optimization.md](vm-optimization.md) for the dispatch, call/return, and fused-branch work.

Call and return manage both stacks:

- **`CALL`** saves the caller's PC, allocates the callee's register window by bumping `register_top`, allocates an aligned local-stack frame (rounded up to 4 slots) by bumping `local_stack_top`, copies arguments into the callee window, and pushes a new `CallFrame`.
- **`RET`** writes the result into the caller's `return_reg`, pops the local-stack frame back to `local_stack_base`, pops the `CallFrame`, and lowers `register_top`. When the call stack empties, the result lands in register 0 and the loop stops.

Other notes:

- Division by zero sets `vm->error` and halts. An out-of-bounds `list[i]` **read** and a missing-key `m[k]` read instead throw catchable `IndexError` / `KeyError` exceptions — the compiler emits the bounds check (list) or a null-slot branch on `INDEX_TRYADDR_MAP` (map) in IR, so the opcode itself never traps on that path. The remaining index opcodes (`.get()`, element-lvalue borrows) still set `vm->error`. See [exceptions.md](exceptions.md).
- `SPILL_REG` / `RELOAD_REG` move register values to/from the local stack for functions that exceed the 255-register limit (see [bytecode.md](bytecode.md)).

## Files

| File | Purpose |
|---|---|
| `include/roxy/vm/vm.hpp` | VM state and API declarations |
| `src/roxy/vm/vm.cpp` | VM initialization and execution |
| `include/roxy/vm/value.hpp` | Value representation |
| `src/roxy/vm/value.cpp` | Value operations |
| `include/roxy/vm/interpreter.hpp` | Interpreter declarations |
| `src/roxy/vm/interpreter.cpp` | Interpreter loop implementation |
