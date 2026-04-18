# Lox Interpreter in Roxy

A Roxy reimplementation of the tree-walk Lox interpreter from Part II of [Crafting Interpreters](https://craftinginterpreters.com/). This serves as a real-world stress test of the Roxy language — nearly every language feature (tagged unions, `uniq` ownership, generics, traits, pattern-matching, exceptions) gets exercised, and several compiler bugs were found along the way (see `TODO.md` at the repo root).

All source lives in this directory. End-to-end tests run via `tests/e2e/test_lox.cpp`, which loads these `.roxy` files as modules and executes `test.roxy`'s Roxy-level assertions. To run a Lox script directly: `./build/roxy examples/lox/main.roxy path/to/script.lox`.

## Current Status

**Implemented** — Phases 1–10, the complete Crafting Interpreters Part II tree-walk interpreter (book Ch. 4 through Ch. 13):

| Phase | Scope | Status |
|-------|-------|--------|
| 1 | Native string helpers (`str_char_at`, `str_substr`, `str_to_f64`, `str_from_code`, `clock`, `read_file`) | ✅ |
| 2 | Scanner — `TokenType` enum, `Token` struct (tagged union over `LiteralKind`), full Lox lexing | ✅ |
| 3a | Expression parser — recursive descent for equality/comparison/term/factor/unary/primary; AST printer | ✅ |
| 3b | Statement parser — var decls, blocks, if/else, while, for-desugaring, print, assignment, logical and/or, error recovery | ✅ |
| 4 | Expression interpreter — literal/grouping/unary/binary with `RuntimeError` exception; `clock()` native | ✅ |
| 5 | Statements + environments — `List<Environment>` pool with `Map<string, LoxValue>` bindings, block scoping via push/pop | ✅ |
| 6 | Control flow | ✅ (folded into Phase 5 — if/while/for/logical all work) |
| 7 | Functions — `LoxFunction` table, `ReturnException`, `clock()` native, recursion, arity checks, nested `fun`, genuine closures | ✅ |
| 8 | Resolver — static variable resolution pass, per-`expr_id` scope-depth table, self-init ban, top-level-`return` ban, scope-depth-aware `env_get_at` / `env_assign_at` | ✅ |
| 9 | Classes — class decls, `LoxClass` / `LoxInstance` pools, fields via `Map<string, LoxValue>`, methods, `this`, `init` (returns `this`, no explicit return value), bound-method creation | ✅ |
| 10 | Inheritance — `class A < B {…}` syntax, `LoxClass.superclass_id`, method lookup walks chain via `find_method`, `super.method` resolved via per-subclass env containing `super`, self-inheritance ban, super-outside-class / non-subclass bans | ✅ |

**Test coverage** — `test.roxy` holds 359 Roxy-level assertions (scanner/parser/evaluator/runner/closures/resolver/classes/inheritance); `tests/e2e/test_lox.cpp` wraps them as a single doctest case. All pass. End-to-end `.lox` scripts exercised via `./build/roxy examples/lox/main.roxy <script.lox>`: Fibonacci, factorial, mutual recursion, closure-based counters, the Ch. 11 `showA`/shadowing example, a class-based bank-account program, and a `Shape → Circle/Square` inheritance program using `super.init`.

**Phase 7/8/9/10 design notes**:

- *Flat function-body table.* `SFunDecl` no longer stores `params`/`body` inline; instead it carries a `fun_body_id: i32` into a flat `List<uniq FunBody>`. The parser fills the list during parsing (as a `Parser.fun_bodies` field); the Interpreter takes ownership at construction time via a field-move out of a by-value Parser parameter. This is what makes nested-`fun` work: a nested SFunDecl can live inside another function's body without the Interpreter needing to navigate arbitrary paths into the parent AST. Each `LoxFunction` stores a `fun_body_id` + the captured `closure_env`; at call time we look up `self.fun_bodies[id].params` / `.body`.
- *Environment-as-pool, non-popping.* Environments live in `Interpreter.envs: List<Environment>` with an `enclosing: i32` parent index. Function calls and blocks push a new env but **never pop** — any closure created inside the scope may escape (returned, stored in a map, etc.) and still reference that env's index. This mirrors jlox's behavior where envs are reachable via GC for as long as they're referenced; in our pool, it just means the pool grows monotonically per script, which is fine for the book-style interpreter.
- *Resolver runs between parse and run.* Methods live in `resolver.roxy` but are defined on `Interpreter` (so they can reuse the same `self.stmts[i]` / `self.fun_bodies[i]` borrow pattern the interpreter uses at runtime). Resolver state (`resolver_scopes`, `resolver_current_function`, `resolver_current_class`) is a set of transient `resolver_*` fields on the Interpreter, clobbered at each `resolve_program()` call. Output goes into `Interpreter.locals: Map<i32, i32>` keyed by `Expr.expr_id`; `lookup_variable` and `EAssign` branch on whether the id has a recorded distance.

- *Classes share the closure/env infrastructure.* A `LoxClass` is a name plus `methods: Map<string, i32>` (name → index into `Interpreter.functions`). Each method is a plain `LoxFunction` whose `closure_env` is whatever env was active at class-decl-exec time. Instance fields live in `LoxInstance.fields: Map<string, LoxValue>`, and the interpreter keeps all instances in a flat `instances: List<LoxInstance>` so `VInstance(id)` values are cheap to copy (they're just indices). Accessing a method via `obj.method` calls `bind_method`, which pushes a new env containing `this`, wraps the method's original closure, and registers a fresh `LoxFunction` entry pointing at the same body but closing over the wrapper env — exactly the pattern from `LoxFunction.bind()` in jlox. `init` methods are tagged with `is_initializer = true` so `call_fun` can fetch `this` out of the wrapper env and return it regardless of what the body did. The resolver opens a `this`-scope around every method and bans `return <value>` inside `init` and all uses of `this` outside a class.

- *ESet wraps its Get target.* In the book (Java) `parse_assignment` destructures a `Expr.Get` into `(object, name)` and builds an `Expr.Set(object, name, value)`. Roxy's move analyzer rejects moving a uniq field out of a uniq parent, so `ESet` instead carries `set_target: uniq Expr` (always of kind `EGet`) plus `set_value`; the interpreter reads `set_target.get_object` / `get_name` at eval time. Same runtime semantics, no destructuring needed.

- *Inheritance slots naturally into the existing infrastructure.* `LoxClass` gains `superclass_id: i32` (-1 for no super). `find_method` walks the chain — subclasses shadow same-named methods. For `super.method`, each subclass's methods close over a dedicated env holding `super → VClass(super_id)`, pushed at class-decl-exec time. At eval time, `ESuper` uses the resolver-recorded distance to reach that env (`distance` → `super`) and the wrapper `this` one frame closer (`distance - 1` → `this`), then calls `find_method` on the super class and binds the result.

**Bugs surfaced along the way** (the ones already fixed in `TODO.md` High Priority — what blocked each phase):
- f-string interpolation of `Printable` struct rvalues
- `if`/`when`/try-catch local-state restoration across branches (three variants)
- struct-typed field assignment copying pointer bits instead of contents
- cross-module non-pub function name collisions
- `BCExceptionHandler` single-range-assumption breaking try/catch-around-a-loop after RPO block reordering
- 1-slot integer field loads not sign-extending
- `Map<K, struct V>` storing values as dangling stack pointers
- variant-field visibility across modules
- constructor-field assignment double-free for uniq and variant-uniq fields
- Roxy's own `run()` took a noncopyable `List<uniq Stmt>` by move and mispassed it to throwing call paths

Plus two **new** compiler bugs surfaced during Phase 8 (both now fixed upstream, see `TODO.md`):
- `inout List<uniq T>` was incorrectly consumed by the call-arg move check in the semantic analyzer, and the IR builder had matching parallel bugs on owned-local tracking, post-call nullification, and loop-header phi merging.
- Moving a noncopyable field out of a by-value struct parameter was permitted by `check_not_field_move` but the IR builder wasn't null-ing the source field, so the by-value parameter's scope-exit destructor double-freed the slot.

Almost every phase hit at least one of these before progressing. Since each fix is upstream, future phases shouldn't re-hit them.

## Original Plan



## Overview

Lox is a dynamically-typed scripting language with:
- Dynamic types: nil, booleans, numbers (f64), strings
- Expressions: arithmetic, comparison, logical, grouping
- Variables with lexical scoping
- Control flow: if/else, while, for
- First-class functions with closures
- Classes with single inheritance, constructors (`init`), `this`, `super`
- A single built-in: `clock()`

We'll implement a full tree-walk interpreter: **Scanner -> Parser -> Resolver -> Interpreter**.

## Key Design Decisions

### Dynamic Values via Tagged Union

Lox is dynamically typed, so we need a `LoxValue` tagged union to represent all runtime values:

```roxy
enum ValueKind { Nil, Bool, Number, Str, Fun, NativeFun, Class, Instance }

struct LoxValue {
    when kind: ValueKind {
        case Nil: _pad: i32;
        case Bool: bool_val: bool;
        case Number: num_val: f64;
        case Str: str_val: string;
        case Fun: fun_id: i32;            // index into function table
        case NativeFun: native_id: i32;   // ID for dispatch in call_native()
        case Class: class_id: i32;        // index into class table
        case Instance: instance_id: i32;  // index into instance pool
    }
}
```

### Recursive AST via `uniq` Pointers

With recursive type support, AST nodes can directly contain their children via `uniq` pointers. This is much more natural than index-based arenas and closely mirrors the Java implementation:

```roxy
struct Expr {
    when kind: ExprKind {
        case Binary:
            left: uniq Expr;       // direct child, not an index
            op: TokenType;
            right: uniq Expr;
        case Literal:
            value: LoxValue;
        // ...
    }
}
```

`uniq` is nullable, so optional children (like the else-branch of an if statement) naturally use `nil`.

### Environments as Indexed Pool

Lox environments need shared ownership: multiple closures can capture the same environment, and child environments reference their parent. Since `ref` cannot be used in struct fields, environments live in a `List<Environment>` pool managed by the interpreter, with parent references by index:

```roxy
struct Environment {
    values: Map<string, LoxValue>;
    enclosing: i32;   // -1 = none, otherwise index into env pool
}
```

This is the one place where we can't use recursive `uniq` — shared ownership requires indirection through the interpreter.

### Callables via Function Table and Native IDs

Roxy doesn't have closures or first-class functions. Lox functions are modeled as data in a function table, with dispatch handled manually by the interpreter:

```roxy
struct LoxFunction {
    name: string;
    // References the AST by storing an index, since the AST
    // is owned by the program's statement list.
    decl_index: i32;               // index into program's stmt list
    param_count: i32;
    closure_env: i32;              // captured environment index
    is_initializer: bool;
}
```

Native functions (like `clock`) use a separate `NativeCallable` variant in `LoxValue`. Each native is identified by an ID, and the interpreter dispatches on it:

```roxy
enum ValueKind { Nil, Bool, Number, Str, Fun, NativeFun, Class, Instance }

// In the LoxValue tagged union:
case NativeFun: native_id: i32;   // ID for dispatch in call_native()

fun call_native(interp: ref Interpreter, native_id: i32, args: List<LoxValue>): LoxValue {
    if (native_id == 0) {
        // clock()
        return make_number(clock());
    }
    // ... other natives
}
```

### Pattern Matching Instead of Visitor

Lox (Java) uses the Visitor pattern. In Roxy, we use `when` to dispatch on tagged union variants:

```roxy
fun eval_expr(interp: ref Interpreter, expr: ref Expr): LoxValue {
    when expr.kind {
        case Binary: return eval_binary(interp, expr);
        case Unary:  return eval_unary(interp, expr);
        case Literal: return expr.value;
        // ...
    }
}
```

## Prerequisites: New Native Functions

The current Roxy string API (`str_concat`, `str_eq`, `str_len`, `print`) is insufficient for writing a scanner. We need to add these native functions first:

| Function | Signature | Purpose |
|----------|-----------|---------|
| `str_char_at` | `(s: string, i: i32): i32` | Get ASCII code of character at index |
| `str_substr` | `(s: string, start: i32, len: i32): string` | Extract substring |
| `str_to_f64` | `(s: string): f64` | Parse string to number |
| `str_from_code` | `(code: i32): string` | Create 1-char string from ASCII code |
| `clock` | `(): f64` | Current time in seconds (for Lox's built-in) |
| `read_file` | `(path: string): string` | Read file contents as string |

`i32_to_string` and `f64_to_string` already exist via the `Printable` trait (usable in f-strings).

These are small additions to `src/roxy/vm/natives.cpp`.

## Phases

### Phase 1: Token Types and Scanner (Ch. 4)

**Goal:** Tokenize Lox source code into a list of tokens.

**Files:** `examples/lox/tokens.roxy`, `examples/lox/scanner.roxy`

**Token types (enum):**
```roxy
enum TokenType {
    // Single-character tokens
    LeftParen, RightParen, LeftBrace, RightBrace,
    Comma, Dot, Minus, Plus, Semicolon, Slash, Star,

    // One or two character tokens
    Bang, BangEqual, Equal, EqualEqual,
    Greater, GreaterEqual, Less, LessEqual,

    // Literals
    Identifier, StringTok, Number,

    // Keywords
    And, Class, Else, False, Fun, For, If, Nil,
    Or, Print, Return, Super, This, True, Var, While,

    Eof
}
```

**Token struct:**
```roxy
struct Token {
    type: TokenType;
    lexeme: string;
    line: i32;
    // Literal value stored separately - number_val for Number, string_val for String
    has_string_literal: bool;
    string_literal: string;
    has_number_literal: bool;
    number_literal: f64;
}
```

**Scanner:** Reads source string character-by-character using `str_char_at`. Produces `List<Token>`. Handles:
- Single/two-char tokens
- String literals (with error on unterminated)
- Number literals (integer and decimal)
- Identifiers and keyword lookup (via `Map<string, TokenType>`)
- Comments (`//` to end of line)
- Whitespace skipping, line counting

**Lox vs Roxy mapping:**
| Lox (Java) | Roxy |
|------------|------|
| `List<Token>` | `List<Token>` |
| `HashMap` for keywords | `Map<string, TokenType>` |
| `char` operations | `str_char_at()` returning `i32`, compare with ASCII codes |
| `substring()` | `str_substr()` |
| `Double.parseDouble()` | `str_to_f64()` |

---

### Phase 2: AST Representation (Ch. 5)

**Goal:** Define expression and statement types as recursive tagged unions.

**Files:** `examples/lox/ast.roxy`

With recursive types, AST nodes own their children via `uniq` pointers. This eliminates the need for arena allocation and integer indices — the tree structure is direct and natural.

**Expression types (12):**
```roxy
enum ExprKind {
    Assign, Binary, Call, Get, Grouping,
    Literal, Logical, Set, Super, This, Unary, Variable
}

struct Expr {
    when kind: ExprKind {
        case Assign:
            assign_name: string;
            assign_value: uniq Expr;
        case Binary:
            binary_left: uniq Expr;
            binary_op: TokenType;
            binary_right: uniq Expr;
        case Call:
            call_callee: uniq Expr;
            call_args: List<uniq Expr>;
        case Get:
            get_object: uniq Expr;
            get_name: string;
        case Grouping:
            group_expr: uniq Expr;
        case Literal:
            literal_value: LoxValue;
        case Logical:
            logical_left: uniq Expr;
            logical_op: TokenType;
            logical_right: uniq Expr;
        case Set:
            set_object: uniq Expr;
            set_name: string;
            set_value: uniq Expr;
        case Super:
            super_method: string;
        case This: _this_pad: i32;
        case Unary:
            unary_op: TokenType;
            unary_operand: uniq Expr;
        case Variable:
            var_name: string;
    }
    expr_id: i32;   // unique id for resolver lookups
}
```

**Statement types (9):**
```roxy
enum StmtKind {
    Block, ClassDecl, ExprStmt, FunDecl,
    IfStmt, PrintStmt, ReturnStmt, VarDecl, WhileStmt
}

struct Stmt {
    when kind: StmtKind {
        case Block:
            block_stmts: List<uniq Stmt>;
        case ClassDecl:
            class_name: string;
            class_superclass: uniq Expr;   // nil if no superclass
            class_methods: List<uniq Stmt>;
        case ExprStmt:
            expr_stmt: uniq Expr;
        case FunDecl:
            fun_name: string;
            fun_params: List<string>;
            fun_body: List<uniq Stmt>;
        case IfStmt:
            if_condition: uniq Expr;
            if_then: uniq Stmt;
            if_else: uniq Stmt;            // nil if no else branch
        case PrintStmt:
            print_expr: uniq Expr;
        case ReturnStmt:
            return_value: uniq Expr;       // nil if bare return
        case VarDecl:
            var_decl_name: string;
            var_decl_init: uniq Expr;      // nil if no initializer
        case WhileStmt:
            while_condition: uniq Expr;
            while_body: uniq Stmt;
    }
}
```

Compared to the index-based approach, this is:
- **More readable** — `left: uniq Expr` is clearer than `left: i32`
- **Type-safe** — no possibility of using a wrong index or indexing the wrong arena
- **Closer to the book** — the Java AST uses direct object references too
- **No arena bookkeeping** — ownership is automatic via `uniq` RAII

Optional fields use `nil` naturally: `if_else: uniq Stmt` is `nil` when there's no else branch.

---

### Phase 3: Parser (Ch. 6)

**Goal:** Recursive descent parser producing AST from tokens.

**Files:** `examples/lox/parser.roxy`

**Parser struct:**
```roxy
struct Parser {
    tokens: List<Token>;
    current: i32;
    next_expr_id: i32;   // counter for unique expr IDs
    had_error: bool;
}
```

Note: the parser no longer needs an `AstArena`. It returns `uniq Expr` and `uniq Stmt` directly — each parse function allocates and returns an owned AST node.

**Operator precedence (lowest to highest):**
1. Assignment (`=`)
2. Or (`or`)
3. And (`and`)
4. Equality (`==`, `!=`)
5. Comparison (`<`, `<=`, `>`, `>=`)
6. Addition (`+`, `-`)
7. Multiplication (`*`, `/`)
8. Unary (`!`, `-`)
9. Call (`.`, `()`)
10. Primary (literals, grouping, identifiers)

**Key functions:**
```roxy
fun parse(parser: ref Parser): List<uniq Stmt>
fun declaration(parser: ref Parser): uniq Stmt
fun statement(parser: ref Parser): uniq Stmt
fun expression(parser: ref Parser): uniq Expr
fun assignment(parser: ref Parser): uniq Expr
// ... down to:
fun primary(parser: ref Parser): uniq Expr
```

Each function returns an owned `uniq` pointer to the constructed node.

**Error recovery:** On parse error, synchronize by advancing to the next statement boundary (`;` or keyword).

**`for` desugaring:** Transform `for` loops into `while` loops in the parser, just like jlox.

---

### Phase 4: Expression Interpreter (Ch. 7)

**Goal:** Evaluate expressions to produce `LoxValue` results.

**Files:** `examples/lox/interpreter.roxy`

**Interpreter struct:**
```roxy
struct Interpreter {
    envs: List<Environment>;       // environment pool (shared ownership)
    globals_env: i32;              // index of global environment
    current_env: i32;              // index of current environment
    functions: List<LoxFunction>;  // function table
    classes: List<LoxClass>;       // class table
    instances: List<LoxInstance>;   // instance pool
    locals: Map<i32, i32>;         // expr_id -> scope depth (from resolver)
    had_runtime_error: bool;
}
```

**Expression evaluation:**
- `Literal` -> return the value directly
- `Grouping` -> evaluate inner expression
- `Unary` -> `-` (negate number), `!` (negate truthiness)
- `Binary` -> arithmetic (`+`, `-`, `*`, `/`), comparison (`<`, `<=`, `>`, `>=`), equality (`==`, `!=`), string concatenation (`+` on strings)
- `Variable` -> look up in environment
- `Assign` -> evaluate value, store in environment

**Truthiness:** `nil` and `false` are falsy, everything else is truthy.

**Runtime errors:** Type mismatches (e.g., `-"hello"`) produce error messages. Use Roxy's exception handling (`try`/`catch`/`throw`) for `RuntimeError`:

```roxy
struct RuntimeError {
    message_str: string;
    line: i32;
}
fun RuntimeError.message(): string for Exception {
    return f"[line {self.line}] {self.message_str}";
}
```

---

### Phase 5: Statements and State (Ch. 8)

**Goal:** Execute statements, manage variables and scoping.

**Key functionality:**
- `ExprStmt` -> evaluate and discard result
- `PrintStmt` -> evaluate and print (using `lox_stringify`)
- `VarDecl` -> evaluate initializer (or `nil`), define in current environment
- `Block` -> create new environment, execute statements, restore environment
- `Assign` / `Variable` -> get/set in environment chain

**Environment operations:**
```roxy
fun env_define(interp: ref Interpreter, name: string, value: LoxValue)
fun env_get(interp: ref Interpreter, name: string): LoxValue
fun env_assign(interp: ref Interpreter, name: string, value: LoxValue)
fun env_get_at(interp: ref Interpreter, distance: i32, name: string): LoxValue
fun env_assign_at(interp: ref Interpreter, distance: i32, name: string, value: LoxValue)
```

---

### Phase 6: Control Flow (Ch. 9)

**Goal:** if/else, while, for loops, logical operators.

**Key functionality:**
- `IfStmt` -> evaluate condition, execute then or else branch (check `if_else != nil`)
- `WhileStmt` -> loop while condition is truthy
- `Logical` (`and`/`or`) -> short-circuit evaluation returning the determining value (not just bool)
- `for` -> already desugared to `while` in parser

---

### Phase 7: Functions (Ch. 10)

**Goal:** Function declarations, calls, closures, return statements.

**LoxFunction** (same struct as in Key Design Decisions):
```roxy
struct LoxFunction {
    name: string;
    decl_index: i32;               // index into program's stmt list
    param_count: i32;
    closure_env: i32;              // captured environment index
    is_initializer: bool;
}
```

**Call mechanism:**
1. Evaluate callee expression -> must be `LoxValue` of kind `Fun`, `NativeFun`, or `Class`
2. For `NativeFun`: dispatch via `call_native(interp, native_id, args)`
3. For `Fun`: evaluate arguments, check arity, create new environment (enclosing = closure env), bind parameters, execute body
4. For `Class`: create instance, call `init` if present
5. Catch `ReturnException` to get return value from user functions

**Return:** Use Roxy's `throw`/`catch` mechanism to implement return as an exception:
```roxy
struct ReturnException {
    value: LoxValue;
}
fun ReturnException.message(): string for Exception {
    return "return";
}
```

**Built-in `clock()`:** Registered at interpreter startup by defining `clock` in the global environment as a `NativeFun` value with `native_id = 0`.

**Lox closures via environment capture:** When a Lox function declaration is executed, its `closure_env` is set to the current environment index. This captures the environment at definition time. When called, a new environment is created with `enclosing = closure_env`, exactly matching jlox's behavior. Note: this models Lox's closure semantics using Roxy's data structures — no Roxy-level closures are needed.

---

### Phase 8: Resolver (Ch. 11)

**Goal:** Static variable resolution pass (pre-interpreter).

**Files:** `examples/lox/resolver.roxy`

**Resolver struct:**
```roxy
enum FunctionType { FunNone, FunFunction, FunInitializer, FunMethod }
enum ClassType { ClassNone, ClassClass, ClassSubclass }

struct Resolver {
    interp: ref Interpreter;
    scopes: List<Map<string, bool>>;  // stack of scopes
    current_function: FunctionType;
    current_class: ClassType;
}
```

**Purpose:** Walk the AST (now a real tree of `uniq` nodes) and for each variable reference, compute how many scopes away the definition is. Store this distance in `Interpreter.locals[expr_id]`.

The resolver traverses the tree via `ref` — it borrows the AST without consuming it:

```roxy
fun resolve_expr(resolver: ref Resolver, expr: ref Expr) {
    when expr.kind {
        case Binary:
            resolve_expr(resolver, expr.binary_left);
            resolve_expr(resolver, expr.binary_right);
        case Variable:
            resolve_local(resolver, expr, expr.var_name);
        // ...
    }
}
```

**Key checks:**
- No reading a variable in its own initializer
- No `return` at top level
- No `this` outside a class
- No `super` outside a subclass
- No self-inheritance

---

### Phase 9: Classes (Ch. 12)

**Goal:** Class declarations, instances, properties, methods.

**LoxClass:**
```roxy
struct LoxClass {
    name: string;
    superclass_id: i32;           // -1 if none
    methods: Map<string, i32>;    // method name -> function table index
}
```

**LoxInstance:**
```roxy
struct LoxInstance {
    class_id: i32;
    fields: Map<string, LoxValue>;
}
```

**Key functionality:**
- `ClassDecl` -> create LoxClass, store methods, define in environment
- Calling a class (kind `Class`) -> create LoxInstance, call `init` if present
- `Get` expression -> check instance fields first, then class methods (bind `this`)
- `Set` expression -> set field on instance
- `This` -> look up in environment (bound during method call)

**Method binding:** When a method is accessed via `Get`, create a new LoxFunction entry with `this` bound in a new environment wrapping the method's closure.

---

### Phase 10: Inheritance (Ch. 13)

**Goal:** Superclass, `super` keyword.

**Key functionality:**
- `ClassDecl` with superclass -> evaluate superclass (must be a class), store in LoxClass
- Method lookup walks superclass chain via `superclass_id`
- `super.method()` -> look up method in superclass, bind to current instance

**Super environment:** When a class has a superclass, create an environment containing `super` bound to the superclass. Methods close over this environment (captured via `closure_env`).

---

## File Structure

```
examples/lox/
├── main.roxy          # Entry point: read file, run pipeline
├── tokens.roxy        # TokenType enum, Token struct
├── scanner.roxy       # Scanner: source string -> List<Token>
├── ast.roxy           # ExprKind, StmtKind, Expr, Stmt, FunBody
├── value.roxy         # LoxValue (and later LoxClass, LoxInstance)
├── parser.roxy        # Token list -> AST, with error recovery
├── interpreter.roxy   # Tree-walk interpreter (+ env pool, fn table, locals)
├── resolver.roxy      # Static variable resolution (methods on Interpreter)
└── test.roxy          # Roxy-level assertion suite
```

Environment lives inside `interpreter.roxy` rather than getting its own file — it's just a struct + 3 methods and only the interpreter touches it. The original plan's `value.roxy` is smaller than expected since function/class metadata also sits in `interpreter.roxy`.

> **Note:** Roxy's module system uses `import`/`from` for multi-file projects. If modules prove cumbersome, we can start with a single file and split later.

## Testing Strategy

For each phase, write small Lox programs as string literals and verify output:

```roxy
// Test: basic arithmetic
var source: string = "print 1 + 2 * 3;";
run(source);
// Expected output: 7
```

We can also create `.lox` test files and run them via the `read_file` native function:
```bash
./build/roxy examples/lox/main.roxy path/to/script.lox
```

(Requires adding command-line argument support or `read_file` + hardcoded path for initial testing.)

## Lox Feature vs Roxy Construct Mapping

| Lox Concept | Roxy Implementation |
|-------------|---------------------|
| Dynamic types | `LoxValue` tagged union |
| AST nodes (recursive tree) | Tagged union structs with `uniq` children |
| Optional AST children | `uniq Expr` / `uniq Stmt` set to `nil` |
| Visitor pattern | `when` pattern matching |
| `HashMap` | `Map<K, V>` |
| `ArrayList` | `List<T>` |
| Environment chain | `List<Environment>` pool with index-based parent refs |
| Closures (env capture) | `closure_env` index into environment pool |
| Exception for `return` | Roxy `throw`/`catch` with `ReturnException` |
| `Object` equality | Custom `lox_equal(a, b): bool` function |
| `toString` | Custom `lox_stringify(v: LoxValue): string` using f-strings |
| `instanceof` checks | `when` on `ValueKind` |
| Java GC | `uniq` ownership for AST; pool-based for environments/instances |
| Native functions | `NativeFun` variant + ID-based dispatch |

## Milestones

1. ✅ **Milestone 1 (Prereqs):** Add native string functions to Roxy runtime
2. ✅ **Milestone 2 (Scanner):** Tokenize Lox source into tokens
3. ✅ **Milestone 3 (Parser):** Parse tokens into recursive AST - test with AST printer
4. ✅ **Milestone 4 (Eval):** Evaluate expressions (calculator mode)
5. ✅ **Milestone 5 (Statements):** Variables, print, blocks, scoping
6. ✅ **Milestone 6 (Control Flow):** if/else, while, for, logical operators
7. ✅ **Milestone 7 (Functions):** Function declarations, calls, return — top-level + nested, real closures via captured env indices
8. ✅ **Milestone 8 (Resolution):** Static variable resolution pass; per-`expr_id` scope-depth; self-init and top-level-`return` checks
9. ✅ **Milestone 9 (Classes):** Class declarations, instances, fields, methods, `this`, `init`, bound methods
10. ✅ **Milestone 10 (Inheritance):** Superclass, `super`, method inheritance — Lox book Part II complete.
