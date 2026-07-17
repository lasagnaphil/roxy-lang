# Roxy Grammar

```
program         -> declaration* EOF
```

## Declarations

```
declaration     -> struct_decl
                 | fun_decl
                 | method_decl
                 | constructor_decl
                 | destructor_decl
                 | var_decl
                 | import_decl
                 | statement ;

struct_decl     -> "struct" Identifier ( ":" Identifier )?
                   "{" field_decl* "}" ;
field_decl      -> ( "pub" )? Identifier ":" type_expr ( "=" expression )? ";" ;

fun_decl        -> ( "pub" )? ( "native" )? "fun" Identifier
                   "(" parameters? ")" ( ":" type_expr )?
                   ( block | ";" ) ;

method_decl     -> ( "pub" )? "fun" Identifier "." Identifier
                   "(" parameters? ")" ( ":" type_expr )?
                   block ;

constructor_decl -> ( "pub" )? "fun" "new" Identifier ( "." Identifier )?
                    "(" parameters? ")"
                    block ;

destructor_decl  -> "fun" "delete" Identifier
                    "(" ")"
                    block ;

var_decl        -> "var" typed_identifier ( "=" expression )? ";" ;

import_decl     -> "import" package_path ";"
                 | "from" package_path "import" import_list ";" ;
package_path    -> Identifier ( "." Identifier )* ;
import_list     -> "*" | Identifier ( "," Identifier )* ;
```

### Scoping: no local shadowing

