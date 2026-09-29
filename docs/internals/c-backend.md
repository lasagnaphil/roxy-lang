# C Backend (AOT Compilation)

> **Status:** every language feature has a codegen path, and function- and statement-level `#line` directives are emitted. Further codegen-quality work (DCE, Relooper, `switch` lowering, readable variable names) is deliberately not pursued — the C compiler's optimizer covers it and it doesn't affect debugger UX.
>
> **Correctness is *not* complete.** Four narrow gaps remain, each pinned by a VM-only test case. **Known C-backend gaps** under Testing is the authoritative list — it is kept in sync with the `// VM-only: C backend:` annotations in `tests/e2e/`.

The C backend (`CEmitter`) translates Roxy's SSA IR into a `.cpp` file that any C++ compiler can build. The body is C-style (structs, gotos, typed `vN` locals); native bindings and the public header use C++ to interface directly with the embedder.

## Pipeline

```
Source → ... → SSA IR
                 ├→ Lowering → Bytecode → VM          (interpreter)
                 └→ CEmitter → .cpp file → g++/clang++ (AOT)
```

SSA IR is the chosen translation source: every op is typed, structs are laid out, generics are monomorphized, and operators are desugared into method calls, so the mapping to C is mechanical. (AST would force re-implementing all of that; bytecode loses types and produces unreadable register-shuffling C.)

Note that the C path branches off the IR *before* bytecode lowering, so anything `lowering.cpp` inserts (e.g. the callee-side deep copy of a copyable container value param) the emitter must emit itself.

## Type Mapping

Primitives map to `<stdint.h>` / `float` / `double` / `bool`; `string`, `List<T>`, `Map<K,V>`, closures, and erased coroutines are type-erased `void*`; `uniq T` / `ref T` are `T*`; `weak T` is `roxy_weak` (`{void* ptr; uint64_t generation;}`).

`uniq`/`ref` add that `*` only over a **value-shaped** pointee — a struct, enum or
primitive. Over a pointee that already emits as a C pointer (`List`, `Map`,
`string`, a closure, a coroutine, or a nested `uniq`/`ref`) the borrow *is* the
same pointer, so no star is added: `ref List<i32>` is `void*`, not `void**`
(`emits_as_c_pointer`).

All heap objects are preceded by a `roxy_object_header`, matching the VM's layout; `weak_generation == 0` means dead/tombstoned.

Roxy identifiers that are C++ keywords get a reserved `roxy_kw_` prefix in `emit_mangled_name`; `$$` in mangled names becomes `__`.

## IR to C Mapping

