/**
 * Tree-sitter grammar for Roxy.
 *
 * Modelled on src/roxy/compiler/parser.cpp, which is the ground truth —
 * docs/grammar.md lags the implementation in several places (it omits the
 * bitwise/shift precedence levels, claims `import *`, and gives destructors an
 * empty parameter list).
 *
 * Precedence numbers mirror get_infix_rule() in parser.cpp exactly. Note that
 * bitwise ops bind *looser* than equality here, unlike C.
 */

const PREC = {
  assign: 1,          // = += -= ... (right)
  ternary: 2,         // ?:          (right)
  or: 3,              // ||
  and: 4,             // &&
  bit_or: 5,          // |
  bit_xor: 6,         // ^
  bit_and: 7,         // &
  equality: 8,        // == !=
  comparison: 9,      // < <= > >=
  shift: 10,          // << >>
  additive: 11,       // + -
  multiplicative: 12, // * / %
  unary: 13,          // ! - ~ ref uniq
  postfix: 14,        // () [] . ::
  generic: 15,
};

const commaSep1 = (rule) => seq(rule, repeat(seq(',', rule)));
const commaSep = (rule) => optional(commaSep1(rule));

module.exports = grammar({
  name: 'roxy',

  extras: ($) => [/\s/, $.line_comment, $.block_comment],

  word: ($) => $.identifier,

  externals: ($) => [
    $.block_comment,
    $.string_literal,
    $._fstring_begin,
    $._fstring_mid,
    $._fstring_end,
    $._error_sentinel,
  ],

  conflicts: ($) => [
    // `ident <` is genuinely ambiguous: a comparison, or the head of a generic
    // call / named-ctor call / struct literal / value-position generic ref.
    // parser.cpp resolves it by trial parse (try_parse_generic_args), which
    // backtracks to comparison unless a '>' is found AND the follow token is
    // one of ( { . ; , ) ] } :. GLR explores the same alternatives; the generic
    // rules carry prec.dynamic so that when *both* parse (`f(a<b, c>(d))`) the
    // generic reading wins, matching the trial parse's commit rule — while a
    // plain `j < high;`, which has no valid generic parse, falls back to
    // comparison on its own.
    [
      $._primary_expression,
      $.generic_call_expression,
      $.generic_constructor_call,
      $.generic_struct_literal,
      $.identifier_type_args,
    ],
    // `Pair<i32>.from(..)` (named ctor) vs `identity<i32>` followed by a field
    // access. parser.cpp:769 commits a '.' after type args to the named-ctor
    // form, and the value-position ref only permits ; , ) ] } : after it — so
    // the ctor reading wins here too.
    [$.generic_constructor_call, $.identifier_type_args],
    [$.generic_call_expression, $.identifier_type_args],
  ],

  supertypes: ($) => [$._expression, $._statement, $._declaration, $._type],

  rules: {
    source_file: ($) => repeat($._declaration),

    // ── Comments ──────────────────────────────────────────────────────────
    // block_comment is external: Roxy nests them (lexer.cpp skip_whitespace).
    line_comment: (_) => token(seq('//', /[^\n]*/)),

    // ── Declarations ──────────────────────────────────────────────────────

    _declaration: ($) =>
      choice(
        $.var_declaration,
        $.function_declaration,
        $.method_declaration,
        $.constructor_declaration,
        $.destructor_declaration,
        $.struct_declaration,
        $.enum_declaration,
        $.trait_declaration,
        $.import_declaration,
        $.from_import_declaration,
        $._statement,
      ),

    var_declaration: ($) =>
      seq(
        optional('pub'),
        'var',
        field('name', $.identifier),
        optional(seq(':', field('type', $._type))),
        optional(seq('=', field('value', $._expression))),
        ';',
      ),

    // fun name<T>(params): Ret { }   |   native fun name(params): Ret;
    function_declaration: ($) =>
      seq(
        optional('pub'),
        optional(field('native', 'native')),
        'fun',
        field('name', $.identifier),
        optional(field('type_parameters', $.type_parameters)),
        field('parameters', $.parameter_list),
        optional(seq(':', field('return_type', $._type))),
        choice(field('body', $.block), ';'),
      ),

    // fun Type.method(params): Ret for Trait { }
    // fun Type<T>.method(...) { }   — type params bind to the struct
    method_declaration: ($) =>
      seq(
        optional('pub'),
        optional(field('native', 'native')),
        'fun',
        field('type', $.identifier),
        optional(field('type_parameters', $.type_parameters)),
        '.',
        field('name', choice($.identifier, 'new', 'delete')),
        field('parameters', $.parameter_list),
        optional(seq(':', field('return_type', $._type))),
        optional($.trait_clause),
        choice(field('body', $.block), ';'),
      ),

    trait_clause: ($) =>
      seq('for', field('trait', $.identifier), optional($.type_arguments)),

    // fun new Type(params) { }  |  fun new Type<T>.named(params) { }
    constructor_declaration: ($) =>
      seq(
        optional('pub'),
        'fun',
        'new',
        field('type', $.identifier),
        optional(field('type_parameters', $.type_parameters)),
        optional(seq('.', field('name', $.identifier))),
        field('parameters', $.parameter_list),
        field('body', $.block),
      ),

    // Destructors take parameters in the parser (parse_ctor_dtor_common),
    // despite docs/grammar.md showing "(" ")".
    destructor_declaration: ($) =>
      seq(
        optional('pub'),
        'fun',
        'delete',
        field('type', $.identifier),
        optional(field('type_parameters', $.type_parameters)),
        optional(seq('.', field('name', $.identifier))),
        field('parameters', $.parameter_list),
        field('body', $.block),
      ),

    struct_declaration: ($) =>
      seq(
        optional('pub'),
        'struct',
        field('name', $.identifier),
        optional(field('type_parameters', $.type_parameters)),
        optional(seq(':', field('parent', $.identifier))),
        field('body', $.struct_body),
      ),

    // Inline `fun` members are accepted by parser.cpp (1816) but are not a
    // working feature: sema never registers them, so calling one is a "no
    // member" error and merely declaring one crashes IR generation. Kept here
    // so such code still highlights and the LSP reports the real diagnostic,
    // rather than degenerating into an ERROR node.
    struct_body: ($) =>
      seq(
        '{',
        repeat(choice($.field_declaration, $.when_field_clause, $.function_declaration)),
        '}',
      ),

    field_declaration: ($) =>
      seq(
        optional('pub'),
        field('name', $.identifier),
        ':',
        field('type', $._type),
        optional(seq('=', field('default', $._expression))),
        ';',
      ),

    // Tagged union: when kind: Enum { case A: f: T; ... }
    when_field_clause: ($) =>
      seq(
        'when',
        field('discriminant', $.identifier),
        ':',
        field('type', $._type),
        '{',
        repeat($.when_field_case),
        '}',
      ),

    when_field_case: ($) =>
      seq('case', commaSep1(field('case', $.identifier)), ':', repeat($.field_declaration)),

    enum_declaration: ($) =>
      seq(
        optional('pub'),
        'enum',
        field('name', $.identifier),
        '{',
        commaSep($.enum_variant),
        optional(','),
        '}',
      ),

    enum_variant: ($) =>
      seq(field('name', $.identifier), optional(seq('=', field('value', $._expression)))),

    // Traits never have a body — parser.cpp consumes ';' unconditionally.
    trait_declaration: ($) =>
      seq(
        optional('pub'),
        'trait',
        field('name', $.identifier),
        optional(field('type_parameters', $.type_parameters)),
        optional(seq(':', field('parent', $.identifier))),
        ';',
      ),

    import_declaration: ($) => seq('import', field('module', $.module_path), ';'),

    // `as` is a contextual keyword (matched by text in import_declaration).
    from_import_declaration: ($) =>
      seq('from', field('module', $.module_path), 'import', commaSep1($.import_name), ';'),

    import_name: ($) =>
      seq(field('name', $.identifier), optional(seq($._as, field('alias', $.identifier)))),

    _as: (_) => token(prec(-1, 'as')),

    module_path: ($) => seq($.identifier, repeat(seq('.', $.identifier))),

    // ── Parameters and type parameters ────────────────────────────────────

    parameter_list: ($) => seq('(', commaSep($.parameter), ')'),

    parameter: ($) =>
      seq(
        field('name', $.identifier),
        ':',
        optional(field('modifier', choice('out', 'inout'))),
        field('type', $._type),
      ),

    type_parameters: ($) => seq('<', commaSep1($.type_parameter), '>'),

    type_parameter: ($) =>
      seq(field('name', $.identifier), optional(seq(':', $.trait_bounds))),

    trait_bounds: ($) => seq($._type, repeat(seq('+', $._type))),

    type_arguments: ($) => seq('<', commaSep1($._type), '>'),

    // ── Types ─────────────────────────────────────────────────────────────

    _type: ($) => choice($.reference_type, $._base_type),

    _base_type: ($) =>
      choice($.function_type, $.generic_type, $.primitive_type, $.type_identifier),

    // [borrowed] [uniq|ref|weak] base
    //
    // type_expression() checks `borrowed` and the ref kind as two *independent*
    // optional prefixes, so `borrowed i32` (no ref kind) and `ref T` (no
    // borrowed) are both valid, as is `borrowed uniq Point`.
    reference_type: ($) =>
      prec.right(
        seq(
          choice(
            seq($._borrowed, optional(field('ref_kind', choice('uniq', 'ref', 'weak')))),
            field('ref_kind', choice('uniq', 'ref', 'weak')),
          ),
          $._base_type,
        ),
      ),

    // `borrowed` is a soft keyword: it must out-rank type_identifier here (both
    // match the text in type position), but it stays a usable identifier
    // everywhere else because this token simply isn't valid outside a type.
    _borrowed: (_) => token(prec(1, 'borrowed')),

    function_type: ($) =>
      prec.right(
        seq('fun', '(', commaSep($._type), ')', optional(seq('->', field('return_type', $._type)))),
      ),

    generic_type: ($) =>
      seq(field('name', choice($.primitive_type, $.type_identifier)), $.type_arguments),

    primitive_type: (_) =>
      choice('i8', 'i16', 'i32', 'i64', 'u8', 'u16', 'u32', 'u64', 'f32', 'f64', 'bool', 'string', 'void'),

    type_identifier: ($) => alias($.identifier, $.type_identifier),

    // ── Statements ────────────────────────────────────────────────────────

    _statement: ($) =>
      choice(
        $.block,
        $.if_statement,
        $.while_statement,
        $.for_statement,
        $.return_statement,
        $.break_statement,
        $.continue_statement,
        $.delete_statement,
        $.when_statement,
        $.throw_statement,
        $.try_statement,
        $.yield_statement,
        $.expression_statement,
      ),

    block: ($) => seq('{', repeat($._declaration), '}'),

    if_statement: ($) =>
      prec.right(
        seq(
          'if',
          '(',
          field('condition', $._expression),
          ')',
          field('consequence', $._statement),
          optional(seq('else', field('alternative', $._statement))),
        ),
      ),

    while_statement: ($) =>
      seq('while', '(', field('condition', $._expression), ')', field('body', $._statement)),

    for_statement: ($) =>
      seq(
        'for',
        '(',
        field('initializer', choice($.var_declaration, $.expression_statement, ';')),
        field('condition', optional($._expression)),
        ';',
        field('increment', optional($._expression)),
        ')',
        field('body', $._statement),
      ),

    return_statement: ($) => seq('return', optional($._expression), ';'),
    break_statement: (_) => seq('break', ';'),
    continue_statement: (_) => seq('continue', ';'),
    delete_statement: ($) => seq('delete', $._expression, ';'),
    throw_statement: ($) => seq('throw', $._expression, ';'),
    yield_statement: ($) => seq('yield', optional($._expression), ';'),

    // `when x { ... }` — the discriminant suppresses struct literals
    // (m_suppress_struct_literal in parser.cpp), so a bare `Ident {` here opens
    // the when body rather than a struct literal.
    when_statement: ($) =>
      seq(
        'when',
        field('discriminant', $._expression_no_struct),
        '{',
        repeat($.when_case),
        optional($.when_else),
        '}',
      ),

    when_case: ($) =>
      seq('case', commaSep1(field('case', $.identifier)), ':', repeat($._declaration)),

    when_else: ($) => seq('else', ':', repeat($._declaration)),

    try_statement: ($) =>
      seq(
        'try',
        field('body', $.block),
        repeat($.catch_clause),
        optional($.finally_clause),
      ),

    catch_clause: ($) =>
      seq(
        'catch',
        '(',
        field('name', $.identifier),
        optional(seq(':', field('type', $._type))),
        ')',
        field('body', $.block),
      ),

    finally_clause: ($) => seq('finally', field('body', $.block)),

    expression_statement: ($) => seq($._expression, ';'),

    // ── Expressions ───────────────────────────────────────────────────────

    _expression: ($) =>
      choice(
        $.assignment_expression,
        $.ternary_expression,
        $.binary_expression,
        $.unary_expression,
        $.ref_expression,
        $.uniq_expression,
        $._primary_expression,
      ),

    // Discriminant of `when`. parser.cpp sets m_suppress_struct_literal across
    // this whole expression, so `when s {` opens the when body instead of
    // parsing `s { ... }` as a struct literal. Modelled as a self-contained
    // postfix chain that never reaches struct_literal — which covers every
    // discriminant the language can meaningfully match on (an enum-valued
    // expression) without reintroducing the `{` ambiguity.
    _expression_no_struct: ($) =>
      choice(
        $.identifier,
        $.self,
        $.static_get_expression,
        $.parenthesized_expression,
        alias($._ns_field_expression, $.field_expression),
        alias($._ns_call_expression, $.call_expression),
        alias($._ns_index_expression, $.index_expression),
      ),

    _ns_field_expression: ($) =>
      prec(
        PREC.postfix,
        seq(field('object', $._expression_no_struct), '.', field('field', $.identifier)),
      ),

    _ns_call_expression: ($) =>
      prec(
        PREC.postfix,
        seq(field('function', $._expression_no_struct), field('arguments', $.argument_list)),
      ),

    _ns_index_expression: ($) =>
      prec(
        PREC.postfix,
        seq(field('object', $._expression_no_struct), '[', field('index', $._expression), ']'),
      ),

    assignment_expression: ($) =>
      prec.right(
        PREC.assign,
        seq(
          field('left', $._expression),
          field(
            'operator',
            choice('=', '+=', '-=', '*=', '/=', '%=', '&=', '|=', '^=', '<<=', '>>='),
          ),
          field('right', $._expression),
        ),
      ),

    ternary_expression: ($) =>
      prec.right(
        PREC.ternary,
        seq(
          field('condition', $._expression),
          '?',
          field('consequence', $._expression),
          ':',
          field('alternative', $._expression),
        ),
      ),

    binary_expression: ($) => {
      const table = [
        ['||', PREC.or],
        ['&&', PREC.and],
        ['|', PREC.bit_or],
        ['^', PREC.bit_xor],
        ['&', PREC.bit_and],
        ['==', PREC.equality],
        ['!=', PREC.equality],
        ['<', PREC.comparison],
        ['<=', PREC.comparison],
        ['>', PREC.comparison],
        ['>=', PREC.comparison],
        ['<<', PREC.shift],
        ['>>', PREC.shift],
        ['+', PREC.additive],
        ['-', PREC.additive],
        ['*', PREC.multiplicative],
        ['/', PREC.multiplicative],
        ['%', PREC.multiplicative],
      ];
      return choice(
        ...table.map(([operator, precedence]) =>
          prec.left(
            precedence,
            seq(
              field('left', $._expression),
              field('operator', operator),
              field('right', $._expression),
            ),
          ),
        ),
      );
    },

    unary_expression: ($) =>
      prec.right(PREC.unary, seq(field('operator', choice('!', '-', '~')), $._expression)),

    ref_expression: ($) => prec.right(PREC.unary, seq('ref', $._expression)),

    // uniq Type(...) | uniq Type.ctor(...) | uniq Type { ... }
    uniq_expression: ($) =>
      prec.right(
        PREC.unary,
        seq(
          'uniq',
          field('type', $.identifier),
          optional(field('type_arguments', $.type_arguments)),
          choice(
            seq(optional(seq('.', field('constructor', $.identifier))), field('arguments', $.argument_list)),
            field('fields', $.field_initializer_list),
          ),
        ),
      ),

    _primary_expression: ($) =>
      choice(
        $.number_literal,
        $.string_literal,
        $.fstring,
        $.boolean_literal,
        $.nil,
        $.self,
        $.super_expression,
        $.lambda_expression,
        $.parenthesized_expression,
        $.call_expression,
        $.generic_call_expression,
        $.generic_constructor_call,
        $.struct_literal,
        $.generic_struct_literal,
        $.identifier_type_args,
        $.static_get_expression,
        $.field_expression,
        $.index_expression,
        $.identifier,
      ),

    parenthesized_expression: ($) => seq('(', $._expression, ')'),

    call_expression: ($) =>
      prec(PREC.postfix, seq(field('function', $._expression), field('arguments', $.argument_list))),

    // identity<i32>(42)
    generic_call_expression: ($) =>
      prec.dynamic(
        PREC.generic,
        seq(
          field('function', $.identifier),
          field('type_arguments', $.type_arguments),
          field('arguments', $.argument_list),
        ),
      ),

    // Pair<i32>.from(10, 32)
    generic_constructor_call: ($) =>
      prec.dynamic(
        PREC.generic,
        seq(
          field('type', $.identifier),
          field('type_arguments', $.type_arguments),
          '.',
          field('constructor', $.identifier),
          field('arguments', $.argument_list),
        ),
      ),

    // Value-position generic reference: `identity<i32>` followed by ; , ) ] } :
    identifier_type_args: ($) =>
      prec.dynamic(
        PREC.generic,
        seq(field('name', $.identifier), field('type_arguments', $.type_arguments)),
      ),

    argument_list: ($) => seq('(', commaSep($.argument), ')'),

    argument: ($) => seq(optional(field('modifier', choice('out', 'inout'))), $._expression),

    field_expression: ($) =>
      prec(PREC.postfix, seq(field('object', $._expression), '.', field('field', $.identifier))),

    index_expression: ($) =>
      prec(PREC.postfix, seq(field('object', $._expression), '[', field('index', $._expression), ']')),

    static_get_expression: ($) =>
      prec(PREC.postfix, seq(field('type', $.identifier), '::', field('member', $.identifier))),

    super_expression: ($) =>
      prec.right(
        seq(
          'super',
          choice(
            field('arguments', $.argument_list),
            seq('.', field('member', $.identifier), optional(field('arguments', $.argument_list))),
          ),
        ),
      ),

    struct_literal: ($) => seq(field('type', $.identifier), field('fields', $.field_initializer_list)),

    generic_struct_literal: ($) =>
      prec.dynamic(
        PREC.generic,
        seq(
          field('type', $.identifier),
          field('type_arguments', $.type_arguments),
          field('fields', $.field_initializer_list),
        ),
      ),

    field_initializer_list: ($) => seq('{', commaSep($.field_initializer), '}'),

    field_initializer: ($) => seq(field('name', $.identifier), '=', field('value', $._expression)),

    // fun[move x](a: i32): i32 => expr   |   fun(a: i32) { ... }
    lambda_expression: ($) =>
      seq(
        'fun',
        optional(field('captures', $.capture_list)),
        field('parameters', $.parameter_list),
        optional(seq(':', field('return_type', $._type))),
        choice(field('body', $.block), seq('=>', field('body', $._expression))),
      ),

    capture_list: ($) => seq('[', commaSep($.capture), ']'),

    // `move` and `copy` are contextual keywords (matched by text in parser.cpp);
    // `weak` is a real keyword.
    capture: ($) =>
      choice(
        seq($._move, field('name', $.identifier)),
        seq($._copy, field('name', $.self)),
        seq('weak', field('name', $.self)),
      ),

    _move: (_) => token(prec(-1, 'move')),
    _copy: (_) => token(prec(-1, 'copy')),

    // ── f-strings ─────────────────────────────────────────────────────────
    // Mirrors the lexer: FStringBegin `f"..{`, FStringMid `}..{`, FStringEnd `}.."`.
    // `f"no interpolation"` degenerates to a plain string_literal in the lexer,
    // so it never reaches this rule.
    fstring: ($) =>
      seq(
        alias($._fstring_begin, $.fstring_text),
        $._expression,
        repeat(seq(alias($._fstring_mid, $.fstring_text), $._expression)),
        alias($._fstring_end, $.fstring_text),
      ),

    // ── Literals ──────────────────────────────────────────────────────────

    number_literal: (_) => {
      const dec = /[0-9][0-9_]*/;
      const hex = /0[xX][0-9a-fA-F][0-9a-fA-F_]*/;
      const bin = /0[bB][01][01_]*/;
      const oct = /0[oO][0-7][0-7_]*/;
      const intSuffix = /(ul|UL|u|U|l|L)?/;
      const float = /[0-9][0-9_]*\.[0-9][0-9_]*([eE][+-]?[0-9]+)?[fF]?/;
      const floatExp = /[0-9][0-9_]*[eE][+-]?[0-9]+[fF]?/;
      const floatSuffix = /[0-9][0-9_]*[fF]/;
      return token(
        choice(
          seq(hex, intSuffix),
          seq(bin, intSuffix),
          seq(oct, intSuffix),
          float,
          floatExp,
          floatSuffix,
          seq(dec, intSuffix),
        ),
      );
    },

    boolean_literal: (_) => choice('true', 'false'),
    nil: (_) => 'nil',
    self: (_) => 'self',

    identifier: (_) => /[a-zA-Z_][a-zA-Z0-9_]*/,
  },
});
