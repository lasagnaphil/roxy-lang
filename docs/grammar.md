# Roxy Grammar

```
program         -> declaration* EOF '
```

## Declarations

```
declaration     -> struct_decl
                 | fun_decl
                 | var_decl
                 | statement ;
struct_decl     -> "struct" Identifier ( ":" Identifier )?
                   "{" field_decl "}" ;
field_decl      -> Identifier ":" Identifier
fun_decl        -> "fun" function ;
var_decl        -> "var" typed_identifier ( "=" expression )? ";" ;
```

## Statements

```
statement       -> expr_stmt
                -> for_stmt
                -> if_stmt
                -> return_stmt
                -> while_stmt
                -> block ;
expr_stmt       -> expression ";" ;
for_stmt        -> "for" ( var_decl | expr_stmt | ";" )
                             expression? ";"
                             expression? statement ;
if_stmt         -> "if" expression statement
                   ( "else" statement )? ;
return_stmt     -> "return" expression? ";" ;
while_stmt      -> "while" expression statement ;
block           -> "{" declaration* "}"
```

## Expressions

```
expression      -> assignment ;

assignment      -> ( call "." )? Identifier "=" assignment
                -> logic_or ;

logic_or        -> logic_and ( "||" logic_and )* ;
logic_and       -> equality ( "&&" equality )* ;
equality        -> comparison ( ( "!=" | "==" ) comparison )* ;
comparison      -> term ( ( ">" | ">=" | "<" | "<=" ) term )* ;
term            -> factor ( ( "-" | "+" ) factor )* ;
factor          -> unary ( ( "/" | "*" ) unary )* ;

unary           -> ( "!" | "-" ) unary | call ;
call            -> primary ( "(" arguments? ")" | "." Identifier )* ;
primary         -> "true" | "false" | "nil" | "this"
                -> Number | String | Identifier | "(" expression ")"
                -> "super" "." Identifier ;
```

## Utility rules

```
function         -> Identifier "(" parameters? ")" block ;
parameters       -> typed_identifier ( "," typed_identifier )* ;
typed_identifier -> Identifier ( ":" Identifier )?
arguments        -> expression ( "," expression )* ;
```

## Lexical Grammar

```
Number          -> DIGIT+ ( "." DIGIT+ )? ;
String          -> "\"" <any char except "\"">* "\"" ;
Identifier      -> ALPHA ( ALPHA | DIGIT )* ;
ALPHA           -> "a" ... "z" | "A" ... "Z" | "_" ;
DIGIT           -> "0" ... "9" ;
```