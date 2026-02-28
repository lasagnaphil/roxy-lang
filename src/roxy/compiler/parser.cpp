#include "roxy/compiler/parser.hpp"

#include <cstring>

namespace rx {

Parser::Parser(Lexer& lexer, BumpAllocator& allocator)
    : m_lexer(lexer)
    , m_allocator(allocator)
    , m_has_error(false)
{
    m_current = m_lexer.next_token();
    m_previous = m_current;
}

Program* Parser::parse() {
    Vector<Decl*> declarations;

    while (!is_at_end()) {
        Decl* decl = declaration();
        if (m_has_error) return nullptr;
        if (decl) {
            declarations.push_back(decl);
        }
    }

    Program* program = alloc<Program>();
    program->declarations = alloc_span(declarations);
    return program;
}

// Token operations

void Parser::advance() {
    m_previous = m_current;
    m_current = m_lexer.next_token();

    if (m_current.kind == TokenKind::Error) {
        report_error_at(m_current, m_current.start);
    }
}

bool Parser::check(TokenKind kind) const {
    return m_current.kind == kind;
}

bool Parser::match(TokenKind kind) {
    if (!check(kind)) return false;
    advance();
    return true;
}

Token Parser::consume(TokenKind kind, const char* message) {
    if (check(kind)) {
        Token token = m_current;
        advance();
        return token;
    }
    report_error(message);
    return m_current;
}

bool Parser::is_at_end() const {
    return m_current.kind == TokenKind::Eof;
}

// Error handling

void Parser::report_error(const char* message) {
    report_error_at(m_current, message);
}

void Parser::report_error_at(const Token& token, const char* message) {
    if (m_has_error) return;  // Only report first error
    m_has_error = true;
    m_error.loc = token.loc;
    m_error.message = message;
}

// Allocation helper

template <typename T>
Span<T> Parser::alloc_span(const Vector<T>& vec) {
    if (vec.empty()) {
        return Span<T>(nullptr, 0);
    }
    T* data = reinterpret_cast<T*>(m_allocator.alloc_bytes(sizeof(T) * vec.size(), alignof(T)));
    for (u32 i = 0; i < vec.size(); i++) {
        new (data + i) T(vec[i]);
    }
    return Span<T>(data, vec.size());
}

// Expression parsing - Pratt parser

// Infix operator rule: precedence, right-associativity, and binary operator
struct InfixRule {
    u8 precedence;      // 0 means not an infix operator
    bool right_assoc;
    BinaryOp op;
};

// Get the infix parsing rule for a token kind
// Precedence levels (higher = tighter binding):
//   1: || (logical or)
//   2: && (logical and)
//   3: |  (bitwise or)
//   4: ^  (bitwise xor)
//   5: &  (bitwise and)
//   6: == != (equality)
//   7: < <= > >= (comparison)
//   8: << >> (shift)
//   9: + - (additive)
//  10: * / % (multiplicative)
static InfixRule get_infix_rule(TokenKind kind) {
    switch (kind) {
        // Precedence 1: logical or
        case TokenKind::PipePipe:     return {1, false, BinaryOp::Or};
        // Precedence 2: logical and
        case TokenKind::AmpAmp:       return {2, false, BinaryOp::And};
        // Precedence 3: bitwise or
        case TokenKind::Pipe:         return {3, false, BinaryOp::BitOr};
        // Precedence 4: bitwise xor
        case TokenKind::Caret:        return {4, false, BinaryOp::BitXor};
        // Precedence 5: bitwise and
        case TokenKind::Amp:          return {5, false, BinaryOp::BitAnd};
        // Precedence 6: equality
        case TokenKind::EqualEqual:   return {6, false, BinaryOp::Equal};
        case TokenKind::BangEqual:    return {6, false, BinaryOp::NotEqual};
        // Precedence 7: comparison
        case TokenKind::Less:         return {7, false, BinaryOp::Less};
        case TokenKind::LessEqual:    return {7, false, BinaryOp::LessEq};
        case TokenKind::Greater:      return {7, false, BinaryOp::Greater};
        case TokenKind::GreaterEqual: return {7, false, BinaryOp::GreaterEq};
        // Precedence 8: shift
        case TokenKind::LessLess:     return {8, false, BinaryOp::Shl};
        case TokenKind::GreaterGreater: return {8, false, BinaryOp::Shr};
        // Precedence 9: additive
        case TokenKind::Plus:         return {9, false, BinaryOp::Add};
        case TokenKind::Minus:        return {9, false, BinaryOp::Sub};
        // Precedence 10: multiplicative
        case TokenKind::Star:         return {10, false, BinaryOp::Mul};
        case TokenKind::Slash:        return {10, false, BinaryOp::Div};
        case TokenKind::Percent:      return {10, false, BinaryOp::Mod};
        // Not an infix operator
        default:                      return {0, false, {}};
    }
}

Expr* Parser::make_binary(Expr* left, BinaryOp op, Expr* right, SourceLocation loc) {
    Expr* binary = alloc<Expr>();
    binary->kind = AstKind::ExprBinary;
    binary->loc = loc;
    binary->binary.op = op;
    binary->binary.left = left;
    binary->binary.right = right;
    return binary;
}

Expr* Parser::expression() {
    // Parse with precedence 0 to get the full expression
    Expr* expr = parse_precedence(1);
    if (m_has_error) return nullptr;

    // Handle ternary operator (lower precedence than binary ops)
    if (match(TokenKind::Question)) {
        SourceLocation loc = m_previous.loc;
        Expr* then_expr = expression();
        if (m_has_error) return nullptr;

        consume(TokenKind::Colon, "Expected ':' in ternary expression");
        if (m_has_error) return nullptr;

        Expr* else_expr = expression();
        if (m_has_error) return nullptr;

        Expr* ternary_expr = alloc<Expr>();
        ternary_expr->kind = AstKind::ExprTernary;
        ternary_expr->loc = loc;
        ternary_expr->ternary.condition = expr;
        ternary_expr->ternary.then_expr = then_expr;
        ternary_expr->ternary.else_expr = else_expr;
        expr = ternary_expr;
    }

    // Handle assignment (lowest precedence, right-associative)
    if (check(TokenKind::Equal) || check(TokenKind::PlusEqual) ||
        check(TokenKind::MinusEqual) || check(TokenKind::StarEqual) ||
        check(TokenKind::SlashEqual) || check(TokenKind::PercentEqual) ||
        check(TokenKind::AmpEqual) || check(TokenKind::PipeEqual) ||
        check(TokenKind::CaretEqual) || check(TokenKind::LessLessEqual) ||
        check(TokenKind::GreaterGreaterEqual)) {

        Token op_token = m_current;
        AssignOp op;
        switch (m_current.kind) {
            case TokenKind::Equal:        op = AssignOp::Assign; break;
            case TokenKind::PlusEqual:    op = AssignOp::AddAssign; break;
            case TokenKind::MinusEqual:   op = AssignOp::SubAssign; break;
            case TokenKind::StarEqual:    op = AssignOp::MulAssign; break;
            case TokenKind::SlashEqual:   op = AssignOp::DivAssign; break;
            case TokenKind::PercentEqual: op = AssignOp::ModAssign; break;
            case TokenKind::AmpEqual:     op = AssignOp::BitAndAssign; break;
            case TokenKind::PipeEqual:    op = AssignOp::BitOrAssign; break;
            case TokenKind::CaretEqual:   op = AssignOp::BitXorAssign; break;
            case TokenKind::LessLessEqual: op = AssignOp::ShlAssign; break;
            case TokenKind::GreaterGreaterEqual: op = AssignOp::ShrAssign; break;
            default: op = AssignOp::Assign; break;
        }
        advance();

        Expr* value = expression();  // Right-associative
        if (m_has_error) return nullptr;

        Expr* assign_expr = alloc<Expr>();
        assign_expr->kind = AstKind::ExprAssign;
        assign_expr->loc = op_token.loc;
        assign_expr->assign.op = op;
        assign_expr->assign.target = expr;
        assign_expr->assign.value = value;
        return assign_expr;
    }

    return expr;
}

Expr* Parser::parse_precedence(u8 min_prec) {
    // Parse prefix expression (unary operators and primary)
    Expr* left = unary();
    if (m_has_error) return nullptr;

    // Parse infix operators using Pratt parsing
    while (true) {
        InfixRule rule = get_infix_rule(m_current.kind);

        // Stop if not an infix operator or precedence is too low
        if (rule.precedence == 0 || rule.precedence < min_prec) break;

        SourceLocation loc = m_current.loc;
        advance();

        // For right-associative operators, use same precedence; for left-associative, use higher
        u8 next_prec = rule.right_assoc ? rule.precedence : rule.precedence + 1;
        Expr* right = parse_precedence(next_prec);
        if (m_has_error) return nullptr;

        left = make_binary(left, rule.op, right, loc);
    }

    return left;
}

Expr* Parser::unary() {
    if (check(TokenKind::Bang) || check(TokenKind::Minus) || check(TokenKind::Tilde)) {
        UnaryOp op;
        switch (m_current.kind) {
            case TokenKind::Bang:  op = UnaryOp::Not; break;
            case TokenKind::Minus: op = UnaryOp::Negate; break;
            case TokenKind::Tilde: op = UnaryOp::BitNot; break;
            default: op = UnaryOp::Not; break;
        }
        SourceLocation loc = m_current.loc;
        advance();

        Expr* operand = unary();
        if (m_has_error) return nullptr;

        Expr* unary_expr = alloc<Expr>();
        unary_expr->kind = AstKind::ExprUnary;
        unary_expr->loc = loc;
        unary_expr->unary.op = op;
        unary_expr->unary.operand = operand;
        return unary_expr;
    }

    return postfix(primary());
}

Expr* Parser::postfix(Expr* expr) {
    if (m_has_error) return nullptr;

    while (true) {
        if (match(TokenKind::LeftParen)) {
            expr = finish_call(expr);
            if (m_has_error) return nullptr;
        } else if (match(TokenKind::Dot)) {
            Token name_token = consume(TokenKind::Identifier, "Expected property name after '.'");
            if (m_has_error) return nullptr;

            Expr* get_expr = alloc<Expr>();
            get_expr->kind = AstKind::ExprGet;
            get_expr->loc = name_token.loc;
            get_expr->get.object = expr;
            get_expr->get.name = name_token.text();
            expr = get_expr;
        } else if (match(TokenKind::LeftBracket)) {
            expr = finish_index(expr);
            if (m_has_error) return nullptr;
        } else {
            break;
        }
    }

    return expr;
}

Expr* Parser::finish_call(Expr* callee) {
    Vector<CallArg> arguments;
    SourceLocation loc = m_previous.loc;

    if (!check(TokenKind::RightParen)) {
        do {
            CallArg arg;
            arg.modifier = ParamModifier::None;
            arg.modifier_loc = {};

            // Check for out/inout modifier
            if (match(TokenKind::KwOut)) {
                arg.modifier = ParamModifier::Out;
                arg.modifier_loc = m_previous.loc;
            } else if (match(TokenKind::KwInout)) {
                arg.modifier = ParamModifier::Inout;
                arg.modifier_loc = m_previous.loc;
            }

            arg.expr = expression();
            if (m_has_error) return nullptr;
            arguments.push_back(arg);
        } while (match(TokenKind::Comma));
    }

    consume(TokenKind::RightParen, "Expected ')' after arguments");
    if (m_has_error) return nullptr;

    Expr* call_expr = alloc<Expr>();
    call_expr->kind = AstKind::ExprCall;
    call_expr->loc = loc;
    call_expr->call.callee = callee;
    call_expr->call.arguments = alloc_span(arguments);
    call_expr->call.type_args = Span<TypeExpr*>();
    call_expr->call.constructor_name = StringView(nullptr, 0);
    call_expr->call.mangled_name = StringView(nullptr, 0);
    call_expr->call.is_heap = false;
    return call_expr;
}

Expr* Parser::finish_index(Expr* object) {
    SourceLocation loc = m_previous.loc;

    Expr* index = expression();
    if (m_has_error) return nullptr;

    consume(TokenKind::RightBracket, "Expected ']' after index");
    if (m_has_error) return nullptr;

    Expr* index_expr = alloc<Expr>();
    index_expr->kind = AstKind::ExprIndex;
    index_expr->loc = loc;
    index_expr->index.object = object;
    index_expr->index.index = index;
    return index_expr;
}

Expr* Parser::primary() {
    // Literals
    if (match(TokenKind::KwNil)) {
        Expr* expr = alloc<Expr>();
        expr->kind = AstKind::ExprLiteral;
        expr->loc = m_previous.loc;
        expr->literal.literal_kind = LiteralKind::Nil;
        return expr;
    }

    if (match(TokenKind::KwTrue)) {
        Expr* expr = alloc<Expr>();
        expr->kind = AstKind::ExprLiteral;
        expr->loc = m_previous.loc;
        expr->literal.literal_kind = LiteralKind::Bool;
        expr->literal.bool_value = true;
        return expr;
    }

    if (match(TokenKind::KwFalse)) {
        Expr* expr = alloc<Expr>();
        expr->kind = AstKind::ExprLiteral;
        expr->loc = m_previous.loc;
        expr->literal.literal_kind = LiteralKind::Bool;
        expr->literal.bool_value = false;
        return expr;
    }

    if (match(TokenKind::IntLiteral)) {
        Expr* expr = alloc<Expr>();
        expr->kind = AstKind::ExprLiteral;
        expr->loc = m_previous.loc;
        expr->literal.int_value = m_previous.int_value;

        // Determine LiteralKind based on suffix
        StringView text = m_previous.text();
        if (!text.empty()) {
            char last = text[text.size() - 1];
            if (last == 'l' || last == 'L') {
                // Check for 'ul' suffix
                if (text.size() >= 2) {
                    char prev = text[text.size() - 2];
                    if (prev == 'u' || prev == 'U') {
                        expr->literal.literal_kind = LiteralKind::U64;
                    } else {
                        expr->literal.literal_kind = LiteralKind::I64;
                    }
                } else {
                    expr->literal.literal_kind = LiteralKind::I64;
                }
            } else if (last == 'u' || last == 'U') {
                expr->literal.literal_kind = LiteralKind::U32;
            } else {
                expr->literal.literal_kind = LiteralKind::I32;
            }
        } else {
            expr->literal.literal_kind = LiteralKind::I32;
        }
        return expr;
    }

    if (match(TokenKind::FloatLiteral)) {
        Expr* expr = alloc<Expr>();
        expr->kind = AstKind::ExprLiteral;
        expr->loc = m_previous.loc;
        expr->literal.float_value = m_previous.float_value;

        // Determine LiteralKind based on suffix
        StringView text = m_previous.text();
        if (!text.empty()) {
            char last = text[text.size() - 1];
            if (last == 'f' || last == 'F') {
                expr->literal.literal_kind = LiteralKind::F32;
            } else {
                expr->literal.literal_kind = LiteralKind::F64;
            }
        } else {
            expr->literal.literal_kind = LiteralKind::F64;
        }
        return expr;
    }

    // F-string interpolation: f"text {expr} text"
    if (match(TokenKind::FStringBegin)) {
        SourceLocation loc = m_previous.loc;
        Vector<StringView> parts;
        Vector<Expr*> expressions;
        parts.push_back(process_fstring_part(m_previous));

        while (true) {
            Expr* e = expression();
            if (m_has_error) return nullptr;
            expressions.push_back(e);

            if (match(TokenKind::FStringMid)) {
                parts.push_back(process_fstring_part(m_previous));
            } else if (match(TokenKind::FStringEnd)) {
                parts.push_back(process_fstring_part(m_previous));
                break;
            } else {
                report_error("Expected '}' in f-string interpolation.");
                return nullptr;
            }
        }

        Expr* expr = alloc<Expr>();
        expr->kind = AstKind::ExprStringInterp;
        expr->loc = loc;
        expr->string_interp.parts = alloc_span(parts);
        expr->string_interp.expressions = alloc_span(expressions);
        return expr;
    }

    if (match(TokenKind::StringLiteral)) {
        Expr* expr = alloc<Expr>();
        expr->kind = AstKind::ExprLiteral;
        expr->loc = m_previous.loc;
        expr->literal.literal_kind = LiteralKind::String;
        // Strip quotes and process escape sequences
        expr->literal.string_value = process_string_literal(m_previous);
        return expr;
    }

    // uniq Type(...) or uniq Type { ... } - heap allocation with constructor/literal
    // Must be parsed before regular identifiers
    if (match(TokenKind::KwUniq)) {
        SourceLocation loc = m_previous.loc;

        // uniq Type(...) or uniq Type { ... }
        Token type_token = consume(TokenKind::Identifier, "Expected type name after 'uniq'");
        if (m_has_error) return nullptr;

        // Check for struct literal: uniq Type { ... }
        if (match(TokenKind::LeftBrace)) {
            Vector<FieldInit> fields;
            if (!check(TokenKind::RightBrace)) {
                do {
                    Token name_token = consume(TokenKind::Identifier, "Expected field name");
                    if (m_has_error) return nullptr;
                    consume(TokenKind::Equal, "Expected '=' after field name");
                    if (m_has_error) return nullptr;
                    Expr* value = expression();
                    if (m_has_error) return nullptr;
                    FieldInit field_init;
                    field_init.name = name_token.text();
                    field_init.value = value;
                    field_init.loc = name_token.loc;
                    fields.push_back(field_init);
                } while (match(TokenKind::Comma));
            }
            consume(TokenKind::RightBrace, "Expected '}' after struct literal fields");
            if (m_has_error) return nullptr;

            Expr* expr = alloc<Expr>();
            expr->kind = AstKind::ExprStructLiteral;
            expr->loc = loc;
            expr->struct_literal.type_name = type_token.text();
            expr->struct_literal.fields = alloc_span(fields);
            expr->struct_literal.type_args = Span<TypeExpr*>();
            expr->struct_literal.mangled_name = StringView(nullptr, 0);
            expr->struct_literal.is_heap = true;
            return expr;
        }

        // Constructor call: uniq Type() or uniq Type.ctor_name()
        StringView ctor_name(nullptr, 0);
        if (match(TokenKind::Dot)) {
            Token name_token = consume(TokenKind::Identifier, "Expected constructor name after '.'");
            if (m_has_error) return nullptr;
            ctor_name = name_token.text();
        }

        consume(TokenKind::LeftParen, "Expected '(' or '{' after type");
        if (m_has_error) return nullptr;

        Vector<CallArg> arguments;
        if (!check(TokenKind::RightParen)) {
            do {
                CallArg arg;
                arg.modifier = ParamModifier::None;
                arg.modifier_loc = {};
                if (match(TokenKind::KwOut)) {
                    arg.modifier = ParamModifier::Out;
                    arg.modifier_loc = m_previous.loc;
                } else if (match(TokenKind::KwInout)) {
                    arg.modifier = ParamModifier::Inout;
                    arg.modifier_loc = m_previous.loc;
                }
                arg.expr = expression();
                if (m_has_error) return nullptr;
                arguments.push_back(arg);
            } while (match(TokenKind::Comma));
        }
        consume(TokenKind::RightParen, "Expected ')' after arguments");
        if (m_has_error) return nullptr;

        // Create callee as identifier
        Expr* callee = alloc<Expr>();
        callee->kind = AstKind::ExprIdentifier;
        callee->loc = type_token.loc;
        callee->identifier.name = type_token.text();

        Expr* expr = alloc<Expr>();
        expr->kind = AstKind::ExprCall;
        expr->loc = loc;
        expr->call.callee = callee;
        expr->call.arguments = alloc_span(arguments);
        expr->call.type_args = Span<TypeExpr*>();
        expr->call.constructor_name = ctor_name;
        expr->call.mangled_name = StringView(nullptr, 0);
        expr->call.is_heap = true;
        return expr;
    }

    // Identifier, or struct literal (Type { ... }), or constructor call (Type())
    // Also handles Type::member (static get)
    if (match(TokenKind::Identifier)) {
        Token name_token = m_previous;

        // Check for static member access (Type::member)
        if (match(TokenKind::ColonColon)) {
            Token member_token = consume(TokenKind::Identifier, "Expected member name after '::'");
            if (m_has_error) return nullptr;

            Expr* expr = alloc<Expr>();
            expr->kind = AstKind::ExprStaticGet;
            expr->loc = name_token.loc;
            expr->static_get.type_name = name_token.text();
            expr->static_get.member_name = member_token.text();
            return expr;
        }

        // Check for generic args: identifier<types>( or identifier<types>{
        // Uses trial parse to disambiguate from comparison operators
        if (check(TokenKind::Less)) {
            Span<TypeExpr*> type_args = try_parse_generic_args();
            if (type_args.size() > 0) {
                // Successfully parsed generic args, next is ( or {
                if (match(TokenKind::LeftBrace)) {
                    // Generic struct literal: Box<i32> { value = 42 }
                    SourceLocation loc = name_token.loc;
                    Vector<FieldInit> fields;
                    if (!check(TokenKind::RightBrace)) {
                        do {
                            Token field_token = consume(TokenKind::Identifier, "Expected field name");
                            if (m_has_error) return nullptr;
                            consume(TokenKind::Equal, "Expected '=' after field name");
                            if (m_has_error) return nullptr;
                            Expr* value = expression();
                            if (m_has_error) return nullptr;
                            FieldInit field_init;
                            field_init.name = field_token.text();
                            field_init.value = value;
                            field_init.loc = field_token.loc;
                            fields.push_back(field_init);
                        } while (match(TokenKind::Comma));
                    }
                    consume(TokenKind::RightBrace, "Expected '}' after struct literal fields");
                    if (m_has_error) return nullptr;

                    Expr* expr = alloc<Expr>();
                    expr->kind = AstKind::ExprStructLiteral;
                    expr->loc = loc;
                    expr->struct_literal.type_name = name_token.text();
                    expr->struct_literal.fields = alloc_span(fields);
                    expr->struct_literal.type_args = type_args;
                    expr->struct_literal.mangled_name = StringView(nullptr, 0);
                    expr->struct_literal.is_heap = false;
                    return expr;
                }

                if (match(TokenKind::LeftParen)) {
                    // Generic function/constructor call: identity<i32>(42)
                    Vector<CallArg> arguments;
                    if (!check(TokenKind::RightParen)) {
                        do {
                            CallArg arg;
                            arg.modifier = ParamModifier::None;
                            arg.modifier_loc = {};
                            if (match(TokenKind::KwOut)) {
                                arg.modifier = ParamModifier::Out;
                                arg.modifier_loc = m_previous.loc;
                            } else if (match(TokenKind::KwInout)) {
                                arg.modifier = ParamModifier::Inout;
                                arg.modifier_loc = m_previous.loc;
                            }
                            arg.expr = expression();
                            if (m_has_error) return nullptr;
                            arguments.push_back(arg);
                        } while (match(TokenKind::Comma));
                    }
                    consume(TokenKind::RightParen, "Expected ')' after arguments");
                    if (m_has_error) return nullptr;

                    Expr* callee = alloc<Expr>();
                    callee->kind = AstKind::ExprIdentifier;
                    callee->loc = name_token.loc;
                    callee->identifier.name = name_token.text();

                    Expr* expr = alloc<Expr>();
                    expr->kind = AstKind::ExprCall;
                    expr->loc = name_token.loc;
                    expr->call.callee = callee;
                    expr->call.arguments = alloc_span(arguments);
                    expr->call.type_args = type_args;
                    expr->call.constructor_name = StringView(nullptr, 0);
                    expr->call.mangled_name = StringView(nullptr, 0);
                    expr->call.is_heap = false;
                    return expr;
                }
            }
            // Fall through to non-generic paths
        }

        // Check for struct literal: Type { ... }
        if (match(TokenKind::LeftBrace)) {
            SourceLocation loc = name_token.loc;
            Vector<FieldInit> fields;
            if (!check(TokenKind::RightBrace)) {
                do {
                    Token field_token = consume(TokenKind::Identifier, "Expected field name");
                    if (m_has_error) return nullptr;
                    consume(TokenKind::Equal, "Expected '=' after field name");
                    if (m_has_error) return nullptr;
                    Expr* value = expression();
                    if (m_has_error) return nullptr;
                    FieldInit field_init;
                    field_init.name = field_token.text();
                    field_init.value = value;
                    field_init.loc = field_token.loc;
                    fields.push_back(field_init);
                } while (match(TokenKind::Comma));
            }
            consume(TokenKind::RightBrace, "Expected '}' after struct literal fields");
            if (m_has_error) return nullptr;

            Expr* expr = alloc<Expr>();
            expr->kind = AstKind::ExprStructLiteral;
            expr->loc = loc;
            expr->struct_literal.type_name = name_token.text();
            expr->struct_literal.fields = alloc_span(fields);
            expr->struct_literal.type_args = Span<TypeExpr*>();
            expr->struct_literal.mangled_name = StringView(nullptr, 0);
            expr->struct_literal.is_heap = false;
            return expr;
        }

        // Regular identifier - let postfix() handle any following . or ( or [
        Expr* expr = alloc<Expr>();
        expr->kind = AstKind::ExprIdentifier;
        expr->loc = name_token.loc;
        expr->identifier.name = name_token.text();
        return expr;
    }

    // self
    if (match(TokenKind::KwSelf)) {
        Expr* expr = alloc<Expr>();
        expr->kind = AstKind::ExprThis;
        expr->loc = m_previous.loc;
        return expr;
    }

    // super() for constructor call, super.method for method call, super.ctor_name() for named constructor
    if (match(TokenKind::KwSuper)) {
        SourceLocation loc = m_previous.loc;

        // Check if this is super() - constructor call with no name
        if (check(TokenKind::LeftParen)) {
            // super() - default parent constructor call
            // Return a SuperExpr with empty method_name to indicate constructor call
            Expr* expr = alloc<Expr>();
            expr->kind = AstKind::ExprSuper;
            expr->loc = loc;
            expr->super_expr.method_name = StringView(nullptr, 0);  // empty = default constructor
            return expr;
        }

        // Otherwise, expect super.something
        consume(TokenKind::Dot, "Expected '.' or '(' after 'super'");
        if (m_has_error) return nullptr;

        Token method_token = consume(TokenKind::Identifier, "Expected method name after 'super.'");
        if (m_has_error) return nullptr;

        Expr* expr = alloc<Expr>();
        expr->kind = AstKind::ExprSuper;
        expr->loc = loc;
        expr->super_expr.method_name = method_token.text();
        return expr;
    }

    // Grouping: (expr)
    if (match(TokenKind::LeftParen)) {
        SourceLocation loc = m_previous.loc;
        Expr* inner = expression();
        if (m_has_error) return nullptr;

        consume(TokenKind::RightParen, "Expected ')' after expression");
        if (m_has_error) return nullptr;

        Expr* expr = alloc<Expr>();
        expr->kind = AstKind::ExprGrouping;
        expr->loc = loc;
        expr->grouping.expr = inner;
        return expr;
    }

    report_error("Expected expression");
    return nullptr;
}

// Statement parsing

Stmt* Parser::statement() {
    if (match(TokenKind::LeftBrace)) {
        return block_statement();
    }
    if (match(TokenKind::KwIf)) {
        return if_statement();
    }
    if (match(TokenKind::KwWhile)) {
        return while_statement();
    }
    if (match(TokenKind::KwFor)) {
        return for_statement();
    }
    if (match(TokenKind::KwReturn)) {
        return return_statement();
    }
    if (match(TokenKind::KwBreak)) {
        return break_statement();
    }
    if (match(TokenKind::KwContinue)) {
        return continue_statement();
    }
    if (match(TokenKind::KwDelete)) {
        return delete_statement();
    }
    if (match(TokenKind::KwWhen)) {
        return when_statement();
    }
    if (match(TokenKind::KwThrow)) {
        return throw_statement();
    }
    if (match(TokenKind::KwTry)) {
        return try_statement();
    }

    return expression_statement();
}

Stmt* Parser::block_statement() {
    SourceLocation loc = m_previous.loc;
    Vector<Decl*> declarations;

    while (!check(TokenKind::RightBrace) && !is_at_end()) {
        Decl* decl = declaration();
        if (m_has_error) return nullptr;
        if (decl) {
            declarations.push_back(decl);
        }
    }

    consume(TokenKind::RightBrace, "Expected '}' after block");
    if (m_has_error) return nullptr;

    Stmt* stmt = alloc<Stmt>();
    stmt->kind = AstKind::StmtBlock;
    stmt->loc = loc;
    stmt->block.declarations = alloc_span(declarations);
    return stmt;
}

Stmt* Parser::if_statement() {
    SourceLocation loc = m_previous.loc;

    consume(TokenKind::LeftParen, "Expected '(' after 'if'");
    if (m_has_error) return nullptr;

    Expr* condition = expression();
    if (m_has_error) return nullptr;

    consume(TokenKind::RightParen, "Expected ')' after if condition");
    if (m_has_error) return nullptr;

    Stmt* then_branch = statement();
    if (m_has_error) return nullptr;

    Stmt* else_branch = nullptr;
    if (match(TokenKind::KwElse)) {
        else_branch = statement();
        if (m_has_error) return nullptr;
    }

    Stmt* stmt = alloc<Stmt>();
    stmt->kind = AstKind::StmtIf;
    stmt->loc = loc;
    stmt->if_stmt.condition = condition;
    stmt->if_stmt.then_branch = then_branch;
    stmt->if_stmt.else_branch = else_branch;
    return stmt;
}

Stmt* Parser::while_statement() {
    SourceLocation loc = m_previous.loc;

    consume(TokenKind::LeftParen, "Expected '(' after 'while'");
    if (m_has_error) return nullptr;

    Expr* condition = expression();
    if (m_has_error) return nullptr;

    consume(TokenKind::RightParen, "Expected ')' after while condition");
    if (m_has_error) return nullptr;

    Stmt* body = statement();
    if (m_has_error) return nullptr;

    Stmt* stmt = alloc<Stmt>();
    stmt->kind = AstKind::StmtWhile;
    stmt->loc = loc;
    stmt->while_stmt.condition = condition;
    stmt->while_stmt.body = body;
    return stmt;
}

Stmt* Parser::for_statement() {
    SourceLocation loc = m_previous.loc;

    consume(TokenKind::LeftParen, "Expected '(' after 'for'");
    if (m_has_error) return nullptr;

    // Initializer
    Decl* initializer = nullptr;
    if (match(TokenKind::Semicolon)) {
        // No initializer
    } else if (match(TokenKind::KwVar)) {
        initializer = var_declaration(false);
        if (m_has_error) return nullptr;
    } else {
        // Expression statement as initializer
        Stmt* expr_stmt = expression_statement();
        if (m_has_error) return nullptr;

        initializer = alloc<Decl>();
        initializer->kind = AstKind::StmtExpr;
        initializer->loc = expr_stmt->loc;
        initializer->stmt = *expr_stmt;
    }

    // Condition
    Expr* condition = nullptr;
    if (!check(TokenKind::Semicolon)) {
        condition = expression();
        if (m_has_error) return nullptr;
    }
    consume(TokenKind::Semicolon, "Expected ';' after for condition");
    if (m_has_error) return nullptr;

    // Increment
    Expr* increment = nullptr;
    if (!check(TokenKind::RightParen)) {
        increment = expression();
        if (m_has_error) return nullptr;
    }
    consume(TokenKind::RightParen, "Expected ')' after for clauses");
    if (m_has_error) return nullptr;

    Stmt* body = statement();
    if (m_has_error) return nullptr;

    Stmt* stmt = alloc<Stmt>();
    stmt->kind = AstKind::StmtFor;
    stmt->loc = loc;
    stmt->for_stmt.initializer = initializer;
    stmt->for_stmt.condition = condition;
    stmt->for_stmt.increment = increment;
    stmt->for_stmt.body = body;
    return stmt;
}

Stmt* Parser::return_statement() {
    SourceLocation loc = m_previous.loc;

    Expr* value = nullptr;
    if (!check(TokenKind::Semicolon)) {
        value = expression();
        if (m_has_error) return nullptr;
    }

    consume(TokenKind::Semicolon, "Expected ';' after return value");
    if (m_has_error) return nullptr;

    Stmt* stmt = alloc<Stmt>();
    stmt->kind = AstKind::StmtReturn;
    stmt->loc = loc;
    stmt->return_stmt.value = value;
    return stmt;
}

Stmt* Parser::break_statement() {
    SourceLocation loc = m_previous.loc;
    consume(TokenKind::Semicolon, "Expected ';' after 'break'");
    if (m_has_error) return nullptr;

    Stmt* stmt = alloc<Stmt>();
    stmt->kind = AstKind::StmtBreak;
    stmt->loc = loc;
    return stmt;
}

Stmt* Parser::continue_statement() {
    SourceLocation loc = m_previous.loc;
    consume(TokenKind::Semicolon, "Expected ';' after 'continue'");
    if (m_has_error) return nullptr;

    Stmt* stmt = alloc<Stmt>();
    stmt->kind = AstKind::StmtContinue;
    stmt->loc = loc;
    return stmt;
}

Stmt* Parser::delete_statement() {
    SourceLocation loc = m_previous.loc;

    // Parse the expression to delete
    // We need to parse this carefully:
    // - "delete expr;" where expr is any expression
    // - "delete expr.dtor_name(args);" where dtor_name is a destructor
    //
    // Since expression() will parse d.save(5) as a call, we need to detect this.
    // If we see an ExprCall where callee is ExprGet, we treat it as a destructor call.

    Expr* expr = expression();
    if (m_has_error) return nullptr;

    StringView dtor_name(nullptr, 0);  // Empty for default destructor
    Vector<CallArg> arguments;

    // Check if the expression is a call on a member (potential destructor call)
    // e.g., d.save(5) where d is the object and save is the destructor
    if (expr->kind == AstKind::ExprCall) {
        CallExpr& call_expr = expr->call;
        if (call_expr.callee->kind == AstKind::ExprGet) {
            GetExpr& get_expr = call_expr.callee->get;
            // This is a destructor call: object.destructor_name(args)
            dtor_name = get_expr.name;
            for (u32 i = 0; i < call_expr.arguments.size(); i++) {
                arguments.push_back(call_expr.arguments[i]);
            }
            // The actual object to delete is the object of the get expression
            expr = get_expr.object;
        }
    }

    consume(TokenKind::Semicolon, "Expected ';' after delete statement");
    if (m_has_error) return nullptr;

    Stmt* stmt = alloc<Stmt>();
    stmt->kind = AstKind::StmtDelete;
    stmt->loc = loc;
    stmt->delete_stmt.expr = expr;
    stmt->delete_stmt.destructor_name = dtor_name;
    stmt->delete_stmt.arguments = alloc_span(arguments);
    return stmt;
}

Stmt* Parser::when_statement() {
    SourceLocation loc = m_previous.loc;

    // Parse discriminant expression
    // We use a simpler parsing approach: identifier with optional member access
    // This avoids the ambiguity where "when c { ... }" could be parsed as struct literal
    Token name_token = consume(TokenKind::Identifier, "Expected discriminant after 'when'");
    if (m_has_error) return nullptr;

    Expr* discriminant = alloc<Expr>();
    discriminant->kind = AstKind::ExprIdentifier;
    discriminant->loc = name_token.loc;
    discriminant->identifier.name = name_token.text();

    // Handle member access (e.g., obj.field)
    while (match(TokenKind::Dot)) {
        Token member_token = consume(TokenKind::Identifier, "Expected member name after '.'");
        if (m_has_error) return nullptr;

        Expr* get_expr = alloc<Expr>();
        get_expr->kind = AstKind::ExprGet;
        get_expr->loc = member_token.loc;
        get_expr->get.object = discriminant;
        get_expr->get.name = member_token.text();
        discriminant = get_expr;
    }

    consume(TokenKind::LeftBrace, "Expected '{' after 'when' discriminant");
    if (m_has_error) return nullptr;

    Vector<WhenCase> cases;
    Span<Decl*> else_body(nullptr, 0);
    SourceLocation else_loc{};

    // Parse cases: case A, B: { ... } or case A, B: stmt; stmt; ...
    while (!check(TokenKind::RightBrace) && !is_at_end()) {
        if (match(TokenKind::KwCase)) {
            WhenCase wc;
            wc.loc = m_previous.loc;

            // Parse case names (comma-separated)
            Vector<StringView> case_names;
            do {
                Token name_token = consume(TokenKind::Identifier, "Expected case name");
                if (m_has_error) return nullptr;
                case_names.push_back(name_token.text());
            } while (match(TokenKind::Comma));

            consume(TokenKind::Colon, "Expected ':' after case name(s)");
            if (m_has_error) return nullptr;

            // Parse case body - multiple statements until next case, else, or }
            Vector<Decl*> body_decls;
            while (!check(TokenKind::KwCase) && !check(TokenKind::KwElse) &&
                   !check(TokenKind::RightBrace) && !is_at_end()) {
                Decl* decl = declaration();
                if (m_has_error) return nullptr;
                if (decl) body_decls.push_back(decl);
            }

            wc.case_names = alloc_span(case_names);
            wc.body = alloc_span(body_decls);
            cases.push_back(wc);
        }
        else if (match(TokenKind::KwElse)) {
            else_loc = m_previous.loc;
            consume(TokenKind::Colon, "Expected ':' after 'else'");
            if (m_has_error) return nullptr;

            // Parse else body - multiple statements until }
            Vector<Decl*> else_decls;
            while (!check(TokenKind::RightBrace) && !is_at_end()) {
                Decl* decl = declaration();
                if (m_has_error) return nullptr;
                if (decl) else_decls.push_back(decl);
            }
            else_body = alloc_span(else_decls);
            break;  // else must be last
        }
        else {
            report_error("Expected 'case' or 'else' in when statement");
            return nullptr;
        }
    }

    consume(TokenKind::RightBrace, "Expected '}' after when statement");
    if (m_has_error) return nullptr;

    Stmt* stmt = alloc<Stmt>();
    stmt->kind = AstKind::StmtWhen;
    stmt->loc = loc;
    stmt->when_stmt.discriminant = discriminant;
    stmt->when_stmt.cases = alloc_span(cases);
    stmt->when_stmt.else_body = else_body;
    stmt->when_stmt.else_loc = else_loc;
    return stmt;
}

Stmt* Parser::throw_statement() {
    SourceLocation loc = m_previous.loc;

    Expr* expr = expression();
    if (m_has_error) return nullptr;

    consume(TokenKind::Semicolon, "Expected ';' after throw expression");
    if (m_has_error) return nullptr;

    Stmt* stmt = alloc<Stmt>();
    stmt->kind = AstKind::StmtThrow;
    stmt->loc = loc;
    stmt->throw_stmt.expr = expr;
    return stmt;
}

Stmt* Parser::try_statement() {
    SourceLocation loc = m_previous.loc;

    // Parse try body
    consume(TokenKind::LeftBrace, "Expected '{' after 'try'");
    if (m_has_error) return nullptr;

    Stmt* try_body = block_statement();
    if (m_has_error) return nullptr;

    // Parse catch clauses
    Vector<CatchClause> catches;
    while (match(TokenKind::KwCatch)) {
        CatchClause clause;
        clause.loc = m_previous.loc;
        clause.resolved_type = nullptr;

        consume(TokenKind::LeftParen, "Expected '(' after 'catch'");
        if (m_has_error) return nullptr;

        Token var_token = consume(TokenKind::Identifier, "Expected variable name in catch clause");
        if (m_has_error) return nullptr;
        clause.var_name = var_token.text();

        // Optional type annotation
        if (match(TokenKind::Colon)) {
            clause.exception_type = type_expression();
            if (m_has_error) return nullptr;
        } else {
            clause.exception_type = nullptr;  // catch-all
        }

        consume(TokenKind::RightParen, "Expected ')' after catch variable");
        if (m_has_error) return nullptr;

        consume(TokenKind::LeftBrace, "Expected '{' after catch clause");
        if (m_has_error) return nullptr;

        clause.body = block_statement();
        if (m_has_error) return nullptr;

        catches.push_back(clause);
    }

    // Parse optional finally
    Stmt* finally_body = nullptr;
    if (match(TokenKind::KwFinally)) {
        consume(TokenKind::LeftBrace, "Expected '{' after 'finally'");
        if (m_has_error) return nullptr;

        finally_body = block_statement();
        if (m_has_error) return nullptr;
    }

    // Validate: at least one catch or a finally
    if (catches.empty() && !finally_body) {
        report_error("'try' requires at least one 'catch' or a 'finally' block");
        return nullptr;
    }

    Stmt* stmt = alloc<Stmt>();
    stmt->kind = AstKind::StmtTry;
    stmt->loc = loc;
    stmt->try_stmt.try_body = try_body;
    stmt->try_stmt.catches = alloc_span(catches);
    stmt->try_stmt.finally_body = finally_body;
    return stmt;
}

Stmt* Parser::expression_statement() {
    SourceLocation loc = m_current.loc;

    Expr* expr = expression();
    if (m_has_error) return nullptr;

    consume(TokenKind::Semicolon, "Expected ';' after expression");
    if (m_has_error) return nullptr;

    Stmt* stmt = alloc<Stmt>();
    stmt->kind = AstKind::StmtExpr;
    stmt->loc = loc;
    stmt->expr_stmt.expr = expr;
    return stmt;
}

// Declaration parsing

Decl* Parser::declaration() {
    bool is_pub = match(TokenKind::KwPub);

    if (match(TokenKind::KwVar)) {
        return var_declaration(is_pub);
    }

    bool is_native = match(TokenKind::KwNative);
    if (match(TokenKind::KwFun)) {
        // Check for constructor: fun new StructName...
        if (match(TokenKind::KwNew)) {
            if (is_native) {
                report_error("constructors cannot be 'native'");
                return nullptr;
            }
            return constructor_declaration(is_pub);
        }
        // Check for destructor: fun delete StructName...
        if (match(TokenKind::KwDelete)) {
            if (is_native) {
                report_error("destructors cannot be 'native'");
                return nullptr;
            }
            return destructor_declaration(is_pub);
        }
        return fun_declaration(is_pub, is_native);
    }

    if (is_native) {
        report_error("'native' can only precede 'fun'");
        return nullptr;
    }

    if (match(TokenKind::KwStruct)) {
        return struct_declaration(is_pub);
    }

    if (match(TokenKind::KwEnum)) {
        return enum_declaration(is_pub);
    }

    if (match(TokenKind::KwTrait)) {
        return trait_declaration(is_pub);
    }

    if (is_pub) {
        report_error("'pub' can only precede declarations");
        return nullptr;
    }

    if (match(TokenKind::KwImport) || match(TokenKind::KwFrom)) {
        TokenKind prev_kind = m_previous.kind;
        if (prev_kind == TokenKind::KwFrom) {
            // Put back in "from" state for import_declaration
            // We need to handle "from pkg import ..."
        }
        return import_declaration();
    }

    // Statement (wrapped in a Decl)
    Stmt* stmt = statement();
    if (m_has_error) return nullptr;

    Decl* decl = alloc<Decl>();
    decl->kind = stmt->kind;
    decl->loc = stmt->loc;
    decl->stmt = *stmt;
    return decl;
}

Decl* Parser::var_declaration(bool is_pub) {
    SourceLocation loc = m_previous.loc;

    Token name_token = consume(TokenKind::Identifier, "Expected variable name");
    if (m_has_error) return nullptr;

    TypeExpr* type = nullptr;
    if (match(TokenKind::Colon)) {
        type = type_expression();
        if (m_has_error) return nullptr;
    }

    Expr* initializer = nullptr;
    if (match(TokenKind::Equal)) {
        initializer = expression();
        if (m_has_error) return nullptr;
    }

    consume(TokenKind::Semicolon, "Expected ';' after variable declaration");
    if (m_has_error) return nullptr;

    Decl* decl = alloc<Decl>();
    decl->kind = AstKind::DeclVar;
    decl->loc = loc;
    decl->var_decl.name = name_token.text();
    decl->var_decl.type = type;
    decl->var_decl.initializer = initializer;
    decl->var_decl.is_pub = is_pub;
    decl->var_decl.resolved_type = nullptr;
    return decl;
}

Vector<Param> Parser::parse_parameters() {
    Vector<Param> params;

    if (!check(TokenKind::RightParen)) {
        do {
            Param param;
            param.modifier = ParamModifier::None;

            Token name_token = consume(TokenKind::Identifier, "Expected parameter name");
            if (m_has_error) return params;
            param.name = name_token.text();
            param.loc = name_token.loc;

            consume(TokenKind::Colon, "Expected ':' after parameter name");
            if (m_has_error) return params;

            // Check for parameter modifiers (out/inout) before the type
            if (match(TokenKind::KwOut)) {
                param.modifier = ParamModifier::Out;
            } else if (match(TokenKind::KwInout)) {
                param.modifier = ParamModifier::Inout;
            }

            param.type = type_expression();
            if (m_has_error) return params;

            params.push_back(param);
        } while (match(TokenKind::Comma));
    }

    return params;
}

Decl* Parser::fun_declaration(bool is_pub, bool is_native) {
    SourceLocation loc = m_previous.loc;

    Token name_token = consume(TokenKind::Identifier, "Expected function name");
    if (m_has_error) return nullptr;

    // Parse optional type params: fun Name<T, U>
    Span<TypeParam> type_params;
    if (check(TokenKind::Less)) {
        type_params = parse_type_params();
        if (m_has_error) return nullptr;
    }

    // Check for method syntax: fun Name.method() or fun Name<T>.method()
    if (match(TokenKind::Dot)) {
        return method_declaration(is_pub, is_native, name_token, type_params);
    }

    // Regular function — type_params are the function's own generic params
    consume(TokenKind::LeftParen, "Expected '(' after function name");
    if (m_has_error) return nullptr;

    Vector<Param> params = parse_parameters();
    if (m_has_error) return nullptr;

    consume(TokenKind::RightParen, "Expected ')' after parameters");
    if (m_has_error) return nullptr;

    TypeExpr* return_type = nullptr;
    if (match(TokenKind::Colon)) {
        return_type = type_expression();
        if (m_has_error) return nullptr;
    }

    Stmt* body = nullptr;
    if (is_native) {
        consume(TokenKind::Semicolon, "Expected ';' after native function declaration");
        if (m_has_error) return nullptr;
    } else {
        consume(TokenKind::LeftBrace, "Expected '{' before function body");
        if (m_has_error) return nullptr;

        body = block_statement();
        if (m_has_error) return nullptr;
    }

    Decl* decl = alloc<Decl>();
    decl->kind = AstKind::DeclFun;
    decl->loc = loc;
    decl->fun_decl.name = name_token.text();
    decl->fun_decl.type_params = type_params;
    decl->fun_decl.params = alloc_span(params);
    decl->fun_decl.return_type = return_type;
    decl->fun_decl.body = body;
    decl->fun_decl.is_pub = is_pub;
    decl->fun_decl.is_native = is_native;
    return decl;
}

Decl* Parser::method_declaration(bool is_pub, bool is_native,
                                  Token struct_token, Span<TypeParam> type_params) {
    SourceLocation loc = struct_token.loc;

    // Parse method name (after the dot).
    // Allow 'new' and 'delete' keywords as method names for constructor/destructor syntax.
    Token method_token;
    if (match(TokenKind::KwNew) || match(TokenKind::KwDelete)) {
        method_token = m_previous;
    } else {
        method_token = consume(TokenKind::Identifier, "Expected method name after '.'");
        if (m_has_error) return nullptr;
    }

    consume(TokenKind::LeftParen, "Expected '(' after method name");
    if (m_has_error) return nullptr;

    Vector<Param> params = parse_parameters();
    if (m_has_error) return nullptr;

    consume(TokenKind::RightParen, "Expected ')' after parameters");
    if (m_has_error) return nullptr;

    TypeExpr* return_type = nullptr;
    if (match(TokenKind::Colon)) {
        return_type = type_expression();
        if (m_has_error) return nullptr;
    }

    // Check for "for Trait" or "for Trait<Args>" clause
    StringView trait_name(nullptr, 0);
    Span<TypeExpr*> trait_type_args;
    if (match(TokenKind::KwFor)) {
        Token trait_token = consume(TokenKind::Identifier, "Expected trait name after 'for'");
        if (m_has_error) return nullptr;
        trait_name = trait_token.text();

        // Check for type args: for Trait<i32, f64>
        if (check(TokenKind::Less)) {
            trait_type_args = parse_type_args();
            if (m_has_error) return nullptr;
        }
    }

    // Handle body: native ends with ';', non-native may have body or ';'
    Stmt* body = nullptr;
    if (is_native) {
        consume(TokenKind::Semicolon, "Expected ';' after native method declaration");
        if (m_has_error) return nullptr;
    } else if (match(TokenKind::Semicolon)) {
        // No body - required trait method declaration
    } else {
        consume(TokenKind::LeftBrace, "Expected '{' before method body");
        if (m_has_error) return nullptr;

        body = block_statement();
        if (m_has_error) return nullptr;
    }

    Decl* decl = alloc<Decl>();
    decl->kind = AstKind::DeclMethod;
    decl->loc = loc;
    decl->method_decl.struct_name = struct_token.text();
    decl->method_decl.name = method_token.text();
    decl->method_decl.type_params = type_params;
    decl->method_decl.params = alloc_span(params);
    decl->method_decl.return_type = return_type;
    decl->method_decl.body = body;
    decl->method_decl.is_pub = is_pub;
    decl->method_decl.is_native = is_native;
    decl->method_decl.trait_name = trait_name;
    decl->method_decl.trait_type_args = trait_type_args;
    return decl;
}

bool Parser::parse_ctor_dtor_common(const char* kind_name, CtorDtorParsed& out) {
    // Parse struct name: StructName or StructName.name
    Token struct_token = consume(TokenKind::Identifier, "Expected struct name");
    if (m_has_error) return false;

    out.struct_name = struct_token.text();
    out.name = StringView(nullptr, 0);  // Empty for default

    // Check for named variant: StructName.name
    if (match(TokenKind::Dot)) {
        Token name_token = consume(TokenKind::Identifier, "Expected name after '.'");
        if (m_has_error) return false;
        out.name = name_token.text();
    }

    consume(TokenKind::LeftParen, "Expected '(' after name");
    if (m_has_error) return false;

    out.params = parse_parameters();
    if (m_has_error) return false;

    consume(TokenKind::RightParen, "Expected ')' after parameters");
    if (m_has_error) return false;

    consume(TokenKind::LeftBrace, "Expected '{' before body");
    if (m_has_error) return false;

    out.body = block_statement();
    if (m_has_error) return false;

    return true;
}

Decl* Parser::constructor_declaration(bool is_pub) {
    SourceLocation loc = m_previous.loc;

    CtorDtorParsed parsed;
    if (!parse_ctor_dtor_common("constructor", parsed)) {
        return nullptr;
    }

    Decl* decl = alloc<Decl>();
    decl->kind = AstKind::DeclConstructor;
    decl->loc = loc;
    decl->constructor_decl.struct_name = parsed.struct_name;
    decl->constructor_decl.name = parsed.name;
    decl->constructor_decl.params = alloc_span(parsed.params);
    decl->constructor_decl.body = parsed.body;
    decl->constructor_decl.is_pub = is_pub;
    return decl;
}

Decl* Parser::destructor_declaration(bool is_pub) {
    SourceLocation loc = m_previous.loc;

    CtorDtorParsed parsed;
    if (!parse_ctor_dtor_common("destructor", parsed)) {
        return nullptr;
    }

    Decl* decl = alloc<Decl>();
    decl->kind = AstKind::DeclDestructor;
    decl->loc = loc;
    decl->destructor_decl.struct_name = parsed.struct_name;
    decl->destructor_decl.name = parsed.name;
    decl->destructor_decl.params = alloc_span(parsed.params);
    decl->destructor_decl.body = parsed.body;
    decl->destructor_decl.is_pub = is_pub;
    return decl;
}

Decl* Parser::struct_declaration(bool is_pub) {
    SourceLocation loc = m_previous.loc;

    Token name_token = consume(TokenKind::Identifier, "Expected struct name");
    if (m_has_error) return nullptr;

    // Check for generic type params: struct Name<T, U>
    Span<TypeParam> type_params;
    if (check(TokenKind::Less)) {
        type_params = parse_type_params();
        if (m_has_error) return nullptr;
    }

    StringView parent_name(nullptr, 0);
    if (match(TokenKind::Colon)) {
        Token parent_token = consume(TokenKind::Identifier, "Expected parent struct name");
        if (m_has_error) return nullptr;
        parent_name = parent_token.text();
    }

    consume(TokenKind::LeftBrace, "Expected '{' before struct body");
    if (m_has_error) return nullptr;

    Vector<FieldDecl> fields;
    Vector<WhenFieldDecl> when_clauses;
    Vector<FunDecl*> methods;

    while (!check(TokenKind::RightBrace) && !is_at_end()) {
        bool member_is_pub = match(TokenKind::KwPub);
        bool member_is_native = match(TokenKind::KwNative);

        if (match(TokenKind::KwFun)) {
            // Method
            Decl* method_decl = fun_declaration(member_is_pub, member_is_native);
            if (m_has_error) return nullptr;
            methods.push_back(&method_decl->fun_decl);
        } else if (match(TokenKind::KwWhen)) {
            // When clause (tagged union discriminant)
            if (member_is_native || member_is_pub) {
                report_error("'when' cannot have 'pub' or 'native' modifiers");
                return nullptr;
            }
            WhenFieldDecl when_decl = parse_when_field_decl();
            if (m_has_error) return nullptr;
            when_clauses.push_back(when_decl);
        } else {
            if (member_is_native) {
                report_error("'native' can only precede 'fun'");
                return nullptr;
            }

            // Field
            Token field_name = consume(TokenKind::Identifier, "Expected field name");
            if (m_has_error) return nullptr;

            consume(TokenKind::Colon, "Expected ':' after field name");
            if (m_has_error) return nullptr;

            TypeExpr* field_type = type_expression();
            if (m_has_error) return nullptr;

            Expr* default_value = nullptr;
            if (match(TokenKind::Equal)) {
                default_value = expression();
                if (m_has_error) return nullptr;
            }

            consume(TokenKind::Semicolon, "Expected ';' after field declaration");
            if (m_has_error) return nullptr;

            FieldDecl field;
            field.name = field_name.text();
            field.type = field_type;
            field.default_value = default_value;
            field.is_pub = member_is_pub;
            field.loc = field_name.loc;
            fields.push_back(field);
        }
    }

    consume(TokenKind::RightBrace, "Expected '}' after struct body");
    if (m_has_error) return nullptr;

    Decl* decl = alloc<Decl>();
    decl->kind = AstKind::DeclStruct;
    decl->loc = loc;
    decl->struct_decl.name = name_token.text();
    decl->struct_decl.type_params = type_params;
    decl->struct_decl.parent_name = parent_name;
    decl->struct_decl.fields = alloc_span(fields);
    decl->struct_decl.when_clauses = alloc_span(when_clauses);
    decl->struct_decl.methods = alloc_span(methods);
    decl->struct_decl.is_pub = is_pub;
    return decl;
}

WhenFieldDecl Parser::parse_when_field_decl() {
    // 'when' has already been consumed
    SourceLocation loc = m_previous.loc;

    // Parse discriminant: name: EnumType
    Token discrim_token = consume(TokenKind::Identifier, "Expected discriminant name after 'when'");
    if (m_has_error) return WhenFieldDecl{};

    consume(TokenKind::Colon, "Expected ':' after discriminant name");
    if (m_has_error) return WhenFieldDecl{};

    TypeExpr* discrim_type = type_expression();
    if (m_has_error) return WhenFieldDecl{};

    consume(TokenKind::LeftBrace, "Expected '{' after discriminant type");
    if (m_has_error) return WhenFieldDecl{};

    Vector<WhenCaseFieldDecl> cases;

    // Parse cases: case A, B: field: Type; ...
    while (!check(TokenKind::RightBrace) && !is_at_end()) {
        if (!match(TokenKind::KwCase)) {
            report_error("Expected 'case' in when clause");
            return WhenFieldDecl{};
        }

        WhenCaseFieldDecl case_decl;
        case_decl.loc = m_previous.loc;

        // Parse case names (comma-separated)
        Vector<StringView> case_names;
        do {
            Token name_token = consume(TokenKind::Identifier, "Expected case name");
            if (m_has_error) return WhenFieldDecl{};
            case_names.push_back(name_token.text());
        } while (match(TokenKind::Comma));

        consume(TokenKind::Colon, "Expected ':' after case name(s)");
        if (m_has_error) return WhenFieldDecl{};

        // Parse fields until next case or end of when clause
        Vector<FieldDecl> fields;
        while (!check(TokenKind::KwCase) && !check(TokenKind::RightBrace) && !is_at_end()) {
            Token field_name = consume(TokenKind::Identifier, "Expected field name");
            if (m_has_error) return WhenFieldDecl{};

            consume(TokenKind::Colon, "Expected ':' after field name");
            if (m_has_error) return WhenFieldDecl{};

            TypeExpr* field_type = type_expression();
            if (m_has_error) return WhenFieldDecl{};

            Expr* default_value = nullptr;
            if (match(TokenKind::Equal)) {
                default_value = expression();
                if (m_has_error) return WhenFieldDecl{};
            }

            consume(TokenKind::Semicolon, "Expected ';' after field declaration");
            if (m_has_error) return WhenFieldDecl{};

            FieldDecl field;
            field.name = field_name.text();
            field.type = field_type;
            field.default_value = default_value;
            field.is_pub = false;  // variant fields are not individually public
            field.loc = field_name.loc;
            fields.push_back(field);
        }

        case_decl.case_names = alloc_span(case_names);
        case_decl.fields = alloc_span(fields);
        cases.push_back(case_decl);
    }

    consume(TokenKind::RightBrace, "Expected '}' after when clause");
    if (m_has_error) return WhenFieldDecl{};

    WhenFieldDecl when_decl;
    when_decl.discriminant_name = discrim_token.text();
    when_decl.discriminant_type = discrim_type;
    when_decl.cases = alloc_span(cases);
    when_decl.loc = loc;
    return when_decl;
}

Decl* Parser::enum_declaration(bool is_pub) {
    SourceLocation loc = m_previous.loc;

    Token name_token = consume(TokenKind::Identifier, "Expected enum name");
    if (m_has_error) return nullptr;

    consume(TokenKind::LeftBrace, "Expected '{' before enum body");
    if (m_has_error) return nullptr;

    Vector<EnumVariant> variants;

    if (!check(TokenKind::RightBrace)) {
        do {
            Token variant_name = consume(TokenKind::Identifier, "Expected enum variant name");
            if (m_has_error) return nullptr;

            Expr* value = nullptr;
            if (match(TokenKind::Equal)) {
                value = expression();
                if (m_has_error) return nullptr;
            }

            EnumVariant variant;
            variant.name = variant_name.text();
            variant.value = value;
            variant.loc = variant_name.loc;
            variants.push_back(variant);
        } while (match(TokenKind::Comma) && !check(TokenKind::RightBrace));
    }

    consume(TokenKind::RightBrace, "Expected '}' after enum body");
    if (m_has_error) return nullptr;

    Decl* decl = alloc<Decl>();
    decl->kind = AstKind::DeclEnum;
    decl->loc = loc;
    decl->enum_decl.name = name_token.text();
    decl->enum_decl.variants = alloc_span(variants);
    decl->enum_decl.is_pub = is_pub;
    return decl;
}

Decl* Parser::trait_declaration(bool is_pub) {
    SourceLocation loc = m_previous.loc;

    Token name_token = consume(TokenKind::Identifier, "Expected trait name");
    if (m_has_error) return nullptr;

    // Check for generic type params: trait Name<T, U>
    Span<TypeParam> type_params;
    if (check(TokenKind::Less)) {
        type_params = parse_type_params();
        if (m_has_error) return nullptr;
    }

    StringView parent_name(nullptr, 0);
    if (match(TokenKind::Colon)) {
        Token parent_token = consume(TokenKind::Identifier, "Expected parent trait name");
        if (m_has_error) return nullptr;
        parent_name = parent_token.text();
    }

    consume(TokenKind::Semicolon, "Expected ';' after trait declaration");
    if (m_has_error) return nullptr;

    Decl* decl = alloc<Decl>();
    decl->kind = AstKind::DeclTrait;
    decl->loc = loc;
    decl->trait_decl.name = name_token.text();
    decl->trait_decl.type_params = type_params;
    decl->trait_decl.parent_name = parent_name;
    decl->trait_decl.is_pub = is_pub;
    return decl;
}

StringView Parser::parse_module_path() {
    Token first = consume(TokenKind::Identifier, "Expected module name");
    if (m_has_error) return StringView();

    // Check if this is a simple single-segment path
    if (!check(TokenKind::Dot)) {
        return first.text();
    }

    // Collect segments and join with dots
    Vector<StringView> segments;
    segments.push_back(first.text());

    while (match(TokenKind::Dot)) {
        Token seg = consume(TokenKind::Identifier, "Expected identifier after '.'");
        if (m_has_error) return StringView();
        segments.push_back(seg.text());
    }

    // Calculate total length needed for "a.b.c" string
    u32 total_len = 0;
    for (u32 i = 0; i < segments.size(); i++) {
        total_len += segments[i].size();
        if (i > 0) total_len += 1;  // dots between segments
    }

    // Allocate and build the joined path string
    char* buf = reinterpret_cast<char*>(m_allocator.alloc_bytes(total_len + 1, 1));
    u32 pos = 0;
    for (u32 i = 0; i < segments.size(); i++) {
        if (i > 0) buf[pos++] = '.';
        memcpy(buf + pos, segments[i].data(), segments[i].size());
        pos += segments[i].size();
    }
    buf[pos] = '\0';

    return StringView(buf, total_len);
}

Decl* Parser::import_declaration() {
    SourceLocation loc = m_previous.loc;
    bool is_from_import = m_previous.kind == TokenKind::KwFrom;

    if (is_from_import) {
        // from pkg.subpkg import name1, name2;
        StringView module_path = parse_module_path();
        if (m_has_error) return nullptr;

        consume(TokenKind::KwImport, "Expected 'import' after module path");
        if (m_has_error) return nullptr;

        Vector<ImportName> names;
        do {
            Token name_token = consume(TokenKind::Identifier, "Expected import name");
            if (m_has_error) return nullptr;

            ImportName import_name;
            import_name.name = name_token.text();
            import_name.alias = StringView(nullptr, 0);  // Initialize alias to empty
            import_name.loc = name_token.loc;

            // Check for alias: "as alias_name"
            if (check(TokenKind::Identifier) && m_current.length == 2 &&
                m_current.start[0] == 'a' && m_current.start[1] == 's') {
                advance();  // consume "as"
                Token alias_token = consume(TokenKind::Identifier, "Expected alias name after 'as'");
                if (m_has_error) return nullptr;
                import_name.alias = alias_token.text();
            }

            names.push_back(import_name);
        } while (match(TokenKind::Comma));

        consume(TokenKind::Semicolon, "Expected ';' after import");
        if (m_has_error) return nullptr;

        Decl* decl = alloc<Decl>();
        decl->kind = AstKind::DeclImport;
        decl->loc = loc;
        decl->import_decl.module_path = module_path;
        decl->import_decl.names = alloc_span(names);
        decl->import_decl.is_from_import = true;
        return decl;
    } else {
        // import pkg.subpkg;
        StringView module_path = parse_module_path();
        if (m_has_error) return nullptr;

        consume(TokenKind::Semicolon, "Expected ';' after import");
        if (m_has_error) return nullptr;

        Decl* decl = alloc<Decl>();
        decl->kind = AstKind::DeclImport;
        decl->loc = loc;
        decl->import_decl.module_path = module_path;
        decl->import_decl.names = Span<ImportName>(nullptr, 0);
        decl->import_decl.is_from_import = false;
        return decl;
    }
}

// Parser state save/restore for trial parsing

Parser::SavedState Parser::save_state() {
    SavedState state;
    state.current = m_current;
    state.previous = m_previous;
    state.lexer_pos = m_lexer.save_position();
    state.has_error = m_has_error;
    return state;
}

void Parser::restore_state(const SavedState& state) {
    m_current = state.current;
    m_previous = state.previous;
    m_lexer.restore_position(state.lexer_pos);
    m_has_error = state.has_error;
    m_error = {};
}

// Generic type parameter/argument parsing

Span<TypeParam> Parser::parse_type_params() {
    // Consume '<'
    consume(TokenKind::Less, "Expected '<' for type parameters");
    if (m_has_error) return {};

    Vector<TypeParam> params;
    do {
        Token name_token = consume(TokenKind::Identifier, "Expected type parameter name");
        if (m_has_error) return {};

        TypeParam type_param;
        type_param.name = name_token.text();
        type_param.loc = name_token.loc;
        type_param.bounds = {};

        // Parse optional trait bounds: <T: Trait1 + Trait2<Args>>
        if (match(TokenKind::Colon)) {
            Vector<TypeExpr*> bounds;
            do {
                TypeExpr* bound = type_expression();
                if (m_has_error) return {};
                bounds.push_back(bound);
            } while (match(TokenKind::Plus));
            type_param.bounds = alloc_span(bounds);
        }

        params.push_back(type_param);
    } while (match(TokenKind::Comma));

    if (!consume_closing_angle()) return {};

    return alloc_span(params);
}

Span<TypeExpr*> Parser::parse_type_args() {
    // Consume '<'
    consume(TokenKind::Less, "Expected '<' for type arguments");
    if (m_has_error) return {};

    Vector<TypeExpr*> args;
    do {
        TypeExpr* type = type_expression();
        if (m_has_error) return {};
        args.push_back(type);
    } while (match(TokenKind::Comma));

    if (!consume_closing_angle()) return {};

    return alloc_span(args);
}

Span<TypeExpr*> Parser::try_parse_generic_args() {
    // Trial parse: save state, try to parse <type_args>, check if followed by ( or {
    SavedState saved = save_state();

    // Consume '<'
    advance(); // consume Less token

    Vector<TypeExpr*> args;
    // Try parsing comma-separated type expressions
    do {
        TypeExpr* type = type_expression();
        if (m_has_error) {
            // Parse failed - restore and return empty
            restore_state(saved);
            return {};
        }
        args.push_back(type);
    } while (match(TokenKind::Comma));

    // Check for '>' (also handle >> splitting for nested generics)
    if (check(TokenKind::Greater)) {
        advance();
    } else if (check(TokenKind::GreaterGreater)) {
        // Split >>: consume first > and rewrite current to >
        m_previous = m_current;
        m_previous.length = 1;
        m_current.kind = TokenKind::Greater;
        m_current.start = m_current.start + 1;
        m_current.length = 1;
        m_current.loc.column += 1;
        m_current.loc.offset += 1;
    } else if (check(TokenKind::GreaterGreaterEqual)) {
        // Split >>=: consume first > and rewrite current to >=
        m_previous = m_current;
        m_previous.length = 1;
        m_current.kind = TokenKind::GreaterEqual;
        m_current.start = m_current.start + 1;
        m_current.length = 2;
        m_current.loc.column += 1;
        m_current.loc.offset += 1;
    } else {
        restore_state(saved);
        return {};
    }

    // Check if followed by '(' or '{' - confirms this is generic args, not comparison
    if (check(TokenKind::LeftParen) || check(TokenKind::LeftBrace)) {
        // Commit - return the parsed type args
        return alloc_span(args);
    }

    // Not followed by ( or { - this was a comparison, backtrack
    restore_state(saved);
    return {};
}

// Closing angle bracket helper for nested generics (e.g., List<List<i32>>)
// Handles >> by splitting: consumes first > and modifies current token to >
bool Parser::consume_closing_angle() {
    if (match(TokenKind::Greater)) {
        return true;
    }
    if (check(TokenKind::GreaterGreater)) {
        // Split >> into > + >: consume one > and rewrite current token to >
        m_previous = m_current;
        m_previous.length = 1;  // Just the first >
        m_current.kind = TokenKind::Greater;
        m_current.start = m_current.start + 1;
        m_current.length = 1;
        m_current.loc.column += 1;
        m_current.loc.offset += 1;
        return true;
    }
    if (check(TokenKind::GreaterGreaterEqual)) {
        // Split >>= into > + >=: consume one > and rewrite current token to >=
        m_previous = m_current;
        m_previous.length = 1;
        m_current.kind = TokenKind::GreaterEqual;
        m_current.start = m_current.start + 1;
        m_current.length = 2;
        m_current.loc.column += 1;
        m_current.loc.offset += 1;
        return true;
    }
    report_error("Expected '>' after type parameters");
    return false;
}

// Type expression parsing

TypeExpr* Parser::type_expression() {
    TypeExpr* type = alloc<TypeExpr>();
    type->ref_kind = RefKind::None;
    type->type_args = Span<TypeExpr*>();

    // Check for reference modifiers
    if (match(TokenKind::KwUniq)) {
        type->ref_kind = RefKind::Uniq;
    } else if (match(TokenKind::KwRef)) {
        type->ref_kind = RefKind::Ref;
    } else if (match(TokenKind::KwWeak)) {
        type->ref_kind = RefKind::Weak;
    }

    Token name_token = consume(TokenKind::Identifier, "Expected type name");
    if (m_has_error) return nullptr;

    type->name = name_token.text();
    type->loc = name_token.loc;

    // Check for generic type args: Type<i32, string>
    // Unambiguous here since type_expression() is only called in type position
    if (check(TokenKind::Less)) {
        type->type_args = parse_type_args();
        if (m_has_error) return nullptr;
    }

    return type;
}

StringView Parser::process_fstring_part(const Token& token) {
    // Token kinds and their raw text:
    // FStringBegin: f"text{   — strip f" prefix and { suffix
    // FStringMid:   }text{    — strip } prefix and { suffix
    // FStringEnd:   }text"    — strip } prefix and " suffix
    const char* src = token.start;
    u32 src_len = token.length;

    // Determine where the actual text content starts and ends
    u32 content_start = 0;
    u32 content_end = src_len;

    if (token.kind == TokenKind::FStringBegin) {
        // f"text{ — skip f" at start, { at end
        content_start = 2;
        content_end = src_len - 1;
    } else if (token.kind == TokenKind::FStringMid) {
        // }text{ — skip } at start, { at end
        content_start = 1;
        content_end = src_len - 1;
    } else if (token.kind == TokenKind::FStringEnd) {
        // }text" — skip } at start, " at end
        content_start = 1;
        content_end = src_len - 1;
    }

    if (content_start >= content_end) {
        return StringView("", 0);
    }

    u32 max_len = content_end - content_start;
    char* buf = reinterpret_cast<char*>(m_allocator.alloc_bytes(max_len + 1, 1));
    if (!buf) return StringView();

    u32 out_idx = 0;
    for (u32 i = content_start; i < content_end; i++) {
        if (src[i] == '\\' && i + 1 < content_end) {
            i++;
            switch (src[i]) {
                case 'n': buf[out_idx++] = '\n'; break;
                case 't': buf[out_idx++] = '\t'; break;
                case 'r': buf[out_idx++] = '\r'; break;
                case '\\': buf[out_idx++] = '\\'; break;
                case '"': buf[out_idx++] = '"'; break;
                case '0': buf[out_idx++] = '\0'; break;
                case '{': buf[out_idx++] = '{'; break;
                case '}': buf[out_idx++] = '}'; break;
                default:
                    buf[out_idx++] = '\\';
                    buf[out_idx++] = src[i];
                    break;
            }
        } else {
            buf[out_idx++] = src[i];
        }
    }

    return StringView(buf, out_idx);
}

StringView Parser::process_string_literal(const Token& token) {
    // Token includes the quotes: "content"
    // We need to strip quotes and process escape sequences
    const char* src = token.start;
    u32 src_len = token.length;

    if (src_len < 2 || src[0] != '"' || src[src_len - 1] != '"') {
        // Invalid string literal format
        return StringView(src, src_len);
    }

    // Calculate the max output size (same as input without quotes)
    u32 max_len = src_len - 2;

    // Allocate buffer for processed string
    char* buf = reinterpret_cast<char*>(m_allocator.alloc_bytes(max_len + 1, 1));
    if (!buf) {
        return StringView();
    }

    u32 out_idx = 0;
    for (u32 i = 1; i < src_len - 1; i++) {
        if (src[i] == '\\' && i + 1 < src_len - 1) {
            // Escape sequence
            i++;
            switch (src[i]) {
                case 'n': buf[out_idx++] = '\n'; break;
                case 't': buf[out_idx++] = '\t'; break;
                case 'r': buf[out_idx++] = '\r'; break;
                case '\\': buf[out_idx++] = '\\'; break;
                case '"': buf[out_idx++] = '"'; break;
                case '0': buf[out_idx++] = '\0'; break;
                case '{': buf[out_idx++] = '{'; break;
                case '}': buf[out_idx++] = '}'; break;
                default:
                    // Unknown escape - keep as-is
                    buf[out_idx++] = '\\';
                    buf[out_idx++] = src[i];
                    break;
            }
        } else {
            buf[out_idx++] = src[i];
        }
    }

    return StringView(buf, out_idx);
}

}
