# Roxy Grammar

```
program         -> declaration* EOF
```

## Declarations

```
declaration     -> struct_decl
                 | enum_decl
                 | trait_decl
                 | fun_decl
                 | method_decl
                 | constructor_decl
                 | destructor_decl
                 | var_decl
                 | import_decl
                 | statement ;

struct_decl     -> "struct" Identifier type_params? ( ":" Identifier )?
                   "{" ( field_decl | when_field_decl )* "}" ;
field_decl      -> ( "pub" )? Identifier ":" type_expr ( "=" expression )? ";" ;

// Tagged union: a discriminant field plus per-variant field groups.
when_field_decl -> "when" Identifier ":" Identifier
                   "{" ( "case" case_names ":" field_decl* )* "}" ;
case_names      -> Identifier ( "," Identifier )* ;

enum_decl       -> "enum" Identifier
                   "{" ( enum_variant ( "," enum_variant )* ","? )? "}" ;
enum_variant    -> Identifier ( "=" expression )? ;

// A trait declaration is a header; its required/default methods are declared
// separately as `fun TraitName.method(...)` (a body makes it a default method).
trait_decl      -> "trait" Identifier type_params? ( ":" Identifier )? ";" ;

fun_decl        -> ( "pub" )? ( "native" )? "fun" Identifier type_params?
                   "(" parameters? ")" ( ":" type_expr )?
                   ( block | ";" ) ;

// `for Trait` marks the method as that trait's implementation for the struct.
method_decl     -> ( "pub" )? "fun" Identifier type_args? "." Identifier
                   "(" parameters? ")" ( ":" type_expr )?
                   ( "for" Identifier type_args? )? block ;

constructor_decl -> ( "pub" )? "fun" "new" Identifier type_args? ( "." Identifier )?
                    "(" parameters? ")"
                    block ;

destructor_decl  -> "fun" "delete" Identifier type_args? ( "." Identifier )?
                    "(" parameters? ")"
                    block ;

var_decl        -> "var" typed_identifier ( "=" expression )? ";" ;

import_decl     -> "import" package_path ";"
                 | "from" package_path "import" import_list ";" ;
package_path    -> Identifier ( "." Identifier )* ;
import_list     -> "*" | Identifier ( "," Identifier )* ;

// Generic parameters, optionally with trait bounds: <T: Printable + Hash, U>
type_params     -> "<" type_param ( "," type_param )* ">" ;
type_param      -> Identifier ( ":" Identifier type_args?
                                    ( "+" Identifier type_args? )* )? ;
type_args       -> "<" type_expr ( "," type_expr )* ">" ;
```

A top-level `var_decl` is a **module global** (persistent storage, ordered
initialization, RAII teardown); inside a block it is an ordinary local.

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
                 | when_stmt
                 | try_stmt
                 | throw_stmt
                 | yield_stmt
                 | delete_stmt
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

// Pattern match on an enum value. Exhaustiveness is detected, not required.
when_stmt       -> "when" expression
                   "{" ( "case" case_names ":" statement* )*
                       ( "else" ":" statement* )? "}" ;

try_stmt        -> "try" block catch_clause* ( "finally" block )? ;
catch_clause    -> "catch" "(" Identifier ( ":" type_expr )? ")" block ;

throw_stmt      -> "throw" expression ";" ;
yield_stmt      -> "yield" expression? ";" ;

// Explicit destruction; the named form calls a named destructor.
delete_stmt     -> "delete" expression ";" ;

block           -> "{" declaration* "}"
```

A function is a **coroutine** if its body contains `yield` — there is no
separate declaration form; its return type is `Coro<T>`.

## Expressions

```
expression      -> assignment ;

assignment      -> assign_target assign_op assignment
                 | ternary ;
assign_target   -> ( call "." )? Identifier | call "[" expression "]" ;
assign_op       -> "=" | "+=" | "-=" | "*=" | "/=" | "%="
                 | "&=" | "|=" | "^=" | "<<=" | ">>=" ;

ternary         -> binary ( "?" ternary ":" ternary )? ;

// Binary operators are parsed by precedence climbing (all left-associative),
// tightest last:
//   1: ||      2: &&      3: |       4: ^       5: &
//   6: == !=   7: < <= > >=          8: << >>
//   9: + -    10: * / %
binary          -> unary ( binary_op unary )* ;

unary           -> ( "!" | "-" | "~" ) unary | alloc_expr ;
alloc_expr      -> "uniq" ( struct_literal | call ) | call ;
call            -> primary ( "(" arguments? ")"
                           | "[" expression "]"
                           | "." Identifier
                           | "::" Identifier )* ;
primary         -> "true" | "false" | "nil" | "self"
                 | Number | String | FString | "(" expression ")"
                 | "super" "." Identifier
                 | lambda
                 | struct_literal
                 | Identifier type_args? ;
struct_literal  -> Identifier type_args? "{" field_init_list? "}" ;
field_init_list -> field_init ( "," field_init )* ;
field_init      -> Identifier "=" expression ;

// Lambdas: a block body, or `=> expr` shorthand.
lambda          -> "fun" capture_list? "(" parameters? ")" ( ":" type_expr )?
                   ( block | "=>" expression ) ;
capture_list    -> "[" ( capture ( "," capture )* )? "]" ;
capture         -> "move" Identifier | "copy" "self" | "weak" "self" ;
```

`move` and `copy` are contextual keywords, recognized only inside a capture
list. Non-captured copyable values are captured implicitly by copy.

## Utility rules

```
typed_identifier -> Identifier ":" type_expr ;
parameters       -> parameter ( "," parameter )* ;
parameter        -> Identifier ":" ( "out" | "inout" )? type_expr ;
arguments        -> argument ( "," argument )* ;
argument         -> ( "out" | "inout" )? expression ;

type_expr        -> ( "uniq" | "ref" | "weak" )? ( named_type | fun_type ) ;
named_type       -> Identifier type_args? ;
fun_type         -> "fun" "(" ( type_expr ( "," type_expr )* )? ")" ( "->" type_expr )? ;
```

`<` in a type-argument position is disambiguated from the less-than operator by
a trial parse (`try_parse_generic_args`); `>>` closing two nested type-argument
lists is split by `consume_closing_angle`.

## Lexical Grammar

```
Number          -> ( DEC | HEX | BIN | OCT ) ( "u" | "l" | "ul" | "f" )? ;
DEC             -> DIGIT+ ( "." DIGIT+ )? ;
HEX             -> "0x" HEXDIGIT+ ;
BIN             -> "0b" ( "0" | "1" )+ ;
OCT             -> "0o" ( "0" ... "7" )+ ;
String          -> "\"" ( <any char except "\"" or "\\"> | escape )* "\"" ;
FString         -> "f" "\"" ( <char> | escape | "{" expression "}" )* "\"" ;
escape          -> "\\" ( "n" | "t" | "r" | "\\" | "\"" | "0" ) ;
Identifier      -> ALPHA ( ALPHA | DIGIT )* ;
ALPHA           -> "a" ... "z" | "A" ... "Z" | "_" ;
DIGIT           -> "0" ... "9" ;
HEXDIGIT        -> DIGIT | "a" ... "f" | "A" ... "F" ;
```

Comments are `//` to end of line and `/* … */`, which **nest**. An f-string's
`{expression}` parts are lexed as source and parsed as ordinary expressions;
each is converted with `to_string` via the `Printable` trait.

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