A local declaration — a `var`, a catch variable, or a lambda parameter — may
not reuse a name already bound to a variable or parameter of the enclosing
function (C#/Java-style rule). The ban crosses lambda boundaries: a lambda
body may not shadow a local of the function it appears in. Module-level names
remain shadowable — a local may reuse the name of a global, a function, or a
type — and sequential (non-overlapping) scopes may reuse names freely:

```roxy
var g: i32 = 1;

fun demo(n: i32) {
    var g: i32 = 2;        // OK: shadows a module-level global
    { var t: i32 = 1; }
    { var t: i32 = 2; }    // OK: previous t's scope has ended
    { var n: i32 = 3; }    // error: shadows parameter n
    var f = fun(): i32 {
        var g: i32 = 4;    // error: shadows the local g (crosses lambda boundary)
        return g;
    };
}
```

## Statements

```
statement       -> expr_stmt
                 | for_stmt
                 | if_stmt
                 | return_stmt
                 | while_stmt
                 | break_stmt
                 | continue_stmt
                 | block ;

expr_stmt       -> expression ";" ;

for_stmt        -> "for" "(" ( var_decl | expr_stmt | ";" )
                             expression? ";"
                             expression? ")" statement ;

if_stmt         -> "if" "(" expression ")" statement
                   ( "else" statement )? ;

return_stmt     -> "return" expression? ";" ;

while_stmt      -> "while" "(" expression ")" statement ;

break_stmt      -> "break" ";" ;

continue_stmt   -> "continue" ";" ;

block           -> "{" declaration* "}"
```

## Expressions

```
expression      -> assignment ;

assignment      -> ( call "." )? Identifier "=" assignment
                 | ternary ;

ternary         -> logic_or ( "?" ternary ":" ternary )? ;

logic_or        -> logic_and ( "||" logic_and )* ;
logic_and       -> equality ( "&&" equality )* ;
equality        -> comparison ( ( "!=" | "==" ) comparison )* ;
comparison      -> term ( ( ">" | ">=" | "<" | "<=" ) term )* ;
term            -> factor ( ( "-" | "+" ) factor )* ;
factor          -> unary ( ( "/" | "*" | "%" ) unary )* ;

unary           -> ( "!" | "-" | "~" ) unary | alloc_expr ;
alloc_expr      -> "uniq" ( struct_literal | call ) | call ;
call            -> primary ( "(" arguments? ")" | "." Identifier | "::" Identifier )* ;
primary         -> "true" | "false" | "nil" | "self"
                 | Number | String | "(" expression ")"
                 | "super" "." Identifier
                 | struct_literal
                 | Identifier ;
struct_literal  -> Identifier "{" field_init_list? "}" ;
field_init_list -> field_init ( "," field_init )* ;
field_init      -> Identifier "=" expression ;
```

## Utility rules

```
typed_identifier -> Identifier ":" type_expr ;
parameters       -> parameter ( "," parameter )* ;
parameter        -> Identifier ":" ( "out" | "inout" )? type_expr ;
type_expr        -> ( "uniq" | "ref" | "weak" )? Identifier ( "<" type_expr ( "," type_expr )* ">" )? ;
arguments        -> argument ( "," argument )* ;
argument         -> ( "out" | "inout" )? expression ;
```

## Lexical Grammar

```
Number          -> DIGIT+ ( "." DIGIT+ )? ( "u" | "l" | "ul" | "f" )? ;
String          -> "\"" <any char except "\"">* "\"" ;
Identifier      -> ALPHA ( ALPHA | DIGIT )* ;
ALPHA           -> "a" ... "z" | "A" ... "Z" | "_" ;
DIGIT           -> "0" ... "9" ;
```

## Numeric Literals

Roxy has strict numeric typing: **typed values never convert implicitly.** An
*unsuffixed* literal, though, has no type of its own yet — it adapts to whatever
its context asks for.

| Literal | Type | Description |
|---------|------|-------------|
| `42` | polymorphic, defaults to `i32` | Unsuffixed integer |
| `42u` | `u32` | Unsigned 32-bit |
| `42l` | `i64` | Signed 64-bit |
| `42ul` | `u64` | Unsigned 64-bit |
| `3.14` | polymorphic, defaults to `f64` | Unsuffixed float |
| `3.14f` | `f32` | 32-bit float |

**Number bases:**
- Decimal: `42`
- Hexadecimal: `0xFF`
- Binary: `0b1010`
- Octal: `0o77`

### Literal adaptation

An unsuffixed literal takes its type from context. An integer literal reaches
any numeric type, float included; a float literal reaches the float types, but
never an integer — adaptation never introduces a truncating conversion. A
literal keeps adapting through arithmetic, so an expression built only from
literals is itself still a literal:

```roxy
var a: i64 = 1;          // OK: adapts to i64
var b: i64 = 1 + 2;      // OK: still a literal, so it adapts as a whole
var c: i64 = 1 + 2l;     // OK: adapts to match the typed operand
var d: f64 = 1;          // OK: an integer literal reaches float types
var e: f32 = 3.14;       // OK: adapts to f32
var f: f64 = 1 + 2.0;    // OK: the integer literal adapts to the float

var g: i32 = 1.0;        // error: a float literal never becomes an integer
```

With no context to choose for it, a literal settles on its default — `i32` for
an integer, `f64` for a float:

```roxy
var x = 1;               // i32
var y = 1.0 + 2.0;       // f64
```

### Strict typing rules

Adaptation is a property of literals only. Once a value is typed, matching is
strict and mixing requires an explicit cast:

```roxy
var a: i32 = 1;
var b: i64 = 2l;
var c: i64 = a + b;      // error: arithmetic requires matching types
var d: i64 = i64(a) + b; // OK
```

## Type Casting

Explicit type conversions use constructor-like syntax:

```roxy
var x: i64 = 1000l;
var y: i32 = i32(x);      // Cast i64 to i32 (truncation)
var z: f64 = f64(42);     // Cast i32 to f64
var b: bool = bool(x);    // Cast integer to bool (0 = false, non-zero = true)
```

**Allowed conversions:**

| Source | Target | Behavior |
|--------|--------|----------|
| Any integer | Any integer | Truncation or sign/zero extension |
| Any integer | Any float | int-to-float conversion |
| Any float | Any integer | Truncation toward zero |
| f32 | f64 | Widening (lossless) |
| f64 | f32 | Narrowing (may lose precision) |
| Any integer/float | bool | Normalize: non-zero → true, zero → false |
| bool | Any integer/float | 0 or 1 |

**Disallowed:** `string` and `void` casts.