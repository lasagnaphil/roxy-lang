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

> **Limitations** (both backends): multiple modules each declaring globals share the synthesized
> `__module_init` / `__module_shutdown` names, so only one module's globals are
> initialized/torn down (slot offsets are re-based at link time, so there is no
> memory corruption — just incomplete init); and `pub` global *sharing* across
> modules is not implemented.

## Storage

Globals live in a dedicated per-VM slot array (`RoxyVM::global_slots`, `u32`
slots — the same 4-byte granularity as the local stack), sized to the module's
`global_slot_count` and zero-initialized at load. Each global is assigned a
cumulative `slot_offset` in declaration order (`IRBuilder::collect_globals`,
run first so every function body can resolve a global reference). Multi-slot
types consume contiguous slots.

A global is, in effect, a pointer to persistent storage — the same shape as an
`inout` parameter — so global access reuses the existing pointer machinery.

## Access

`IROp::GlobalAddr` (→ `GLOBAL_ADDR`, mirroring `STACK_ADDR`) yields
`&global_slots[slot_offset]`.

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
  structs, `StorePtr` otherwise). The global then owns what it holds: an
  initializer temp with drop glue is adopted (so its own cleanup does not release
  it when `__module_init` returns), and a `string` that is not a fresh temp —
  another global — is retained. Because init runs in order, a later global's
  initializer may read an earlier global.
- **`__module_shutdown`** — for each global with drop glue (`member_needs_drop`),
  in **reverse** order: destroy it (`Delete` through the slot's address for value
  structs, including copyable ones holding a `string`; `LoadPtr` + `Delete` for
  `uniq`/`List`/`Map`; `StrRelease` for a `string`; `RefDec` for a `ref`).

Assigning to a `string` global stores the new value, adopts or retains it, and
then releases the overwritten one — retain before release, so `g = g` is safe.

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
  counting), so a reentrant free traps. A **`ref` global** is a counted borrow
  held for the whole VM lifetime: `build_module_init` RefIncs the pointee after
  storing the initializer and `build_module_shutdown` RefDecs it (reverse order,
  so before the owner's `Delete`), so deleting the borrowed owner traps while the
  global still borrows it. A **`weak` global** stays uncounted (generational) and
  never blocks a delete, but its deref is still `WeakCheck`-guarded. An explicit
  `delete` of a `uniq` global nulls its slot (`gen_delete_stmt`) so the
  null-guarded shutdown `Delete` doesn't double-free it. All of this lives in the
  shared init/shutdown IR, so both backends inherit it.
- **C backend (AOT):** each global becomes a zero-initialized
  `static <CType> g_<name>;` and `GlobalAddr` lowers to `&g_<name>`. The generated
  `main()` calls `__module_init()` after the ctx is active and `__module_shutdown()`
  before teardown, mirroring the VM's load/destroy bracketing.
- **Linking:** `Compiler::link_modules` merges each module's globals into the
  merged module, re-basing slot offsets (and the matching `GlobalAddr` ops) so
  module ranges don't overlap; `global_slot_count` is the sum. (Single-module is
  exact; multi-module init is the limitation noted in the status block.)

## Files

- `compiler/ir/ir_builder.cpp` — `collect_globals`, `gen_global_read`, `build_module_init` / `build_module_shutdown`
- `compiler/driver/compiler.cpp` — `link_modules` global merging and offset re-basing
- `vm/vm.cpp` — `global_slots` allocation, init at load, shutdown at destroy
- `compiler/codegen/c_emitter.cpp` — `emit_global_definitions`, `main()` init/shutdown
- `tests/e2e/test_globals.cpp`
