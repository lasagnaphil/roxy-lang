# Virtual Machine

The Roxy VM is a register-based virtual machine. It executes bytecode against a **shared register file** (windowed per call) plus a **separate 4-byte-slot local stack** for struct data, with a pre-allocated **call-frame stack** tracking active calls. State, `CallFrame`, `VMConfig` and the public API are in `include/roxy/vm/vm.hpp`.

## VM State

Points not obvious from the struct:

- **`roxy_ctx ctx` is embedded first**, so a `RoxyVM*` can be reinterpreted as a `roxy_ctx*`. The interpreter points the thread-local context at it on every public entry; natives and runtime helpers reach it via `roxy_get_ctx()` (see [c-backend.md](c-backend.md)). The `SlabAllocator` is plugged into `ctx` through a `roxy_allocator` vtable.
- **Global slots** hold module-level globals; see [globals.md](globals.md).
- **Dispatch side-tables**: `map_dispatch` (per-map `Hash`/`Eq` bytecode indices for `Map<Struct, V>`) and `closure_env_dtors` (env `type_id` → destructor index).

## Value Representation

**At runtime, VM registers are untyped `u64` slots.** `Value` (`include/roxy/vm/value.hpp`) is a tagged union used only at the **public API and native-function boundary**; it is not the register format.

## Interpreter Loop

`interpret()` uses **computed-goto (threaded) dispatch** under GCC/Clang: each handler ends in `DISPATCH()`, jumping through a 256-entry label table so each opcode has its own indirect-branch site. A `switch`-based fallback is kept for MSVC. The hot frame state (`pc`, `registers`, `func`) is cached in locals and refreshed on call/return. See [vm-optimization.md](vm-optimization.md).

A call bumps `register_top` for the callee's register window and `local_stack_top` for its local-stack frame; return pops both. When the call stack empties, the result lands in register 0 and the loop stops.

Runtime errors: division by zero sets `vm->error` and halts. An out-of-bounds `list[i]` **read** and a missing-key `m[k]` read instead throw catchable `IndexError` / `KeyError` — the compiler emits the bounds check (list) or a null-slot branch on `INDEX_TRYADDR_MAP` (map) in IR, so no opcode traps on that path. The remaining index opcodes (`.get()`, element-lvalue borrows) still set `vm->error`. See [exceptions.md](exceptions.md).
