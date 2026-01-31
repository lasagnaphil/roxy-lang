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