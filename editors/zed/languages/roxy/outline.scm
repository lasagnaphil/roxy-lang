(struct_declaration
  "struct" @context
  name: (identifier) @name) @item

(enum_declaration
  "enum" @context
  name: (identifier) @name) @item

(trait_declaration
  "trait" @context
  name: (identifier) @name) @item

(enum_variant
  name: (identifier) @name) @item

(field_declaration
  name: (identifier) @name) @item

(function_declaration
  "fun" @context
  name: (identifier) @name) @item

; fun Type.method(...) — show the receiver so the outline reads "fun Vec2.length"
(method_declaration
  "fun" @context
  type: (identifier) @context
  "." @context
  name: (identifier) @name) @item

(constructor_declaration
  "fun" @context
  "new" @context
  type: (identifier) @name) @item

(destructor_declaration
  "fun" @context
  "delete" @context
  type: (identifier) @name) @item
