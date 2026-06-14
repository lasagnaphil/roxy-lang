# Module-Level Globals

A top-level `var` declaration is a module-level global: it has persistent storage
for the VM's lifetime, is initialized once (constructors included) before any user
call, and — when noncopyable — is destroyed at VM teardown (RAII).

```roxy
var counter: i32 = 0;
var origin: Point = Point { x = 3, y = 4 };
var registry: uniq Table = uniq Table();   // ctor runs at init, dtor at shutdown

fun bump() { counter = counter + 1; }       // read/write from any function
```

> **Status:** Implemented for the VM. The C backend is **not** wired yet —
> emitting a global to C raises a `#error` rather than silently producing wrong
> code. Cross-module / `pub` global *sharing* is not implemented (each module
> initializes its own globals).

## Why this is a feature, not a field

Before this, the parser accepted top-level `var` and semantic analysis resolved
the symbol, but **nothing downstream emitted it** — no storage, no initializer,
no access path — so any program with a global failed to compile. A `uniq` global
therefore appeared to "skip its constructor" when really globals didn't exist at
all. This adds the missing storage, initialization, access, and teardown.

## Storage

Globals live in a dedicated per-VM slot array (`RoxyVM::global_slots`, `u32`
slots — the same 4-byte granularity as the local stack), sized to the module's
`global_slot_count` and zero-initialized at load. Each global is assigned a
cumulative `slot_offset` in declaration order (`IRBuilder::collect_globals`,
run first so every function body can resolve a global reference). Multi-slot
types (`i64`/`f64` = 2, structs = N, `uniq`/`ref` = 2, `weak` = 4) consume
contiguous slots. The count flows IR → bytecode as `IRModule::global_slot_count`
→ `BCModule::global_slot_count`.

A global is, in effect, a pointer to persistent storage — the same shape as an
`inout` parameter — so global access reuses the existing pointer machinery.

## Access

One new IR op and one new opcode:

- `IROp::GlobalAddr` (payload: `slot_offset`) → `GLOBAL_ADDR dst, imm16` (ABI
  format, mirrors `STACK_ADDR`): `regs[dst] = &global_slots[slot_offset]`.

`gen_identifier_expr` routes a name that is **not a local** but **is a global** to
`gen_global_read` (a local of the same name shadows the global): `GlobalAddr`
then — for a struct global — use the address directly (field ops want a pointer),
otherwise `LoadPtr` the value out of the slot. `gen_assign_local` routes a global
write through `GlobalAddr` + store, with the **same old-value destroy + RHS-temp
consume** as `uniq`-field / `inout` assignment (so reassigning a `uniq` global
frees the overwritten object — no leak, no double-free).

## Initialization and teardown

The IR builder synthesizes two parameterless functions:

- **`__module_init`** — for each global with an initializer, in declaration
  order: evaluate the initializer (which runs `New` + the constructor for
  `uniq T(..)`), then store it into the global's slot (struct-copy for value
  structs, `StorePtr` otherwise), consuming the initializer temp. Because init
  runs in order, a later global's initializer may read an earlier global.
- **`__module_shutdown`** — for each noncopyable global, in **reverse** order:
  destroy it (`Delete` through the slot's address for value structs, or
  `LoadPtr` + `Delete` for `uniq`/`List`/`Map`).

Both are skipped (not generated) when no global needs them.

The VM drives them around the module lifecycle:

- `vm_load_module` allocates `global_slots` (zero-init) and calls `__module_init`
  once, before any user call — so globals are live by the time `main` runs.
- `vm_destroy` calls `__module_shutdown` **first** — while the heap/allocator and
  execution machinery are still intact — then frees `global_slots`.

## Interaction with other features

- **Constraint references (lifetimes.md):** a `uniq` global is a heap owner like
  any other; its destructor runs once at shutdown. Calling a method on a `uniq`
  global counts the receiver for the call's duration (call-site receiver
  counting), so a reentrant free traps.
- **C backend:** deferred. `CEmitter` emits a `#error` for `GlobalAddr` so an AOT
  build of code using globals fails loudly. (A future pass would emit C globals
  plus init/teardown in the generated `main`.)

## Files

| File | Change |
|------|--------|
| `include/roxy/compiler/ssa_ir.hpp` | `IROp::GlobalAddr`, `GlobalData`, `IRGlobal`, `IRModule::globals` / `global_slot_count` |
| `src/roxy/compiler/ir_builder.{hpp,cpp}` | `collect_globals`; `emit_global_addr` / `gen_global_read`; global read in `gen_identifier_expr`, global write in `gen_assign_local`; `build_module_init` / `build_module_shutdown`; `m_global_indices` |
| `src/roxy/compiler/lowering.cpp` | `GlobalAddr` → `GLOBAL_ADDR`; `BCModule::global_slot_count`; liveness no-operand classification |
| `src/roxy/compiler/ssa_ir.cpp`, `ir_validator.cpp`, `ir_optimize.hpp` | `GlobalAddr` in printers / validator / `for_each_operand` |
| `include/roxy/vm/bytecode.hpp` | `GLOBAL_ADDR` (0xBE), `BCModule::global_slot_count` |
| `src/roxy/vm/interpreter.cpp` | `GLOBAL_ADDR` handler + dispatch entry |
| `include/roxy/vm/vm.hpp`, `src/roxy/vm/vm.cpp` | `RoxyVM::global_slots`; alloc + `__module_init` at load; `__module_shutdown` + free at destroy |
| `src/roxy/compiler/c_emitter.cpp` | `#error` for `GlobalAddr` (C backend deferred) |
| `tests/e2e/test_globals.cpp` | E2E tests |
