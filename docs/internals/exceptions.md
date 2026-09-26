# Exception Handling

Roxy provides structured error recovery via `try`/`catch`/`throw`/`finally`. It uses a built-in `Exception` trait, concrete-`type_id` catch matching, and handler tables for zero-overhead on the non-exception path.

## Syntax

```roxy
// Built-in trait (auto-registered like Printable/Hash)
trait Exception;
fun Exception.message(): string;

// User exception type
struct ValueError {
    msg: string;
    value: i32;
}

fun ValueError.message(): string for Exception {
    return f"ValueError: {self.msg}";
}

fun risky(): i32 {
    throw ValueError { msg = "bad", value = -1 };
}

fun main(): i32 {
    try {
        var result = risky();
    } catch (e: ValueError) {
        print(f"Caught: {e.msg}");
    } catch (e) {
        // Catch-all: e is ExceptionRef (opaque, only message() callable)
        print(f"Unknown error: {e.message()}");
    } finally {
        print("cleanup");
    }
    return 0;
}
```

## Grammar

```
try_stmt     = "try" block catch_clause* [ "finally" block ] ;
catch_clause = "catch" "(" IDENT [ ":" type_expr ] ")" block ;
throw_stmt   = "throw" expression ";" ;
```

## Language Rules

- `try` requires at least one `catch` OR a `finally` (or both).
- `throw` accepts any struct expression that implements the `Exception` trait; struct literal values are implicitly heap-allocated.
- `catch (e: Type)` catches only that exact concrete type (no subtype matching).
- `catch (e)` is a catch-all (matches any exception); it must be last.
- Catch clauses are tested top-to-bottom.
- `finally` always executes (normal exit, catch exit, or stack unwinding).
- Code after `throw` is unreachable (dead code).

## Exception Trait

`Exception` is a builtin trait (like `Printable` and `Hash`) requiring `message(): string`; every thrown type must implement it.

**Catch-all type:** A `catch (e)` with no type annotation gives `e` the opaque `ExceptionRef` type (`TypeKind::ExceptionRef`). Only `message()` is callable on it — field access and other method calls are rejected. This needs only a single stored function index, avoiding a `dyn Trait` mechanism.

## Index-operator exceptions

Two built-in exception types are thrown by the container index operator, so a
missing lookup is recoverable instead of a hard abort:

- `list[i]` with `i` out of bounds (including negative) throws **`IndexError`**.
- `m[k]` with `k` absent throws **`KeyError`**.

Both are catchable by type — `catch (e: IndexError)` / `catch (e: KeyError)` — or
by a catch-all, and both implement `Exception`, so `e.message()` works in a typed
catch (`"List index out of bounds"` / `"Map key not found"`). `m.get(k)` and
`list.pop()` are unchanged (they still abort); the throw is specific to the `[]`
read. (An `inout`/`out` element *borrow* — the `INDEX_ADDR_*` lvalue path — also
still traps rather than throwing.)

```roxy
try {
    var v: i32 = m[missing];
} catch (e: KeyError) {
    print(e.message());   // "Map key not found"
}
```

**Registration.** `KeyError` and `IndexError` are decl-less, fieldless structs
implementing `Exception`, registered once in the shared `TypeEnv`
(`register_builtin_exception_types`) so every module — and every single-source
compile path — can name them in a `catch` without a per-module symbol or a prelude
module. Their `message()` bodies are synthesized on demand by the IR builder, like
container `to_string` (`request_exception_message`). The C backend also needs the
struct definitions, so the throw / message sites push the type into
`IRModule::struct_types` (`register_backend_exception_type`; the VM ignores that
list).

