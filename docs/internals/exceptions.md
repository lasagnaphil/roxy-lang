# Exception Handling

Exception handling in Roxy provides structured error recovery via `try`/`catch`/`throw`/`finally`. It uses a built-in `Exception` trait, concrete-type catch matching, and handler tables for zero-overhead on the non-exception path.

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

- `try` requires at least one `catch` OR a `finally` (or both)
- `throw` accepts any struct expression that implements the `Exception` trait
- `throw` implicitly heap-allocates struct literal values
- `catch (e: Type)` catches only that exact concrete type (no subtype matching)
- `catch (e)` is a catch-all (matches any exception)
- Catch clauses are tested top-to-bottom; catch-all must be last
- `finally` always executes (normal exit, catch exit, or stack unwinding)
- Code after `throw` is unreachable (dead code)

## Exception Trait

The `Exception` trait is registered as a built-in trait during semantic analysis (same pattern as `Printable` and `Hash`). It requires a single method: `message(): string`.

**throw validation:** The semantic analyzer checks that the thrown expression's type implements `Exception`. If not: `"thrown type 'X' does not implement the Exception trait"`.

**Catch-all type:** When `catch (e)` has no type annotation, `e` gets the opaque `ExceptionRef` type (`TypeKind::ExceptionRef`). Only `message()` can be called on it — field access and other method calls are rejected.

## Pipeline

### Lexer & Parser

Four keywords: `try`, `catch`, `throw`, `finally` (`KwTry`, `KwCatch`, `KwThrow`, `KwFinally`).

Two AST nodes:

```cpp
struct ThrowStmt {
    Expr* expr;                   // Expression implementing Exception trait
};

struct CatchClause {
    StringView var_name;          // Catch variable name
    TypeExpr* exception_type;     // Type annotation (nullptr for catch-all)
    Stmt* body;                   // Block statement
    SourceLocation loc;
    Type* resolved_type;          // Set by semantic analysis
};

struct TryStmt {
    Stmt* try_body;               // Block statement
    Span<CatchClause> catches;
    Stmt* finally_body;           // nullptr if no finally
};
```

### Semantic Analysis

- Validates thrown types implement `Exception` trait
- Resolves typed catch variable types, checks they implement `Exception`
- Ensures catch-all is last when present
- Catch-all variable gets `ExceptionRef` type
- Validates `try` has at least one catch or finally

### IR Generation

`throw` emits `IROp::Throw` (unary operand = exception pointer) followed by an `Unreachable` terminator. Code after throw is dead.

`try/catch/finally` generates the following control flow:

```
[try body blocks] ──(normal)──> [finally?] → [after-try]
                                     ↑
[catch dispatch] ← handler table     |
   ├─ type_id match? → [catch body] → [finally?] → [after-try]
   └─ no match       → re-throw
```

Exception handler metadata is stored on `IRFunction`:

```cpp
struct IRExceptionHandler {
    BlockId try_entry;       // First block of try body
    BlockId try_exit;        // Last block of try body (inclusive)
    BlockId handler_block;   // Catch handler entry block
    u32 type_id;             // Concrete type_id to match (0 = catch-all)
    StringView type_name;    // Struct name for the catch type
};

struct IRFinallyInfo {
    BlockId try_entry, try_exit;
    BlockId finally_block;
    BlockId finally_end_block;
};
```

Variables modified inside try/catch/finally bodies are propagated through block arguments (phi nodes) to the after-try merge block, following the same pattern as `if`/`when` statements.

### Bytecode Lowering

`IROp::Throw` lowers to `THROW` opcode (`0xD2`, ABC format: throw `regs[a]`).

Handler metadata is converted from block IDs to PC offsets:

```cpp
struct BCExceptionHandler {
    u32 try_start_pc;     // Protected range start (inclusive)
    u32 try_end_pc;       // Protected range end (exclusive)
    u32 handler_pc;       // Catch handler entry point
    u32 type_id;          // Concrete type_id to match (0 = catch-all)
    u8 exception_reg;     // Register to store exception ptr in handler
};
```

### Runtime

**VM state:**
```cpp
void* in_flight_exception;          // Exception object being propagated
u32 in_flight_exception_type_id;    // type_id from ObjectHeader
u32 in_flight_message_fn_idx;       // Function index for message() method
```

**THROW opcode:** Reads exception pointer from register, extracts `type_id` from `ObjectHeader`, looks up `message()` function index, stores in VM state, then enters the unwinding loop.

**Stack unwinding algorithm:**
1. Get current frame's function and PC offset
2. Search `exception_handlers` (in order) for matching handler:
   - `try_start_pc <= current_pc < try_end_pc`
   - `type_id` matches or handler is catch-all (`type_id == 0`)
3. If handler found: set PC to `handler_pc`, store exception ptr in `exception_reg`, clear `in_flight_exception`, resume execution
4. If no handler: clean up current frame (ref-dec parameters), pop frame, continue unwinding in caller
5. If call stack is empty: set `vm->error = "Unhandled exception: ..."`, return false

## RPO Block Reordering

Exception handler blocks (catch blocks) are not reachable through normal control flow — they're only entered via exception dispatch. The RPO reordering pass seeds its DFS traversal with handler block IDs in addition to the entry block, ensuring handler blocks are preserved and correctly ordered. Handler block IDs in the metadata are remapped after reordering.

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
