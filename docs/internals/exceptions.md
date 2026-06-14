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

The `Exception` trait is registered as a built-in during semantic analysis (same pattern as `Printable` and `Hash`). It requires a single method, `message(): string`.

The analyzer checks that each thrown expression's type implements `Exception`, failing with `"thrown type 'X' does not implement the Exception trait"` otherwise.

**Catch-all type:** A `catch (e)` with no type annotation gives `e` the opaque `ExceptionRef` type (`TypeKind::ExceptionRef`). Only `message()` is callable on it — field access and other method calls are rejected. This needs only a single stored function index, avoiding a `dyn Trait` mechanism.

## Pipeline

The lexer recognizes four keywords (`KwTry`, `KwCatch`, `KwThrow`, `KwFinally`). The parser produces `ThrowStmt`, `TryStmt`, and `CatchClause` AST nodes (see `ast.hpp`); semantic analysis validates thrown/caught types against `Exception`, resolves catch variable types, enforces catch-all-last, and assigns `ExceptionRef` to catch-all variables.

**IR generation.** `throw` emits `IROp::Throw` (unary operand = exception pointer) followed by an `Unreachable` terminator. A `try/catch/finally` generates this control flow:

```
[try body blocks] ──(normal)──> [finally?] → [after-try]
                                     ↑
[catch dispatch] ← handler table     |
   ├─ type_id match? → [catch body] → [finally?] → [after-try]
   └─ no match       → re-throw
```

Handler/finally metadata is recorded on `IRFunction` as `IRExceptionHandler` (try-block range, handler block, `type_id` to match with 0 = catch-all, type name) and `IRFinallyInfo` (see `ssa_ir.hpp`). `finally` is realized by duplicating the finally body per exit path. Variables modified inside try/catch/finally bodies are propagated to the after-try merge via block arguments, like `if`/`when`.

**Bytecode lowering.** `IROp::Throw` lowers to the `THROW` opcode (`0xD2`, ABC: throw `regs[a]`). Handler metadata is translated from block IDs to PC offsets as `BCExceptionHandler` (protected `[try_start_pc, try_end_pc)` range, `handler_pc`, `type_id`, and the `exception_reg` to receive the exception pointer in the handler — see `bytecode.hpp`).

**Runtime.** `THROW` reads the exception pointer from a register, extracts `type_id` from its `ObjectHeader`, looks up the `message()` function index, and stows all three in VM state (`in_flight_exception`, `in_flight_exception_type_id`, `in_flight_message_fn_idx`) before entering the unwinding loop:

1. Take the current frame's function and PC offset.
2. Scan `exception_handlers` in order for a handler whose range covers the PC (`try_start_pc <= pc < try_end_pc`) and whose `type_id` matches (or is catch-all, `type_id == 0`).
3. If found: set PC to `handler_pc`, store the exception pointer in `exception_reg`, clear `in_flight_exception`, resume.
4. If not: clean up the current frame (ref-dec parameters), pop it, and continue unwinding in the caller.
5. If the call stack empties: set `vm->error = "Unhandled exception: ..."` and return false.

**C backend.** The AOT path can't use the VM's runtime PC-range handler table, so
it lowers the same IR (handlers, `finally` duplication, `cleanup_info`) with a
**checked-return** model: a thread-local in-flight exception, per-try
`__dispatch_<id>` labels reached by `throw` / a pending-after-call check, and
null-guarded per-frame cleanup reusing `emit_typed_delete`. See
`docs/internals/c-backend.md` ("Exceptions").

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

| File | Purpose |
|------|---------|
| `include/roxy/shared/token_kinds.hpp` | `KwTry`, `KwCatch`, `KwThrow`, `KwFinally` tokens |
| `include/roxy/compiler/ast.hpp` | `ThrowStmt`, `TryStmt`, `CatchClause` AST nodes |
| `src/roxy/compiler/parser.cpp` | `throw_statement()`, `try_statement()` |
| `include/roxy/compiler/types.hpp` | `TypeKind::ExceptionRef` |
| `src/roxy/compiler/semantic.cpp` | Exception trait registration, throw/try analysis |
| `include/roxy/compiler/ssa_ir.hpp` | `IROp::Throw`, `IRExceptionHandler`, `IRFinallyInfo` |
| `src/roxy/compiler/ir_builder.cpp` | `gen_throw_stmt()`, `gen_try_stmt()` |
| `src/roxy/compiler/ssa_ir.cpp` | RPO reordering with handler block seeding |
| `include/roxy/vm/bytecode.hpp` | `THROW` opcode, `BCExceptionHandler` |
| `src/roxy/compiler/lowering.cpp` | Throw lowering, handler table PC translation |
| `include/roxy/vm/vm.hpp` | `in_flight_exception`, `in_flight_message_fn_idx` |
| `src/roxy/vm/interpreter.cpp` | THROW handler, unwinding loop |
| `src/roxy/compiler/ir_validator.cpp` | Throw/handler validation |
| `tests/e2e/test_exceptions.cpp` | E2E test suite (15 tests) |