> The messages are intentionally generic — `KeyError` does not embed the
> offending key (arbitrary key types can't be formatted uniformly) and
> `IndexError` does not embed the index/length. Enriching them with fields is a
> possible follow-up.

**Lowering (miss detection).** `list[i]` reads emit a cheap in-IR bounds check
(`len = List$$len; if (i < 0 || i >= len) throw IndexError`) before the existing
`INDEX_GET_LIST` — the length is a header read, so no second probe. `m[k]` reads
stay single-probe: a new `IROp::IndexTryAddr` (VM opcode `INDEX_TRYADDR_MAP`; C
`roxy_map_get_or(map, key, NULL)`) returns the value-slot pointer or 0, the IR
branches on `ptr == 0` to a `throw KeyError` block, and the hit path loads the
value from `ptr` (`LoadPtr` for an inline value; a struct value re-reads via
`IndexGet`, the uncommon case). Both throws reuse the ordinary `New` + `Throw` +
unwinding machinery, so `finally` and cross-frame propagation need nothing extra.

## Pipeline

**IR generation.** `throw` emits `IROp::Throw` (unary operand = exception pointer) followed by an `Unreachable` terminator. A `try/catch/finally` generates this control flow:

```
[try body blocks] ──(normal)──> [finally?] → [after-try]
                                     ↑
[catch dispatch] ← handler table     |
   ├─ type_id match? → [catch body] → [finally?] → [after-try]
   └─ no match       → re-throw
```

Handler/finally metadata is recorded on `IRFunction` (`IRExceptionHandler`, `IRFinallyInfo`; `type_id` 0 = catch-all). `finally` is realized by duplicating the finally body per exit path. Variables modified inside try/catch/finally bodies are propagated to the after-try merge via block arguments, like `if`/`when`.

**Bytecode lowering / runtime.** Handler metadata is translated from block IDs to PC ranges (`BCExceptionHandler`). `THROW` stows the exception pointer and its header `type_id` as the VM's in-flight exception, then unwinds: in each frame it scans the handler table in order for a range covering the PC whose `type_id` matches (or is catch-all); on a match it jumps to the handler with the exception in `exception_reg`, otherwise it runs the frame's PC-range cleanup records (`execute_cleanup`), pops the frame, and continues in the caller. The matched-handler path also runs the cleanup records for the scopes it exits. An empty call stack frees the exception and fails with "Unhandled exception".

**C backend.** With no runtime PC-range table, the AOT path lowers the same IR with a
**checked-return** model; see [c-backend.md → Exceptions](c-backend.md#exceptions).

## Exception object lifetime

`throw` heap-allocates the exception (a struct literal is implicitly boxed), and
the handled path hands the raw pointer to the catch without freeing it. To reclaim
it, the IR builder registers the **caught exception as an owned local of the catch
scope** (`gen_try_stmt`), so the ordinary scope-cleanup machinery frees it exactly
once on **every** catch exit: normal fall-through, `return`, `break`, `continue`,
and a *new* `throw` unwinding out of the catch. A typed `catch (e: E)` frees it as
`uniq E` (running `E`'s destructor); a catch-all `catch (e)` has no compile-time
concrete type, so it frees the memory **type-erased** — the caught type's
`fun delete` does not run (the same limitation as the unhandled-exception path,
which a type-erased free can't reach a bytecode destructor through).

**Re-throw is a hand-off, not a free.** A `throw e` (or a nested `throw` while an
exception is in flight) routes the object to the unwind machinery, which owns it
until the *next* handler frees it. Rather than track this with move state, the free
paths carry an **in-flight guard**: the VM's `object_free` / `delete_value` skip
`vm->in_flight_exception`, and the C backend's dispatch cleanup skips
`roxy_exception_current()`. So a catch scope's cleanup record firing during a
re-throw's unwind is a no-op for the object being re-thrown; the eventual handler
frees it once. This makes `throw e`, `throw new` (frees the old, unwinds the new),
and conditional re-throw all correct without per-path bookkeeping.

## RPO Block Reordering

Catch (handler) blocks are not reachable through normal control flow — they're entered only via exception dispatch. The RPO reordering pass therefore seeds its DFS with the handler block IDs in addition to the entry block, so handler blocks are preserved and correctly ordered; the handler block IDs in the metadata are remapped after reordering.

## Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Exception mechanism | Built-in `Exception` trait | Fits Roxy's trait system |
| Runtime matching | Concrete `type_id` comparison | Simple, fast, no hierarchy walk |
| Catch-all | `ExceptionRef` (opaque, `message()` only) | Single stored function index; no `dyn Trait` needed |
| Runtime mechanism | Handler table | Zero overhead on non-exception path |
| throw allocation | Implicit heap alloc for struct literals | Clean syntax |
| Finally | Code duplication per exit path | Simple; finally bodies are typically small |

## Files

- `compiler/ir/ir_builder_stmt.cpp` — `gen_throw_stmt`, `gen_try_stmt` (catch-scope ownership of the caught exception)
- `compiler/ir/ir_builder.cpp` — index-operator throws, synthesized `message()` bodies
- `compiler/codegen/lowering.cpp` — handler-table PC translation
- `vm/interpreter.cpp` — `THROW` and the unwinding loop; `vm/object.cpp` — in-flight guard
- `compiler/ir/ssa_ir.cpp` — RPO reordering with handler seeding
- `tests/e2e/test_exceptions.cpp`, `tests/e2e/test_index_exceptions.cpp`
