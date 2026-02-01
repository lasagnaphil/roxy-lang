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
type_expr        -> ( "uniq" | "ref" | "weak" )? Identifier ( "[" "]" )? ;
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

Roxy supports strict numeric typing with no implicit conversions.

| Literal | Type | Description |
|---------|------|-------------|
| `42` | `i32` | Default integer |
| `42u` | `u32` | Unsigned 32-bit |
| `42l` | `i64` | Signed 64-bit |
| `42ul` | `u64` | Unsigned 64-bit |
| `3.14` | `f64` | Default float |
| `3.14f` | `f32` | 32-bit float |

**Number bases:**
- Decimal: `42`
- Hexadecimal: `0xFF`
- Binary: `0b1010`
- Octal: `0o77`

**Strict typing rules:**
- No implicit conversions between numeric types
- Binary operators require matching types: `1 + 2l` is an error (i32 + i64)
- Use explicit suffixes to match types: `1l + 2l` works (i64 + i64)

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