Most ops map one-to-one onto C expressions (see the emitter's op switch). The non-obvious parts:

- **Structs.** Types are forward-declared, then defined in dependency order (a topological sort over field types *and* tagged-union variant fields, since both embed by value). Slot-based layout is field-order compatible with C.
- **`StackAlloc`** emits both the backing value and a pointer to it (the IR refers to structs through pointers), zero-initialized with `memset` — not `= {0}`, which fails in C++ when the first field is an enum.
- **`StructCopy`** is a `memcpy` sized from the slot model (`slots * 4`) — see the tagged-union row under Known C-backend gaps for where that disagrees with the C layout.
- **Functions** take no `roxy_ctx*` (the context is thread-local, below). Struct params are pointers. A child passed where a parent is expected gets an explicit cast. Large struct returns (slot_count > 4) use the IR builder's hidden `__ret_ptr` param and a `void` return. A method returning a closure has an unset IR `return_type`, so prototypes use `effective_return_type`, derived from the actual `Return` value.
- **Control flow** is labels + gotos: each `IRBlock` becomes a label and block arguments become locals assigned before the `goto`. Gotos are trivially correct for any CFG shape, and C compilers reconstruct structured control flow internally, so there is no performance penalty.
- **`Delete`** is a recursive typed delete (`emit_typed_delete` / `emit_delete_slot`), the C analogue of the VM's descriptor-driven `delete_value`: struct destructor chains, container element/key/value teardown, then the buffers and header. It is null-guarded (delete-on-null is a no-op, as on the VM).
- **Tagged unions** emit a struct with the discriminant plus an anonymous `union` of anonymous per-variant structs, so variant fields are accessed directly on the parent.

### Value vs. pointer representation

The recurring source of C-backend bugs. A small struct returned by value is a *value* local, while a struct local, param, or field address is a *pointer*; C++'s strict typing turns every mismatch into a compile error or a miscompile. Sites that must distinguish them: `StructCopy` operands, call arguments to pointer-semantics params (`self`/`ref`), struct-value block params (a merge can receive a pointer from one predecessor and a value from another — the arg is dereferenced into the param), and owned-local init/delete/null-out. `m_pointer_values` records which values are pointers (and whether field access uses `->` or `.`).

Similarly, `void*` → `T*` is ill-formed in C++, so a null stored into a typed pointer slot (`SetField`, `StorePtr`, block arguments) is cast to the destination type, and a nil assigned to a value-struct or `weak` field is a `memset`. An `out`/`inout` param adds one level of indirection on top of `emit_type` (`inout uniq T` is `T**`), except the `self` receiver, which is also flagged `param_is_ptr` but is a one-level `ref`.

### Coroutines

`coroutine_lower()` runs *before* the C backend and rewrites every coroutine
function into ordinary functions — `init`, `__coro_<func>$$resume`, and
`__coro_<func>$$delete` — built from ops the backend already handles.
`IROp::Yield` never reaches the emitter (the IR validator rejects any survivor).
Since coroutine values are first-class, the emitter has a little dedicated logic:

- **A known `Coro<T>` is a pointer to its state struct** (`__coro_<func>*`). The
  synthesized state struct is appended to `IRModule::struct_types` *in the lowering
  pass* (after `collect_backend_types` runs), so it gets a typedef, a
  dependency-sorted definition, and a `TYPEID_` like any other struct.
- **An erased `Coro<T>` is `void*`**, exactly like a closure value. `resume()` is an
  `IROp::CallIndirect` through `g_closure_fns[]`, the same dispatch as a closure,
  with the resume function registered via `IROp::FuncIndex`. `done()` reads
  `__state` through a `__coro_header` cast (the common `{__resume_idx, __state}`
  prefix), so it works without the concrete struct.
- **Deleting a `Coro<T>` runs `__coro_<func>$$delete`** — directly for a known value;
  for an erased one via `DropKind::Closure` → `__closure_delete`, which switches on
  `__resume_idx` (the resume function's dispatch slot doubles as the delete key).

Promoted locals are stored with raw `SetField` where the regular path would use
`StructCopy` / `Nullify`, so `SetField` dereferences a struct rvalue assigned to a
struct-value field and casts a null assigned to a pointer field. The generated
`$$delete` returns a void-typed `ConstInt` sentinel, so a void function emits
`return;` and void constants emit nothing.

### Exceptions

The VM resolves exceptions with a runtime handler table + an unwinding loop that
runs PC-range-keyed cleanup. The C backend has labels + gotos, not PC ranges, so
it uses a **checked-return** model instead (no setjmp/longjmp, no C++ EH):

- **Pending state.** A thread-local in-flight exception lives in `roxy_rt`.
  `throw` stores the heap exception object and jumps to a routing label; after
  every `Call` / `CallExternal` the emitter writes
  `if (roxy_exception_pending()) goto …`. User-`native` functions are assumed not
  to throw (no post-`CallNative` check).
- **Routing.** Each set of `IRExceptionHandler`s sharing a try entry becomes one
  `__dispatch_<id>` label. A throw / pending-after-call in block *B* routes to the
  innermost try whose `try_body_blocks` contains *B*, else to `__unwind`. The
  dispatch runs the try-body cleanup, then a `roxy_exception_type_id()` if/else
  chain (catch-all / `finally.catch` last); no match falls to the next-outer
  dispatch or `__unwind`. `finally` needs no special logic — the IR already
  duplicates it into each exit. Unhandled exceptions out of `main_entry` print and
  exit nonzero.
- **Cleanup.** Per-frame cleanup reuses `emit_typed_delete`, null-guarded
  (`if (v) { … v = 0; }`) and LIFO. A dispatch cleans owned locals created inside
  its try body; `__unwind` cleans the whole frame. Correctness rests on every
  cleanup-tracked owned-local pointer — block parameters included — being **zero-initialized** at declaration,
  **nulled after** a normal scope-exit `Delete`, and nulled on move — so the guard
  skips not-yet-created, already-freed, and moved values. Cross-frame unwinding
  falls out naturally: a callee's `__unwind` cleans its frame and returns, then
  the caller's post-call check propagates.
- **Move-into-call ordering.** A `nullify V` consumed by a later call (or
  `Closure`) is emitted *after* that op reads `V` (the VM treats `nullify` as a
  scope marker, not a runtime zero) but *before* the post-call exception check, so
  a moved argument is owned by the callee and skipped by the caller's unwinding
  cleanup. A nullify of a value being returned is dropped.

### Closures

The frontend lifts each lambda to a top-level `__lambda_<id>_call(__env, …)`
function and an env struct (`[__call_idx: u32][captures…]`); a function value is a
type-erased `void*` to that env. Env structs are appended to
`IRModule::struct_types` (in the IR builder's post-build loop) so they get
typedefs + `TYPEID_`s.

The VM dispatches `CALL_INDIRECT` through its function table; AOT has no such
table, so the emitter builds a per-module `g_closure_fns[]` indexed by
`__call_idx`:

- **`CallIndirect`** reads `*(uint32_t*)env`, indexes `g_closure_fns`, casts to
  `Ret(*)(void*, params…)`, and prepends the env. A `ref fun` callee is the same
  pointer; a `weak fun` callee uses `.ptr`.
- **`AssertHeap`** (ref/weak `self` capture on a possibly-stack receiver) →
  `if (!roxy_heap_owns(p)) abort();`, reproducing the VM's `owns` trap.
- **Delete** of a `Function` is type-erased, so it routes to a generated
  `__closure_delete(env)` that dispatches the env destructor by `__call_idx`, then
  frees.

## Thread-Local Runtime Context

The VM's native ABI (reading/writing registers through `RoxyVM*`) has no analogue in AOT. Instead runtime state lives in a small `roxy_ctx` (`allocator` / `exception_state` / `user_data`) reached through thread-local storage (`roxy_set_ctx` / `roxy_get_ctx`), so it never appears in a function signature.

`roxy_ctx` is the first member of `RoxyVM`, so the same native functions work in both modes. In **VM mode** the interpreter brackets every public entry with `roxy::ScopedContext(&vm->ctx)`. In **AOT mode** the generated `main()` (or the embedder, via `ScopedContext`) sets it. Natives are therefore plain C++ taking only their logical parameters and reach engine state via `roxy_get_ctx()->user_data`.

### Native Binding Categories

| Binding style | VM mode | AOT mode |
|---|---|---|
| `bind<FnPtr>("name")` | `FunctionBinder` wraps FnPtr, VM sets TLS ctx | direct call to FnPtr |
| `bind_method<FnPtr>(...)` | binder wraps | direct call with `self*` |
| `bind_native(fn, sig)` | VM-style manual wrapper | needs `bind_native(vm_fn, sig, aot_symbol)` overload |
| Built-in (print, list, …) | VM native functions | `roxy_rt` C functions (use TLS ctx internally) |

`bind<FnPtr>` stores both the VM wrapper and the original function pointer, which the emitter references directly. A `CallNative` referencing a VM-only `bind_native(fn, sig)` is an emitter compile error. Built-in natives map by name to `roxy_rt` equivalents (`lookup_static_native_mapping`).

When that static lookup misses, the emitter consults `CEmitterConfig::native_registry` and emits a typed direct call using the entry's `aot_symbol_name` (defaults to the Roxy name). It pre-scans IR for user-native `CallNative` ops and writes `extern` declarations into the preamble, so AOT binaries link against either inline-defined `native_include_paths` headers or separately-compiled translation units.

`List<T>` / `Map<K,V>` are type-erased, slot-sized containers: one C implementation serves all element types (allocated with the element slot count), values go in and out by pointer, and the emitter casts at the use site.

## Runtime Library (`roxy_rt.h`)

C implementations of everything needing allocation or complex logic; allocating functions read `roxy_get_ctx()` internally, so the public API is context-free. See `rt/roxy_rt.h`.

Allocation flows through `roxy_ctx.allocator`, a `roxy_allocator` vtable (`alloc` / `free` / `owns` / `userdata`):

1. **Slab allocator** (`rt/slab_allocator.{hpp,cpp}`) — used by both VM and AOT. Freed slots stay mapped (zeroed) so stale weak refs reliably read "dead", and recycled slots get fresh random generations. AOT brings up a process-wide slab via `roxy_rt_init` / `roxy_rt_shutdown`; the VM installs a per-VM one via `make_slab_allocator_vtable(...)`.
2. **Malloc allocator** (`roxy_malloc_allocator`) — defensive fallback when there is no ctx/allocator (e.g. static initializers before `roxy_rt_init`). Weak-ref soundness is best-effort since libc may reuse addresses.

`roxy_rt.h` also provides the embedder-facing C++ wrappers: `roxy::uniq<T>` (move-only, destructor + free on scope exit), `roxy::ref<T>` (counted borrow handle that never frees), `roxy::weak<T>` (pointer + generation), and typed facades `roxy::String` / `roxy::List<T>` / `roxy::Map<K,V>`. The VM bindings `rx::RoxyString` / `rx::RoxyList<T>` / `rx::RoxyMap<K,V>` are aliases of these, so VM and AOT share one wrapper implementation.

## Generated Output

`emit_source()` produces one `.cpp`; `emit_header()` produces a `.hpp` with only `pub` items: enums, structs with inline method wrappers (same codegen as the free-function call), `make_<T>` / `make_<T>__<ctor>` factories returning `roxy::uniq<T>`, and function declarations.

```cpp
struct Point {
    int32_t x; int32_t y;
    int32_t sum() { return Point__sum(this); }   // inline wrapper
};

inline roxy::uniq<Player> make_Player(void* name, int32_t health) {
    Player* ptr = (Player*)roxy_alloc(sizeof(Player), TYPEID_Player);
    Player__new(ptr, name, health);
    return roxy::uniq<Player>(ptr, Player__delete);
}
```

The `.cpp` does not include the header. Functions have external linkage; module globals, drop glue and the closure dispatch table are `static`. In standalone mode (`emit_main_entry`) it ends with a `main()` that sets up the ctx and calls `main_entry()`. The embedder brackets calls with `roxy::ScopedContext`.

`#line` directives map generated lines back to Roxy source: `IRFunction::source_line` seeds one at function entry, and `IRInst::source_line` (set by `IRBuilder::emit_inst` at each statement/decl boundary) re-emits it at every statement-line transition, deduplicated.

## Build Integration

**Not implemented yet.** The `roxy` CLI has no C-backend flags, and `CEmitter` has no production driver — today it is reached only through `compile_to_cpp` / `compile_and_run_cpp` in the test harness. The intended shape is a CMake `add_custom_command` that emits `scripts.{hpp,cpp}` before the main build, compiled alongside engine code against `roxy_rt`, with the embedder's headers passed through `CEmitterConfig::native_include_paths`.

## Testing

`compile_and_run_cpp(source)` runs the full pipeline (Roxy → IR → CEmitter → temp `.cpp` → `c++ -std=c++17` → run → check exit code + stdout). `compile_to_cpp` returns the C++ string; `compile_and_run_cpp_with_registry` links an inline native header for AOT NativeRegistry tests; pass `debug=true` to dump IR and generated source. Because these invoke the system compiler, the suite must run outside the sandbox.

The IR reaching `CEmitter` is **optimized** — the harness runs `coroutine_lower` → `optimize_module` → `IRValidator`, the same order as `Compiler::link_modules()`. `CEmitter` has no production driver, so this harness *is* the C-backend pipeline; keep it aligned with the bytecode path rather than letting the two drift.

The harness runs the binary with stderr left attached (no `2>&1`): a redirect makes the shell fork, masking a runtime trap (SIGABRT/SIGSEGV) into a 128+signo *exit code* instead of `WIFSIGNALED`.

**Binary cache.** Per-case wall time is dominated not by compilation (~110ms) but by **running** the freshly-built binary (~350ms) — macOS performs a synchronous first-launch security assessment on each never-seen executable; a warm re-run of the *same* file is ~5ms. `compile_and_run_cpp` therefore content-addresses the compiled binary under `$TMPDIR/roxy_ccache/c_<hash>`, keyed on `fnv1a64(generated_source) ⊕ runtime_version_hash` — the runtime-version hash (of the compiled `roxy_rt` objects) is folded in so a runtime change invalidates every cached binary. A miss compiles to a temp path and `rename`s into the cache atomically (a partial build never becomes a hit). The full `<C>` suite takes **~1s on a warm cache**. Set `ROXY_CBACKEND_NO_CACHE=1` to bypass. (`compile_and_run_cpp_with_registry` is intentionally uncached — it links extra translation units not captured by the key.)

### Parametric E2E coverage (`<VM>` / `<C>`)

Most `tests/e2e/` suites are **backend-parametric**: a single test body runs on
both the bytecode VM and the C backend via doctest's `TEST_CASE_TEMPLATE`, driven
by `tests/e2e/test_e2e_backend.hpp` (`VMBackend` / `CBackend`, unified result
where `value` is the VM return value or the C process exit code). Each
instantiation is a separate case named `<TestName><VM>` / `<TestName><C>`:

```bash
./roxy_tests --test-case="*<VM>*"          # VM only (sandbox-safe, fast)
./roxy_tests --test-case="*<C>*"           # C only  (needs the system compiler)
./roxy_tests --test-case-exclude="*<C>*" \
             --test-suite-exclude="E2E C Backend"   # everything compiler-free (in-sandbox)
```

Cases the C backend cannot run are demoted to plain `TEST_CASE` (VM-only) with a
`// VM-only: <reason>` annotation, in three categories: results outside the
0..255 exit-code range, runtime-trap/abort tests whose behavior differs on C, and
the known C-backend gaps below. `test_c_backend.cpp` retains only C-specific
tests (generated-header emission, AOT NativeRegistry dispatch, `#line`
directives) — feature coverage proper lives in the shared parametric suites.

### Known C-backend gaps (surfaced by the parametric suite)

These cases are VM-only pending fixes:

| Area | Symptom |
|------|---------|
| **ref-local count balancing** | `ref`-local `RefInc`/`RefDec` balancing across control flow (loop continue/break, nested scopes). |
| **coroutine uniq-field cleanup** | `Coro<T>` promoting `uniq`/`List<uniq>`/`Map<_,uniq>` state |
| **cleanup record naming a by-value struct** | `emit_cleanup_records` guards every record with `if (v) { … v = 0; }`, which assumes the value is pointer-shaped. When the record names a *value* struct — a small struct returned by value and materialized into a local, e.g. the `make(a)` result in `var v: Box = Box(""); if (…) { v = make(a); } throw E(v);` — that emits `if (v8)` and `v8 = 0;` on a `Box`, and the generated source does not compile at all. Needs the same value-vs-pointer distinction `emit_typed_delete` already makes (address-of, and no null guard). |
| **tagged union with a pointer-sized variant field** | byte sizes derived from the 4-byte slot model disagree with the C compiler's natural layout. `struct V { when kind: K { case Num: n: i32; case Str: s: string; } }` is 3 slots = 12 bytes, but the emitted `struct V { K kind; union { …; struct { void* s; }; }; }` aligns the union to 8, so `sizeof(V) == 16` and `s` lives at offset 8. `StructCopy` then emits `memcpy(dst, src, 12)` and copies **half the pointer**; the `memset` zeroing the variant payload is off by the same 4 bytes. Any use of the copy that touches the pointer field (a retain, a read) segfaults. Nothing to do with exceptions — a plain `var e: E = E { v = val };` crashes too. The VM is unaffected: there the slot model *is* the layout. Fixing it means sizing struct copies/zeroing from the emitted C type (`sizeof`/`offsetof`) rather than `slots * 4`. |

Not bugs, also VM-only: tests asserting a result > 255 (8-bit exit code — many
are recoverable by asserting printed stdout instead) and runtime-trap/overflow
tests where the C binary aborts rather than trapping cleanly.

## Files

- `compiler/codegen/c_emitter.{hpp,cpp}` — the emitter
- `rt/roxy_rt.{h,cpp}` — C runtime + C++ wrappers; `rt/slab_allocator`, `rt/vmem*`, `rt/string_intern` — shared runtime pieces
- `vm/binding/binder.hpp`, `registry.hpp` — `bind<>` / `bind_native` overloads, `aot_symbol_name`
- `tests/e2e/test_c_backend.cpp`, `tests/unit/test_runtime_ctx.cpp`, `tests/e2e/test_helpers.{hpp,cpp}` (C harness + binary cache)
