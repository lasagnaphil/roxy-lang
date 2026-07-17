; Roxy syntax highlighting.
; Ordered general -> specific: later patterns win.

(identifier) @variable

; ── Types ──────────────────────────────────────────────────────────────────

(type_identifier) @type
(primitive_type) @type.builtin

(struct_declaration
  name: (identifier) @type)

(enum_declaration
  name: (identifier) @type)

(trait_declaration
  name: (identifier) @type.interface)

(trait_clause
  trait: (identifier) @type.interface)

(struct_declaration
  parent: (identifier) @type)

(trait_declaration
  parent: (identifier) @type)

(type_parameter
  name: (identifier) @type)

(method_declaration
  type: (identifier) @type)

(constructor_declaration
  type: (identifier) @type)

(destructor_declaration
  type: (identifier) @type)

(struct_literal
  type: (identifier) @type)

(generic_struct_literal
  type: (identifier) @type)

(uniq_expression
  type: (identifier) @type)

(static_get_expression
  type: (identifier) @type)

(generic_constructor_call
  type: (identifier) @type)

; ── Properties and fields ──────────────────────────────────────────────────

(field_declaration
  name: (identifier) @property)

(field_expression
  field: (identifier) @property)

(field_initializer
  name: (identifier) @property)

(when_field_clause
  discriminant: (identifier) @property)

; ── Enum variants / when cases ─────────────────────────────────────────────

(enum_variant
  name: (identifier) @constant)

(static_get_expression
  member: (identifier) @constant)

(when_case
  case: (identifier) @constant)

(when_field_case
  case: (identifier) @constant)

; ── Functions ──────────────────────────────────────────────────────────────

(call_expression
  function: (identifier) @function)

(generic_call_expression
  function: (identifier) @function)

; a.b() — the method name in a call, matched structurally.
(call_expression
  function: (field_expression
    field: (identifier) @function.method))

(function_declaration
  name: (identifier) @function.definition)

(method_declaration
  name: (identifier) @function.method.definition)

(constructor_declaration
  name: (identifier) @function.definition)

(destructor_declaration
  name: (identifier) @function.definition)

(generic_constructor_call
  constructor: (identifier) @function)

(uniq_expression
  constructor: (identifier) @function)

(parameter
  name: (identifier) @variable.parameter)

(self) @variable.special

; ── Keywords ───────────────────────────────────────────────────────────────

[
  "var"
  "fun"
  "struct"
  "enum"
  "trait"
  "import"
  "from"
] @keyword

[
  "if"
  "else"
  "for"
  "while"
  "break"
  "continue"
  "return"
  "when"
  "case"
  "try"
  "catch"
  "throw"
  "finally"
  "yield"
] @keyword.control

[
  "pub"
  "native"
  "uniq"
  "ref"
  "weak"
  "out"
  "inout"
] @keyword

[
  "new"
  "delete"
  "super"
] @keyword

; `for` in a trait impl clause (`fun Vec2.add(..): Vec2 for Add`) is the same
; token as a for-loop, but it reads as a declaration keyword — which is how the
; VSCode/TextMate grammar scopes it too. Must come after @keyword.control.
(trait_clause
  "for" @keyword)

; ── Literals ───────────────────────────────────────────────────────────────

(number_literal) @number
(boolean_literal) @boolean
(nil) @constant.builtin
(string_literal) @string
(fstring) @string
(fstring_text) @string

; ── Comments ───────────────────────────────────────────────────────────────

[
  (line_comment)
  (block_comment)
] @comment

; ── Operators and punctuation ──────────────────────────────────────────────

[
  "+" "-" "*" "/" "%"
  "==" "!=" "<" "<=" ">" ">="
  "&&" "||" "!"
  "&" "|" "^" "~" "<<" ">>"
  "=" "+=" "-=" "*=" "/=" "%=" "&=" "|=" "^=" "<<=" ">>="
  "?" "=>" "->"
] @operator

[
  "(" ")"
  "[" "]"
  "{" "}"
] @punctuation.bracket

[
  ";"
  ","
  "."
  ":"
  "::"
] @punctuation.delimiter
