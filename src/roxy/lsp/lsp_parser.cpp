#include "roxy/lsp/lsp_parser.hpp"
#include "roxy/core/format.hpp"

#include <cstring>

namespace rx {

// --- Infix operator table (same as compiler parser) ---

struct InfixRule {
    u8 precedence;  // 0 means not an infix operator
    bool right_assoc;
};

static InfixRule get_infix_rule(TokenKind kind) {
    switch (kind) {
        case TokenKind::PipePipe:            return {1, false};
        case TokenKind::AmpAmp:              return {2, false};
        case TokenKind::Pipe:                return {3, false};
        case TokenKind::Caret:               return {4, false};
        case TokenKind::Amp:                 return {5, false};
        case TokenKind::EqualEqual:          return {6, false};
        case TokenKind::BangEqual:           return {6, false};
        case TokenKind::Less:                return {7, false};
        case TokenKind::LessEqual:           return {7, false};
        case TokenKind::Greater:             return {7, false};
        case TokenKind::GreaterEqual:        return {7, false};
        case TokenKind::LessLess:            return {8, false};
        case TokenKind::GreaterGreater:      return {8, false};
        case TokenKind::Plus:                return {9, false};
        case TokenKind::Minus:               return {9, false};
        case TokenKind::Star:                return {10, false};
        case TokenKind::Slash:               return {10, false};
        case TokenKind::Percent:             return {10, false};
        default:                             return {0, false};
    }
}

static bool is_assignment_op(TokenKind kind) {
    switch (kind) {
        case TokenKind::Equal:
        case TokenKind::PlusEqual:
        case TokenKind::MinusEqual:
        case TokenKind::StarEqual:
        case TokenKind::SlashEqual:
        case TokenKind::PercentEqual:
        case TokenKind::AmpEqual:
        case TokenKind::PipeEqual:
        case TokenKind::CaretEqual:
        case TokenKind::LessLessEqual:
        case TokenKind::GreaterGreaterEqual:
            return true;
        default:
            return false;
    }
}

// --- Constructor ---

LspParser::LspParser(Lexer& lexer, BumpAllocator& allocator)
    : m_lexer(lexer)
    , m_allocator(allocator)
    , m_source(lexer.source())
    , m_source_length(lexer.length())
{
    m_current = m_lexer.next_token();
    m_previous = m_current;
}

// --- Token operations ---

void LspParser::advance() {
    m_previous = m_current;
    m_current = m_lexer.next_token();

    // Lexer error tokens become diagnostics but don't stop parsing
    if (m_current.kind == TokenKind::Error) {
        add_diagnostic(
            TextRange{m_current.loc.offset, m_current.loc.offset + m_current.length},
            m_current.start);
        // Advance past the error token
        m_previous = m_current;
        m_current = m_lexer.next_token();
    }
}

bool LspParser::check(TokenKind kind) const {
    return m_current.kind == kind;
}

bool LspParser::match(TokenKind kind) {
    if (!check(kind)) return false;
    advance();
    return true;
}

bool LspParser::is_at_end() const {
    return m_current.kind == TokenKind::Eof;
}

Token LspParser::consume_or_synthetic(TokenKind expected, const char* message) {
    if (check(expected)) {
        Token token = m_current;
        advance();
        return token;
    }

    // Create a synthetic zero-width token at current position
    add_diagnostic(
        TextRange{m_current.loc.offset, m_current.loc.offset + m_current.length},
        message);

    Token synthetic;
    synthetic.kind = expected;
    synthetic.loc = m_current.loc;
    synthetic.start = m_current.start;
    synthetic.length = 0;
    synthetic.int_value = 0;
    return synthetic;
}

// --- Error recovery ---

void LspParser::synchronize_to_statement_boundary() {
    while (!is_at_end()) {
        // Stop after semicolons
        if (m_previous.kind == TokenKind::Semicolon) return;

        // Stop at statement/declaration-starting keywords
        if (is_statement_start(m_current.kind)) return;

        // Stop at closing brace
        if (check(TokenKind::RightBrace)) return;

        advance();
    }
}

void LspParser::skip_to_closing_bracket(TokenKind open, TokenKind close) {
    i32 depth = 1;
    while (!is_at_end() && depth > 0) {
        if (check(open)) depth++;
        else if (check(close)) depth--;
        if (depth > 0) advance();
    }
}

SyntaxNode* LspParser::make_error_node(const char* message) {
    SyntaxNode* node = m_allocator.emplace<SyntaxNode>();
    node->kind = SyntaxKind::Error;
    node->range = TextRange{m_current.loc.offset, m_current.loc.offset + m_current.length};
    node->parent = nullptr;
    node->children = Span<SyntaxNode*>();
    node->token = Token{};
    node->error_message = message;

    add_diagnostic(node->range, message);
    return node;
}

void LspParser::add_diagnostic(TextRange range, const char* message) {
    ParseDiagnostic diagnostic;
    diagnostic.range = range;
    diagnostic.message = String(message);
    m_diagnostics.push_back(std::move(diagnostic));
}

// --- Node construction ---

LspParser::NodeBuilder LspParser::begin_node(SyntaxKind kind) {
    NodeBuilder builder;
    builder.kind = kind;
    builder.start_offset = m_current.loc.offset;
    return builder;
}

SyntaxNode* LspParser::finish_node(NodeBuilder& builder) {
    u32 end_offset = m_previous.loc.offset + m_previous.length;
    if (end_offset < builder.start_offset) {
        end_offset = builder.start_offset;
    }

    SyntaxNode* node = m_allocator.emplace<SyntaxNode>();
    node->kind = builder.kind;
    node->range = TextRange{builder.start_offset, end_offset};
    node->parent = nullptr;
    node->children = alloc_span(builder.children);
    node->token = Token{};
    node->error_message = nullptr;

    // Set parent pointers on children
    for (u32 i = 0; i < node->children.size(); i++) {
        node->children[i]->parent = node;
    }

    return node;
}

SyntaxNode* LspParser::make_token_node(const Token& token) {
    SyntaxNode* node = m_allocator.emplace<SyntaxNode>();
    node->kind = token_kind_to_syntax_kind(token.kind);
    node->range = TextRange{token.loc.offset, token.loc.offset + token.length};
    node->parent = nullptr;
    node->children = Span<SyntaxNode*>();
    node->token = token;
    node->error_message = nullptr;
    return node;
}

template <typename T>
Span<T> LspParser::alloc_span(const Vector<T>& vec) {
    if (vec.empty()) return Span<T>(nullptr, 0);
    T* data = reinterpret_cast<T*>(m_allocator.alloc_bytes(sizeof(T) * vec.size(), alignof(T)));
    for (u32 i = 0; i < vec.size(); i++) {
        new (data + i) T(vec[i]);
    }
    return Span<T>(data, vec.size());
}

// --- Parser state save/restore ---

LspParser::SavedState LspParser::save_state() {
    SavedState state;
    state.current = m_current;
    state.previous = m_previous;
    state.lexer_pos = m_lexer.save_position();
    state.diagnostic_count = m_diagnostics.size();
    return state;
}

void LspParser::restore_state(const SavedState& state) {
    m_current = state.current;
    m_previous = state.previous;
    m_lexer.restore_position(state.lexer_pos);
    // Remove any diagnostics added during trial parse
    while (m_diagnostics.size() > state.diagnostic_count) {
        m_diagnostics.pop_back();
    }
}

// --- Helpers ---

bool LspParser::is_statement_start(TokenKind kind) const {
    switch (kind) {
        case TokenKind::KwVar:
        case TokenKind::KwFun:
        case TokenKind::KwStruct:
        case TokenKind::KwEnum:
        case TokenKind::KwTrait:
        case TokenKind::KwPub:
        case TokenKind::KwNative:
        case TokenKind::KwIf:
        case TokenKind::KwFor:
        case TokenKind::KwWhile:
        case TokenKind::KwReturn:
        case TokenKind::KwBreak:
        case TokenKind::KwContinue:
        case TokenKind::KwDelete:
        case TokenKind::KwWhen:
        case TokenKind::KwThrow:
        case TokenKind::KwTry:
        case TokenKind::KwImport:
        case TokenKind::KwFrom:
            return true;
        default:
            return false;
    }
}

// --- Top-level parse ---

SyntaxTree LspParser::parse() {
    SyntaxNode* root = parse_program();

    SyntaxTree tree;
    tree.root = root;
    tree.diagnostics = std::move(m_diagnostics);
    tree.source = m_source;
    tree.source_length = m_source_length;
    return tree;
}

SyntaxNode* LspParser::parse_program() {
    auto builder = begin_node(SyntaxKind::NodeProgram);

    while (!is_at_end()) {
        SyntaxNode* decl = parse_declaration();
        if (decl) {
            builder.children.push_back(decl);
        }
    }

    return finish_node(builder);
}

// --- Declaration parsing ---

SyntaxNode* LspParser::parse_declaration() {
    DeclModifiers mods;

    // Consume pub if present
    if (match(TokenKind::KwPub)) {
        mods.has_pub = true;
        mods.pub_token = m_previous;
    }

    if (match(TokenKind::KwVar)) {
        return parse_var_decl(mods);
    }

    if (match(TokenKind::KwNative)) {
        mods.has_native = true;
        mods.native_token = m_previous;
    }

    if (match(TokenKind::KwFun)) {
        if (match(TokenKind::KwNew)) {
            if (mods.has_native) {
                add_diagnostic(
                    TextRange{m_previous.loc.offset, m_previous.loc.offset + m_previous.length},
                    "constructors cannot be 'native'");
            }
            return parse_constructor_decl(mods);
        }
        if (match(TokenKind::KwDelete)) {
            if (mods.has_native) {
                add_diagnostic(
                    TextRange{m_previous.loc.offset, m_previous.loc.offset + m_previous.length},
                    "destructors cannot be 'native'");
            }
            return parse_destructor_decl(mods);
        }
        return parse_fun_decl(mods);
    }

    if (mods.has_native) {
        add_diagnostic(
            TextRange{m_current.loc.offset, m_current.loc.offset + m_current.length},
            "'native' can only precede 'fun'");
        synchronize_to_statement_boundary();
        return make_error_node("'native' can only precede 'fun'");
    }

    if (match(TokenKind::KwStruct)) {
        return parse_struct_decl(mods);
    }

    if (match(TokenKind::KwEnum)) {
        return parse_enum_decl(mods);
    }

    if (match(TokenKind::KwTrait)) {
        return parse_trait_decl(mods);
    }

    if (mods.has_pub) {
        add_diagnostic(
            TextRange{m_current.loc.offset, m_current.loc.offset + m_current.length},
            "'pub' can only precede declarations");
        synchronize_to_statement_boundary();
        return make_error_node("'pub' can only precede declarations");
    }

    if (match(TokenKind::KwImport) || match(TokenKind::KwFrom)) {
        return parse_import_decl();
    }

    // Statement (wrapped as a declaration)
    return parse_statement();
}

void LspParser::insert_modifier_children(NodeBuilder& builder, const DeclModifiers& mods) {
    if (mods.has_pub) {
        builder.children.push_back(make_token_node(mods.pub_token));
        builder.start_offset = mods.pub_token.loc.offset;
    }
    if (mods.has_native) {
        builder.children.push_back(make_token_node(mods.native_token));
        if (!mods.has_pub) {
            builder.start_offset = mods.native_token.loc.offset;
        }
    }
}

SyntaxNode* LspParser::parse_var_decl(const DeclModifiers& mods) {
    // 'var' already consumed
    auto builder = begin_node(SyntaxKind::NodeVarDecl);
    builder.start_offset = m_previous.loc.offset; // start from 'var' keyword
    insert_modifier_children(builder, mods);

    // Name
    Token name_token = consume_or_synthetic(TokenKind::Identifier, "Expected variable name");
    builder.children.push_back(make_token_node(name_token));

    // Optional type annotation
    if (match(TokenKind::Colon)) {
        builder.children.push_back(make_token_node(m_previous)); // ':'
        SyntaxNode* type_node = parse_type_expr();
        builder.children.push_back(type_node);
    }

    // Optional initializer
    if (match(TokenKind::Equal)) {
        builder.children.push_back(make_token_node(m_previous)); // '='
        SyntaxNode* init_expr = parse_expression();
        builder.children.push_back(init_expr);
    }

    Token semi = consume_or_synthetic(TokenKind::Semicolon, "Expected ';' after variable declaration");
    builder.children.push_back(make_token_node(semi));

    return finish_node(builder);
}

SyntaxNode* LspParser::parse_fun_decl(const DeclModifiers& mods) {
    // 'fun' already consumed
    auto builder = begin_node(SyntaxKind::NodeFunDecl);
    builder.start_offset = m_previous.loc.offset;
    insert_modifier_children(builder, mods);

    Token name_token = consume_or_synthetic(TokenKind::Identifier, "Expected function name");
    builder.children.push_back(make_token_node(name_token));

    // Optional type params: <T, U>
    if (check(TokenKind::Less)) {
        SyntaxNode* type_params = parse_type_params();
        builder.children.push_back(type_params);
    }

    // Check for method syntax: fun Name.method() or fun Name<T>.method()
    if (match(TokenKind::Dot)) {
        builder.children.push_back(make_token_node(m_previous)); // '.'
        return parse_method_decl(builder, mods);
    }

    // Regular function
    Token lparen = consume_or_synthetic(TokenKind::LeftParen, "Expected '(' after function name");
    builder.children.push_back(make_token_node(lparen));

    SyntaxNode* params = parse_param_list();
    builder.children.push_back(params);

    Token rparen = consume_or_synthetic(TokenKind::RightParen, "Expected ')' after parameters");
    builder.children.push_back(make_token_node(rparen));

    // Optional return type
    if (match(TokenKind::Colon)) {
        builder.children.push_back(make_token_node(m_previous)); // ':'
        SyntaxNode* return_type = parse_type_expr();
        builder.children.push_back(return_type);
    }

    // Body or semicolon
    if (mods.has_native) {
        Token semi = consume_or_synthetic(TokenKind::Semicolon, "Expected ';' after native function declaration");
        builder.children.push_back(make_token_node(semi));
    } else {
        if (check(TokenKind::LeftBrace)) {
            advance();
            builder.children.push_back(make_token_node(m_previous)); // '{'
            SyntaxNode* body = parse_block_stmt();
            builder.children.push_back(body);
        } else {
            Token lbrace = consume_or_synthetic(TokenKind::LeftBrace, "Expected '{' before function body");
            builder.children.push_back(make_token_node(lbrace));
        }
    }

    return finish_node(builder);
}

SyntaxNode* LspParser::parse_method_decl(NodeBuilder& builder, const DeclModifiers& mods) {
    // Already in the builder: modifier tokens, struct_name, optional type_params, '.'
    builder.kind = SyntaxKind::NodeMethodDecl;

    // Method name - allow 'new' and 'delete' as method names
    if (match(TokenKind::KwNew) || match(TokenKind::KwDelete)) {
        builder.children.push_back(make_token_node(m_previous));
    } else {
        Token method_name = consume_or_synthetic(TokenKind::Identifier, "Expected method name after '.'");
        builder.children.push_back(make_token_node(method_name));
    }

    Token lparen = consume_or_synthetic(TokenKind::LeftParen, "Expected '(' after method name");
    builder.children.push_back(make_token_node(lparen));

    SyntaxNode* params = parse_param_list();
    builder.children.push_back(params);

    Token rparen = consume_or_synthetic(TokenKind::RightParen, "Expected ')' after parameters");
    builder.children.push_back(make_token_node(rparen));

    // Optional return type
    if (match(TokenKind::Colon)) {
        builder.children.push_back(make_token_node(m_previous));
        SyntaxNode* return_type = parse_type_expr();
        builder.children.push_back(return_type);
    }

    // Check for "for Trait" or "for Trait<Args>" clause
    if (match(TokenKind::KwFor)) {
        builder.children.push_back(make_token_node(m_previous)); // 'for'
        Token trait_name = consume_or_synthetic(TokenKind::Identifier, "Expected trait name after 'for'");
        builder.children.push_back(make_token_node(trait_name));

        if (check(TokenKind::Less)) {
            SyntaxNode* trait_type_args = parse_type_args();
            builder.children.push_back(trait_type_args);
        }
    }

    // Body, semicolon, or nothing (required trait method)
    if (mods.has_native) {
        Token semi = consume_or_synthetic(TokenKind::Semicolon, "Expected ';' after native method declaration");
        builder.children.push_back(make_token_node(semi));
    } else if (match(TokenKind::Semicolon)) {
        builder.children.push_back(make_token_node(m_previous)); // ';'
    } else if (check(TokenKind::LeftBrace)) {
        advance();
        builder.children.push_back(make_token_node(m_previous)); // '{'
        SyntaxNode* body = parse_block_stmt();
        builder.children.push_back(body);
    } else {
        Token lbrace = consume_or_synthetic(TokenKind::LeftBrace, "Expected '{' before method body");
        builder.children.push_back(make_token_node(lbrace));
    }

    return finish_node(builder);
}

SyntaxNode* LspParser::parse_constructor_decl(const DeclModifiers& mods) {
    // 'new' already consumed
    auto builder = begin_node(SyntaxKind::NodeConstructorDecl);
    builder.start_offset = m_previous.loc.offset;
    insert_modifier_children(builder, mods);

    Token struct_name = consume_or_synthetic(TokenKind::Identifier, "Expected struct name");
    builder.children.push_back(make_token_node(struct_name));

    // Optional named variant: StructName.name
    if (match(TokenKind::Dot)) {
        builder.children.push_back(make_token_node(m_previous));
        Token ctor_name = consume_or_synthetic(TokenKind::Identifier, "Expected constructor name after '.'");
        builder.children.push_back(make_token_node(ctor_name));
    }

    Token lparen = consume_or_synthetic(TokenKind::LeftParen, "Expected '(' after name");
    builder.children.push_back(make_token_node(lparen));

    SyntaxNode* params = parse_param_list();
    builder.children.push_back(params);

    Token rparen = consume_or_synthetic(TokenKind::RightParen, "Expected ')' after parameters");
    builder.children.push_back(make_token_node(rparen));

    if (check(TokenKind::LeftBrace)) {
        advance();
        builder.children.push_back(make_token_node(m_previous));
        SyntaxNode* body = parse_block_stmt();
        builder.children.push_back(body);
    } else {
        Token lbrace = consume_or_synthetic(TokenKind::LeftBrace, "Expected '{' before body");
        builder.children.push_back(make_token_node(lbrace));
    }

    return finish_node(builder);
}

SyntaxNode* LspParser::parse_destructor_decl(const DeclModifiers& mods) {
    // 'delete' already consumed
    auto builder = begin_node(SyntaxKind::NodeDestructorDecl);
    builder.start_offset = m_previous.loc.offset;
    insert_modifier_children(builder, mods);

    Token struct_name = consume_or_synthetic(TokenKind::Identifier, "Expected struct name");
    builder.children.push_back(make_token_node(struct_name));

    // Optional named variant: StructName.name
    if (match(TokenKind::Dot)) {
        builder.children.push_back(make_token_node(m_previous));
        Token dtor_name = consume_or_synthetic(TokenKind::Identifier, "Expected destructor name after '.'");
        builder.children.push_back(make_token_node(dtor_name));
    }

    Token lparen = consume_or_synthetic(TokenKind::LeftParen, "Expected '(' after name");
    builder.children.push_back(make_token_node(lparen));

    SyntaxNode* params = parse_param_list();
    builder.children.push_back(params);

    Token rparen = consume_or_synthetic(TokenKind::RightParen, "Expected ')' after parameters");
    builder.children.push_back(make_token_node(rparen));

    if (check(TokenKind::LeftBrace)) {
        advance();
        builder.children.push_back(make_token_node(m_previous));
        SyntaxNode* body = parse_block_stmt();
        builder.children.push_back(body);
    } else {
        Token lbrace = consume_or_synthetic(TokenKind::LeftBrace, "Expected '{' before body");
        builder.children.push_back(make_token_node(lbrace));
    }

    return finish_node(builder);
}

SyntaxNode* LspParser::parse_struct_decl(const DeclModifiers& mods) {
    // 'struct' already consumed
    auto builder = begin_node(SyntaxKind::NodeStructDecl);
    builder.start_offset = m_previous.loc.offset;
    insert_modifier_children(builder, mods);

    Token name_token = consume_or_synthetic(TokenKind::Identifier, "Expected struct name");
    builder.children.push_back(make_token_node(name_token));

    // Optional type params: <T, U>
    if (check(TokenKind::Less)) {
        SyntaxNode* type_params = parse_type_params();
        builder.children.push_back(type_params);
    }

    // Optional parent: : ParentName
    if (match(TokenKind::Colon)) {
        builder.children.push_back(make_token_node(m_previous));
        Token parent = consume_or_synthetic(TokenKind::Identifier, "Expected parent struct name");
        builder.children.push_back(make_token_node(parent));
    }

    Token lbrace = consume_or_synthetic(TokenKind::LeftBrace, "Expected '{' before struct body");
    builder.children.push_back(make_token_node(lbrace));

    // Parse struct body: fields, when clauses, methods
    while (!check(TokenKind::RightBrace) && !is_at_end()) {
        DeclModifiers member_mods;
        if (match(TokenKind::KwPub)) {
            member_mods.has_pub = true;
            member_mods.pub_token = m_previous;
        }
        if (match(TokenKind::KwNative)) {
            member_mods.has_native = true;
            member_mods.native_token = m_previous;
        }

        if (match(TokenKind::KwFun)) {
            SyntaxNode* method = parse_fun_decl(member_mods);
            builder.children.push_back(method);
        } else if (match(TokenKind::KwWhen)) {
            if (member_mods.has_native || member_mods.has_pub) {
                add_diagnostic(
                    TextRange{m_previous.loc.offset, m_previous.loc.offset + m_previous.length},
                    "'when' cannot have 'pub' or 'native' modifiers");
            }
            SyntaxNode* when_decl = parse_when_field_decl();
            builder.children.push_back(when_decl);
        } else if (check(TokenKind::Identifier)) {
            if (member_mods.has_native) {
                add_diagnostic(
                    TextRange{m_current.loc.offset, m_current.loc.offset + m_current.length},
                    "'native' can only precede 'fun'");
            }
            SyntaxNode* field = parse_field_decl(member_mods);
            builder.children.push_back(field);
        } else {
            // Unrecognized token in struct body — skip and report error
            builder.children.push_back(make_error_node("Expected field, method, or 'when' in struct body"));
            advance();
        }
    }

    Token rbrace = consume_or_synthetic(TokenKind::RightBrace, "Expected '}' after struct body");
    builder.children.push_back(make_token_node(rbrace));

    return finish_node(builder);
}

SyntaxNode* LspParser::parse_field_decl(const DeclModifiers& mods) {
    auto builder = begin_node(SyntaxKind::NodeFieldDecl);
    insert_modifier_children(builder, mods);

    Token field_name = consume_or_synthetic(TokenKind::Identifier, "Expected field name");
    builder.children.push_back(make_token_node(field_name));

    Token colon = consume_or_synthetic(TokenKind::Colon, "Expected ':' after field name");
    builder.children.push_back(make_token_node(colon));

    SyntaxNode* field_type = parse_type_expr();
    builder.children.push_back(field_type);

    // Optional default value
    if (match(TokenKind::Equal)) {
        builder.children.push_back(make_token_node(m_previous));
        SyntaxNode* default_value = parse_expression();
        builder.children.push_back(default_value);
    }

    Token semi = consume_or_synthetic(TokenKind::Semicolon, "Expected ';' after field declaration");
    builder.children.push_back(make_token_node(semi));

    return finish_node(builder);
}

SyntaxNode* LspParser::parse_when_field_decl() {
    // 'when' already consumed
    auto builder = begin_node(SyntaxKind::NodeWhenFieldDecl);

    // Discriminant: name: EnumType
    Token discrim_name = consume_or_synthetic(TokenKind::Identifier, "Expected discriminant name after 'when'");
    builder.children.push_back(make_token_node(discrim_name));

    Token colon = consume_or_synthetic(TokenKind::Colon, "Expected ':' after discriminant name");
    builder.children.push_back(make_token_node(colon));

    SyntaxNode* discrim_type = parse_type_expr();
    builder.children.push_back(discrim_type);

    Token lbrace = consume_or_synthetic(TokenKind::LeftBrace, "Expected '{' after discriminant type");
    builder.children.push_back(make_token_node(lbrace));

    // Parse cases
    while (!check(TokenKind::RightBrace) && !is_at_end()) {
        if (!match(TokenKind::KwCase)) {
            add_diagnostic(
                TextRange{m_current.loc.offset, m_current.loc.offset + m_current.length},
                "Expected 'case' in when clause");
            synchronize_to_statement_boundary();
            continue;
        }

        auto case_builder = begin_node(SyntaxKind::NodeWhenCaseFieldDecl);
        case_builder.children.push_back(make_token_node(m_previous)); // 'case'

        // Case names (comma-separated)
        do {
            Token case_name = consume_or_synthetic(TokenKind::Identifier, "Expected case name");
            case_builder.children.push_back(make_token_node(case_name));
        } while (match(TokenKind::Comma));

        Token case_colon = consume_or_synthetic(TokenKind::Colon, "Expected ':' after case name(s)");
        case_builder.children.push_back(make_token_node(case_colon));

        // Parse fields until next case or end
        while (!check(TokenKind::KwCase) && !check(TokenKind::RightBrace) && !is_at_end()) {
            if (check(TokenKind::Identifier)) {
                DeclModifiers no_mods;
                SyntaxNode* field = parse_field_decl(no_mods);
                case_builder.children.push_back(field);
            } else {
                case_builder.children.push_back(make_error_node("Expected field declaration"));
                advance();
            }
        }

        builder.children.push_back(finish_node(case_builder));
    }

    Token rbrace = consume_or_synthetic(TokenKind::RightBrace, "Expected '}' after when clause");
    builder.children.push_back(make_token_node(rbrace));

    return finish_node(builder);
}

SyntaxNode* LspParser::parse_enum_decl(const DeclModifiers& mods) {
    // 'enum' already consumed
    auto builder = begin_node(SyntaxKind::NodeEnumDecl);
    builder.start_offset = m_previous.loc.offset;
    insert_modifier_children(builder, mods);

    Token name_token = consume_or_synthetic(TokenKind::Identifier, "Expected enum name");
    builder.children.push_back(make_token_node(name_token));

    Token lbrace = consume_or_synthetic(TokenKind::LeftBrace, "Expected '{' before enum body");
    builder.children.push_back(make_token_node(lbrace));

    // Parse variants
    if (!check(TokenKind::RightBrace)) {
        do {
            auto variant_builder = begin_node(SyntaxKind::NodeEnumVariant);

            Token variant_name = consume_or_synthetic(TokenKind::Identifier, "Expected enum variant name");
            variant_builder.children.push_back(make_token_node(variant_name));

            if (match(TokenKind::Equal)) {
                variant_builder.children.push_back(make_token_node(m_previous));
                SyntaxNode* value_expr = parse_expression();
                variant_builder.children.push_back(value_expr);
            }

            builder.children.push_back(finish_node(variant_builder));
        } while (match(TokenKind::Comma) && !check(TokenKind::RightBrace));
    }

    Token rbrace = consume_or_synthetic(TokenKind::RightBrace, "Expected '}' after enum body");
    builder.children.push_back(make_token_node(rbrace));

    return finish_node(builder);
}

SyntaxNode* LspParser::parse_trait_decl(const DeclModifiers& mods) {
    // 'trait' already consumed
    auto builder = begin_node(SyntaxKind::NodeTraitDecl);
    builder.start_offset = m_previous.loc.offset;
    insert_modifier_children(builder, mods);

    Token name_token = consume_or_synthetic(TokenKind::Identifier, "Expected trait name");
    builder.children.push_back(make_token_node(name_token));

    // Optional type params
    if (check(TokenKind::Less)) {
        SyntaxNode* type_params = parse_type_params();
        builder.children.push_back(type_params);
    }

    // Optional parent trait
    if (match(TokenKind::Colon)) {
        builder.children.push_back(make_token_node(m_previous));
        Token parent = consume_or_synthetic(TokenKind::Identifier, "Expected parent trait name");
        builder.children.push_back(make_token_node(parent));
    }

    Token semi = consume_or_synthetic(TokenKind::Semicolon, "Expected ';' after trait declaration");
    builder.children.push_back(make_token_node(semi));

    return finish_node(builder);
}

SyntaxNode* LspParser::parse_import_decl() {
    // 'import' or 'from' already consumed
    auto builder = begin_node(SyntaxKind::NodeImportDecl);
    builder.start_offset = m_previous.loc.offset;
    bool is_from_import = (m_previous.kind == TokenKind::KwFrom);
    builder.children.push_back(make_token_node(m_previous)); // 'import' or 'from'

    if (is_from_import) {
        // from pkg.subpkg import name1, name2;
        parse_module_path(builder);

        Token import_kw = consume_or_synthetic(TokenKind::KwImport, "Expected 'import' after module path");
        builder.children.push_back(make_token_node(import_kw));

        // Parse import names
        do {
            auto name_builder = begin_node(SyntaxKind::NodeImportName);
            Token name_token = consume_or_synthetic(TokenKind::Identifier, "Expected import name");
            name_builder.children.push_back(make_token_node(name_token));

            // Check for "as alias"
            if (check(TokenKind::Identifier) && m_current.length == 2 &&
                m_current.start[0] == 'a' && m_current.start[1] == 's') {
                advance(); // consume "as"
                name_builder.children.push_back(make_token_node(m_previous));
                Token alias_token = consume_or_synthetic(TokenKind::Identifier, "Expected alias name after 'as'");
                name_builder.children.push_back(make_token_node(alias_token));
            }

            builder.children.push_back(finish_node(name_builder));
        } while (match(TokenKind::Comma));
    } else {
        // import pkg.subpkg;
        parse_module_path(builder);
    }

    Token semi = consume_or_synthetic(TokenKind::Semicolon, "Expected ';' after import");
    builder.children.push_back(make_token_node(semi));

    return finish_node(builder);
}

SyntaxNode* LspParser::parse_module_path(NodeBuilder& builder) {
    Token first = consume_or_synthetic(TokenKind::Identifier, "Expected module name");
    builder.children.push_back(make_token_node(first));

    while (match(TokenKind::Dot)) {
        builder.children.push_back(make_token_node(m_previous)); // '.'
        Token seg = consume_or_synthetic(TokenKind::Identifier, "Expected identifier after '.'");
        builder.children.push_back(make_token_node(seg));
    }

    return nullptr; // path tokens added directly to builder
}

// --- Statement parsing ---

SyntaxNode* LspParser::parse_statement() {
    if (match(TokenKind::LeftBrace)) {
        return parse_block_stmt();
    }

    if (match(TokenKind::KwIf)) return parse_if_stmt();
    if (match(TokenKind::KwWhile)) return parse_while_stmt();
    if (match(TokenKind::KwFor)) return parse_for_stmt();
    if (match(TokenKind::KwReturn)) return parse_return_stmt();
    if (match(TokenKind::KwBreak)) return parse_break_stmt();
    if (match(TokenKind::KwContinue)) return parse_continue_stmt();
    if (match(TokenKind::KwDelete)) return parse_delete_stmt();
    if (match(TokenKind::KwWhen)) return parse_when_stmt();
    if (match(TokenKind::KwThrow)) return parse_throw_stmt();
    if (match(TokenKind::KwTry)) return parse_try_stmt();

    return parse_expr_stmt();
}

SyntaxNode* LspParser::parse_block_stmt() {
    // '{' already consumed (handled by caller or match)
    auto builder = begin_node(SyntaxKind::NodeBlockStmt);

    while (!check(TokenKind::RightBrace) && !is_at_end()) {
        SyntaxNode* decl = parse_declaration();
        if (decl) {
            builder.children.push_back(decl);
        }
    }

    Token rbrace = consume_or_synthetic(TokenKind::RightBrace, "Expected '}' after block");
    builder.children.push_back(make_token_node(rbrace));

    return finish_node(builder);
}

SyntaxNode* LspParser::parse_if_stmt() {
    // 'if' already consumed
    auto builder = begin_node(SyntaxKind::NodeIfStmt);
    builder.children.push_back(make_token_node(m_previous)); // 'if'

    Token lparen = consume_or_synthetic(TokenKind::LeftParen, "Expected '(' after 'if'");
    builder.children.push_back(make_token_node(lparen));

    SyntaxNode* condition = parse_expression();
    builder.children.push_back(condition);

    Token rparen = consume_or_synthetic(TokenKind::RightParen, "Expected ')' after if condition");
    builder.children.push_back(make_token_node(rparen));

    SyntaxNode* then_branch = parse_statement();
    builder.children.push_back(then_branch);

    if (match(TokenKind::KwElse)) {
        builder.children.push_back(make_token_node(m_previous)); // 'else'
        SyntaxNode* else_branch = parse_statement();
        builder.children.push_back(else_branch);
    }

    return finish_node(builder);
}

SyntaxNode* LspParser::parse_while_stmt() {
    // 'while' already consumed
    auto builder = begin_node(SyntaxKind::NodeWhileStmt);
    builder.children.push_back(make_token_node(m_previous)); // 'while'

    Token lparen = consume_or_synthetic(TokenKind::LeftParen, "Expected '(' after 'while'");
    builder.children.push_back(make_token_node(lparen));

    SyntaxNode* condition = parse_expression();
    builder.children.push_back(condition);

    Token rparen = consume_or_synthetic(TokenKind::RightParen, "Expected ')' after while condition");
    builder.children.push_back(make_token_node(rparen));

    SyntaxNode* body = parse_statement();
    builder.children.push_back(body);

    return finish_node(builder);
}

SyntaxNode* LspParser::parse_for_stmt() {
    // 'for' already consumed
    auto builder = begin_node(SyntaxKind::NodeForStmt);
    builder.children.push_back(make_token_node(m_previous)); // 'for'

    Token lparen = consume_or_synthetic(TokenKind::LeftParen, "Expected '(' after 'for'");
    builder.children.push_back(make_token_node(lparen));

    // Initializer
    if (match(TokenKind::Semicolon)) {
        builder.children.push_back(make_token_node(m_previous)); // ';'
    } else if (match(TokenKind::KwVar)) {
        DeclModifiers no_mods;
        SyntaxNode* var_decl = parse_var_decl(no_mods);
        builder.children.push_back(var_decl);
    } else {
        SyntaxNode* init_expr = parse_expr_stmt();
        builder.children.push_back(init_expr);
    }

    // Condition
    if (!check(TokenKind::Semicolon)) {
        SyntaxNode* condition = parse_expression();
        builder.children.push_back(condition);
    }
    Token semi2 = consume_or_synthetic(TokenKind::Semicolon, "Expected ';' after for condition");
    builder.children.push_back(make_token_node(semi2));

    // Increment
    if (!check(TokenKind::RightParen)) {
        SyntaxNode* increment = parse_expression();
        builder.children.push_back(increment);
    }

    Token rparen = consume_or_synthetic(TokenKind::RightParen, "Expected ')' after for clauses");
    builder.children.push_back(make_token_node(rparen));

    SyntaxNode* body = parse_statement();
    builder.children.push_back(body);

    return finish_node(builder);
}

SyntaxNode* LspParser::parse_return_stmt() {
    // 'return' already consumed
    auto builder = begin_node(SyntaxKind::NodeReturnStmt);
    builder.children.push_back(make_token_node(m_previous)); // 'return'

    if (!check(TokenKind::Semicolon)) {
        SyntaxNode* value = parse_expression();
        builder.children.push_back(value);
    }

    Token semi = consume_or_synthetic(TokenKind::Semicolon, "Expected ';' after return value");
    builder.children.push_back(make_token_node(semi));

    return finish_node(builder);
}

SyntaxNode* LspParser::parse_break_stmt() {
    auto builder = begin_node(SyntaxKind::NodeBreakStmt);
    builder.children.push_back(make_token_node(m_previous)); // 'break'

    Token semi = consume_or_synthetic(TokenKind::Semicolon, "Expected ';' after 'break'");
    builder.children.push_back(make_token_node(semi));

    return finish_node(builder);
}

SyntaxNode* LspParser::parse_continue_stmt() {
    auto builder = begin_node(SyntaxKind::NodeContinueStmt);
    builder.children.push_back(make_token_node(m_previous)); // 'continue'

    Token semi = consume_or_synthetic(TokenKind::Semicolon, "Expected ';' after 'continue'");
    builder.children.push_back(make_token_node(semi));

    return finish_node(builder);
}

SyntaxNode* LspParser::parse_delete_stmt() {
    // 'delete' already consumed
    auto builder = begin_node(SyntaxKind::NodeDeleteStmt);
    builder.children.push_back(make_token_node(m_previous)); // 'delete'

    SyntaxNode* expr = parse_expression();
    builder.children.push_back(expr);

    Token semi = consume_or_synthetic(TokenKind::Semicolon, "Expected ';' after delete statement");
    builder.children.push_back(make_token_node(semi));

    return finish_node(builder);
}

SyntaxNode* LspParser::parse_when_stmt() {
    // 'when' already consumed
    auto builder = begin_node(SyntaxKind::NodeWhenStmt);
    builder.children.push_back(make_token_node(m_previous)); // 'when'

    // Parse discriminant: identifier with optional member access
    Token name_token = consume_or_synthetic(TokenKind::Identifier, "Expected discriminant after 'when'");
    builder.children.push_back(make_token_node(name_token));

    while (match(TokenKind::Dot)) {
        builder.children.push_back(make_token_node(m_previous)); // '.'
        Token member_token = consume_or_synthetic(TokenKind::Identifier, "Expected member name after '.'");
        builder.children.push_back(make_token_node(member_token));
    }

    Token lbrace = consume_or_synthetic(TokenKind::LeftBrace, "Expected '{' after 'when' discriminant");
    builder.children.push_back(make_token_node(lbrace));

    // Parse cases
    while (!check(TokenKind::RightBrace) && !is_at_end()) {
        if (match(TokenKind::KwCase)) {
            auto case_builder = begin_node(SyntaxKind::NodeWhenCase);
            case_builder.children.push_back(make_token_node(m_previous)); // 'case'

            // Case names (comma-separated)
            do {
                Token case_name = consume_or_synthetic(TokenKind::Identifier, "Expected case name");
                case_builder.children.push_back(make_token_node(case_name));
            } while (match(TokenKind::Comma));

            Token case_colon = consume_or_synthetic(TokenKind::Colon, "Expected ':' after case name(s)");
            case_builder.children.push_back(make_token_node(case_colon));

            // Case body: declarations until next case/else/}
            while (!check(TokenKind::KwCase) && !check(TokenKind::KwElse) &&
                   !check(TokenKind::RightBrace) && !is_at_end()) {
                SyntaxNode* decl = parse_declaration();
                if (decl) case_builder.children.push_back(decl);
            }

            builder.children.push_back(finish_node(case_builder));
        } else if (match(TokenKind::KwElse)) {
            auto else_builder = begin_node(SyntaxKind::NodeWhenCase);
            else_builder.children.push_back(make_token_node(m_previous)); // 'else'

            Token else_colon = consume_or_synthetic(TokenKind::Colon, "Expected ':' after 'else'");
            else_builder.children.push_back(make_token_node(else_colon));

            // Else body: declarations until }
            while (!check(TokenKind::RightBrace) && !is_at_end()) {
                SyntaxNode* decl = parse_declaration();
                if (decl) else_builder.children.push_back(decl);
            }

            builder.children.push_back(finish_node(else_builder));
            break;
        } else {
            builder.children.push_back(make_error_node("Expected 'case' or 'else' in when statement"));
            synchronize_to_statement_boundary();
        }
    }

    Token rbrace = consume_or_synthetic(TokenKind::RightBrace, "Expected '}' after when statement");
    builder.children.push_back(make_token_node(rbrace));

    return finish_node(builder);
}

SyntaxNode* LspParser::parse_throw_stmt() {
    // 'throw' already consumed
    auto builder = begin_node(SyntaxKind::NodeThrowStmt);
    builder.children.push_back(make_token_node(m_previous)); // 'throw'

    SyntaxNode* expr = parse_expression();
    builder.children.push_back(expr);

    Token semi = consume_or_synthetic(TokenKind::Semicolon, "Expected ';' after throw expression");
    builder.children.push_back(make_token_node(semi));

    return finish_node(builder);
}

SyntaxNode* LspParser::parse_try_stmt() {
    // 'try' already consumed
    auto builder = begin_node(SyntaxKind::NodeTryStmt);
    builder.children.push_back(make_token_node(m_previous)); // 'try'

    Token lbrace = consume_or_synthetic(TokenKind::LeftBrace, "Expected '{' after 'try'");
    builder.children.push_back(make_token_node(lbrace));

    SyntaxNode* try_body = parse_block_stmt();
    builder.children.push_back(try_body);

    // Parse catch clauses
    while (match(TokenKind::KwCatch)) {
        auto catch_builder = begin_node(SyntaxKind::NodeCatchClause);
        catch_builder.children.push_back(make_token_node(m_previous)); // 'catch'

        Token catch_lparen = consume_or_synthetic(TokenKind::LeftParen, "Expected '(' after 'catch'");
        catch_builder.children.push_back(make_token_node(catch_lparen));

        Token var_name = consume_or_synthetic(TokenKind::Identifier, "Expected variable name in catch clause");
        catch_builder.children.push_back(make_token_node(var_name));

        // Optional type annotation
        if (match(TokenKind::Colon)) {
            catch_builder.children.push_back(make_token_node(m_previous)); // ':'
            SyntaxNode* exception_type = parse_type_expr();
            catch_builder.children.push_back(exception_type);
        }

        Token catch_rparen = consume_or_synthetic(TokenKind::RightParen, "Expected ')' after catch variable");
        catch_builder.children.push_back(make_token_node(catch_rparen));

        Token catch_lbrace = consume_or_synthetic(TokenKind::LeftBrace, "Expected '{' after catch clause");
        catch_builder.children.push_back(make_token_node(catch_lbrace));

        SyntaxNode* catch_body = parse_block_stmt();
        catch_builder.children.push_back(catch_body);

        builder.children.push_back(finish_node(catch_builder));
    }

    // Optional finally
    if (match(TokenKind::KwFinally)) {
        builder.children.push_back(make_token_node(m_previous)); // 'finally'

        Token finally_lbrace = consume_or_synthetic(TokenKind::LeftBrace, "Expected '{' after 'finally'");
        builder.children.push_back(make_token_node(finally_lbrace));

        SyntaxNode* finally_body = parse_block_stmt();
        builder.children.push_back(finally_body);
    }

    return finish_node(builder);
}

SyntaxNode* LspParser::parse_expr_stmt() {
    auto builder = begin_node(SyntaxKind::NodeExprStmt);

    SyntaxNode* expr = parse_expression();
    builder.children.push_back(expr);

    Token semi = consume_or_synthetic(TokenKind::Semicolon, "Expected ';' after expression");
    builder.children.push_back(make_token_node(semi));

    return finish_node(builder);
}

// --- Expression parsing ---

SyntaxNode* LspParser::parse_expression() {
    SyntaxNode* expr = parse_precedence(1);

    // Handle ternary
    if (match(TokenKind::Question)) {
        auto builder = begin_node(SyntaxKind::NodeTernaryExpr);
        builder.start_offset = expr->range.start;
        builder.children.push_back(expr);
        builder.children.push_back(make_token_node(m_previous)); // '?'

        SyntaxNode* then_expr = parse_expression();
        builder.children.push_back(then_expr);

        Token colon = consume_or_synthetic(TokenKind::Colon, "Expected ':' in ternary expression");
        builder.children.push_back(make_token_node(colon));

        SyntaxNode* else_expr = parse_expression();
        builder.children.push_back(else_expr);

        expr = finish_node(builder);
    }

    // Handle assignment
    if (is_assignment_op(m_current.kind)) {
        auto builder = begin_node(SyntaxKind::NodeAssignExpr);
        builder.start_offset = expr->range.start;
        builder.children.push_back(expr);

        advance(); // consume the assignment operator
        builder.children.push_back(make_token_node(m_previous));

        SyntaxNode* value = parse_expression(); // right-associative
        builder.children.push_back(value);

        expr = finish_node(builder);
    }

    return expr;
}

SyntaxNode* LspParser::parse_precedence(u8 min_prec) {
    SyntaxNode* left = parse_unary();

    while (true) {
        InfixRule rule = get_infix_rule(m_current.kind);
        if (rule.precedence == 0 || rule.precedence < min_prec) break;

        auto builder = begin_node(SyntaxKind::NodeBinaryExpr);
        builder.start_offset = left->range.start;
        builder.children.push_back(left);

        advance(); // consume operator
        builder.children.push_back(make_token_node(m_previous));

        u8 next_prec = rule.right_assoc ? rule.precedence : rule.precedence + 1;
        SyntaxNode* right = parse_precedence(next_prec);
        builder.children.push_back(right);

        left = finish_node(builder);
    }

    return left;
}

SyntaxNode* LspParser::parse_unary() {
    if (check(TokenKind::Bang) || check(TokenKind::Minus) || check(TokenKind::Tilde)) {
        auto builder = begin_node(SyntaxKind::NodeUnaryExpr);
        advance();
        builder.children.push_back(make_token_node(m_previous));

        SyntaxNode* operand = parse_unary();
        builder.children.push_back(operand);

        return finish_node(builder);
    }

    SyntaxNode* expr = parse_primary();
    return parse_postfix(expr);
}

SyntaxNode* LspParser::parse_postfix(SyntaxNode* expr) {
    while (true) {
        if (match(TokenKind::LeftParen)) {
            // Call expression
            auto builder = begin_node(SyntaxKind::NodeCallExpr);
            builder.start_offset = expr->range.start;
            builder.children.push_back(expr);
            builder.children.push_back(make_token_node(m_previous)); // '('

            parse_call_args(builder);

            Token rparen = consume_or_synthetic(TokenKind::RightParen, "Expected ')' after arguments");
            builder.children.push_back(make_token_node(rparen));

            expr = finish_node(builder);
        } else if (match(TokenKind::Dot)) {
            // Member access
            auto builder = begin_node(SyntaxKind::NodeGetExpr);
            builder.start_offset = expr->range.start;
            builder.children.push_back(expr);
            builder.children.push_back(make_token_node(m_previous)); // '.'

            Token name_token = consume_or_synthetic(TokenKind::Identifier, "Expected property name after '.'");
            builder.children.push_back(make_token_node(name_token));

            expr = finish_node(builder);
        } else if (match(TokenKind::LeftBracket)) {
            // Index expression
            auto builder = begin_node(SyntaxKind::NodeIndexExpr);
            builder.start_offset = expr->range.start;
            builder.children.push_back(expr);
            builder.children.push_back(make_token_node(m_previous)); // '['

            SyntaxNode* index_expr = parse_expression();
            builder.children.push_back(index_expr);

            Token rbracket = consume_or_synthetic(TokenKind::RightBracket, "Expected ']' after index");
            builder.children.push_back(make_token_node(rbracket));

            expr = finish_node(builder);
        } else {
            break;
        }
    }

    return expr;
}

SyntaxNode* LspParser::parse_call_args(NodeBuilder& builder) {
    if (!check(TokenKind::RightParen)) {
        do {
            auto arg_builder = begin_node(SyntaxKind::NodeCallArg);

            // Check for out/inout modifier
            if (match(TokenKind::KwOut) || match(TokenKind::KwInout)) {
                arg_builder.children.push_back(make_token_node(m_previous));
            }

            SyntaxNode* arg_expr = parse_expression();
            arg_builder.children.push_back(arg_expr);

            builder.children.push_back(finish_node(arg_builder));
        } while (match(TokenKind::Comma));
    }
    return nullptr;
}

SyntaxNode* LspParser::parse_primary() {
    // Literals
    if (match(TokenKind::KwNil) || match(TokenKind::KwTrue) || match(TokenKind::KwFalse) ||
        match(TokenKind::IntLiteral) || match(TokenKind::FloatLiteral) ||
        match(TokenKind::StringLiteral)) {
        auto builder = begin_node(SyntaxKind::NodeLiteralExpr);
        builder.start_offset = m_previous.loc.offset;
        builder.children.push_back(make_token_node(m_previous));
        return finish_node(builder);
    }

    // F-string interpolation
    if (check(TokenKind::FStringBegin)) {
        return parse_fstring();
    }

    // uniq Type(...) or uniq Type { ... }
    if (match(TokenKind::KwUniq)) {
        auto builder = begin_node(SyntaxKind::NodeUniqExpr);
        builder.children.push_back(make_token_node(m_previous)); // 'uniq'

        Token type_token = consume_or_synthetic(TokenKind::Identifier, "Expected type name after 'uniq'");
        builder.children.push_back(make_token_node(type_token));

        if (match(TokenKind::LeftBrace)) {
            // Struct literal: uniq Type { ... }
            builder.kind = SyntaxKind::NodeStructLiteralExpr;
            builder.children.push_back(make_token_node(m_previous)); // '{'

            if (!check(TokenKind::RightBrace)) {
                do {
                    auto field_builder = begin_node(SyntaxKind::NodeFieldInit);
                    Token field_name = consume_or_synthetic(TokenKind::Identifier, "Expected field name");
                    field_builder.children.push_back(make_token_node(field_name));

                    Token eq = consume_or_synthetic(TokenKind::Equal, "Expected '=' after field name");
                    field_builder.children.push_back(make_token_node(eq));

                    SyntaxNode* value_expr = parse_expression();
                    field_builder.children.push_back(value_expr);

                    builder.children.push_back(finish_node(field_builder));
                } while (match(TokenKind::Comma));
            }

            Token rbrace = consume_or_synthetic(TokenKind::RightBrace, "Expected '}' after struct literal fields");
            builder.children.push_back(make_token_node(rbrace));

            return finish_node(builder);
        }

        // Constructor call: uniq Type.ctor_name() or uniq Type()
        if (match(TokenKind::Dot)) {
            builder.children.push_back(make_token_node(m_previous)); // '.'
            Token ctor_name = consume_or_synthetic(TokenKind::Identifier, "Expected constructor name after '.'");
            builder.children.push_back(make_token_node(ctor_name));
        }

        Token lparen = consume_or_synthetic(TokenKind::LeftParen, "Expected '(' or '{' after type");
        builder.children.push_back(make_token_node(lparen));
        builder.kind = SyntaxKind::NodeCallExpr;

        parse_call_args(builder);

        Token rparen = consume_or_synthetic(TokenKind::RightParen, "Expected ')' after arguments");
        builder.children.push_back(make_token_node(rparen));

        return finish_node(builder);
    }

    // Identifier, struct literal, constructor call, static get, generics
    if (match(TokenKind::Identifier)) {
        Token name_token = m_previous;

        // Static member access: Type::member
        if (match(TokenKind::ColonColon)) {
            auto builder = begin_node(SyntaxKind::NodeStaticGetExpr);
            builder.start_offset = name_token.loc.offset;
            builder.children.push_back(make_token_node(name_token));
            builder.children.push_back(make_token_node(m_previous)); // '::'
            Token member = consume_or_synthetic(TokenKind::Identifier, "Expected member name after '::'");
            builder.children.push_back(make_token_node(member));
            return finish_node(builder);
        }

        // Generic args trial parse: identifier<types>( or identifier<types>{
        if (check(TokenKind::Less)) {
            auto builder = begin_node(SyntaxKind::NodeIdentifierExpr);
            builder.start_offset = name_token.loc.offset;
            builder.children.push_back(make_token_node(name_token));

            if (try_parse_generic_args(builder)) {
                // Successfully parsed generic args
                if (match(TokenKind::LeftBrace)) {
                    // Generic struct literal: Box<i32> { ... }
                    builder.kind = SyntaxKind::NodeStructLiteralExpr;
                    builder.children.push_back(make_token_node(m_previous)); // '{'

                    if (!check(TokenKind::RightBrace)) {
                        do {
                            auto field_builder = begin_node(SyntaxKind::NodeFieldInit);
                            Token field_name = consume_or_synthetic(TokenKind::Identifier, "Expected field name");
                            field_builder.children.push_back(make_token_node(field_name));

                            Token eq = consume_or_synthetic(TokenKind::Equal, "Expected '=' after field name");
                            field_builder.children.push_back(make_token_node(eq));

                            SyntaxNode* value_expr = parse_expression();
                            field_builder.children.push_back(value_expr);

                            builder.children.push_back(finish_node(field_builder));
                        } while (match(TokenKind::Comma));
                    }

                    Token rbrace = consume_or_synthetic(TokenKind::RightBrace, "Expected '}' after struct literal fields");
                    builder.children.push_back(make_token_node(rbrace));
                    return finish_node(builder);
                }

                if (match(TokenKind::LeftParen)) {
                    // Generic call: identity<i32>(42)
                    builder.kind = SyntaxKind::NodeCallExpr;
                    builder.children.push_back(make_token_node(m_previous)); // '('
                    parse_call_args(builder);
                    Token rparen = consume_or_synthetic(TokenKind::RightParen, "Expected ')' after arguments");
                    builder.children.push_back(make_token_node(rparen));
                    return finish_node(builder);
                }

                // Should not happen if try_parse_generic_args succeeded,
                // but fall through to identifier
                return finish_node(builder);
            }
            // Trial parse failed, fall through to non-generic paths
        }

        // Struct literal: Type { ... }
        if (match(TokenKind::LeftBrace)) {
            auto builder = begin_node(SyntaxKind::NodeStructLiteralExpr);
            builder.start_offset = name_token.loc.offset;
            builder.children.push_back(make_token_node(name_token));
            builder.children.push_back(make_token_node(m_previous)); // '{'

            if (!check(TokenKind::RightBrace)) {
                do {
                    auto field_builder = begin_node(SyntaxKind::NodeFieldInit);
                    Token field_name = consume_or_synthetic(TokenKind::Identifier, "Expected field name");
                    field_builder.children.push_back(make_token_node(field_name));

                    Token eq = consume_or_synthetic(TokenKind::Equal, "Expected '=' after field name");
                    field_builder.children.push_back(make_token_node(eq));

                    SyntaxNode* value_expr = parse_expression();
                    field_builder.children.push_back(value_expr);

                    builder.children.push_back(finish_node(field_builder));
                } while (match(TokenKind::Comma));
            }

            Token rbrace = consume_or_synthetic(TokenKind::RightBrace, "Expected '}' after struct literal fields");
            builder.children.push_back(make_token_node(rbrace));
            return finish_node(builder);
        }

        // Regular identifier
        auto builder = begin_node(SyntaxKind::NodeIdentifierExpr);
        builder.start_offset = name_token.loc.offset;
        builder.children.push_back(make_token_node(name_token));
        return finish_node(builder);
    }

    // self
    if (match(TokenKind::KwSelf)) {
        auto builder = begin_node(SyntaxKind::NodeSelfExpr);
        builder.start_offset = m_previous.loc.offset;
        builder.children.push_back(make_token_node(m_previous));
        return finish_node(builder);
    }

    // super
    if (match(TokenKind::KwSuper)) {
        auto builder = begin_node(SyntaxKind::NodeSuperExpr);
        builder.start_offset = m_previous.loc.offset;
        builder.children.push_back(make_token_node(m_previous)); // 'super'

        if (!check(TokenKind::LeftParen)) {
            Token dot = consume_or_synthetic(TokenKind::Dot, "Expected '.' or '(' after 'super'");
            builder.children.push_back(make_token_node(dot));
            Token method_name = consume_or_synthetic(TokenKind::Identifier, "Expected method name after 'super.'");
            builder.children.push_back(make_token_node(method_name));
        }

        return finish_node(builder);
    }

    // Grouping: (expr)
    if (match(TokenKind::LeftParen)) {
        auto builder = begin_node(SyntaxKind::NodeGroupingExpr);
        builder.children.push_back(make_token_node(m_previous)); // '('

        SyntaxNode* inner = parse_expression();
        builder.children.push_back(inner);

        Token rparen = consume_or_synthetic(TokenKind::RightParen, "Expected ')' after expression");
        builder.children.push_back(make_token_node(rparen));

        return finish_node(builder);
    }

    // Error: unexpected token
    return make_error_node("Expected expression");
}

SyntaxNode* LspParser::parse_fstring() {
    // FStringBegin already peeked but not consumed
    auto builder = begin_node(SyntaxKind::NodeStringInterpExpr);

    advance(); // consume FStringBegin
    builder.children.push_back(make_token_node(m_previous));

    while (true) {
        SyntaxNode* interp_expr = parse_expression();
        builder.children.push_back(interp_expr);

        if (match(TokenKind::FStringMid)) {
            builder.children.push_back(make_token_node(m_previous));
        } else if (match(TokenKind::FStringEnd)) {
            builder.children.push_back(make_token_node(m_previous));
            break;
        } else {
            add_diagnostic(
                TextRange{m_current.loc.offset, m_current.loc.offset + m_current.length},
                "Expected '}' in f-string interpolation");
            break;
        }
    }

    return finish_node(builder);
}

// --- Type parsing ---

SyntaxNode* LspParser::parse_type_expr() {
    auto builder = begin_node(SyntaxKind::NodeTypeExpr);

    // Optional reference modifier
    if (match(TokenKind::KwUniq) || match(TokenKind::KwRef) || match(TokenKind::KwWeak)) {
        builder.children.push_back(make_token_node(m_previous));
    }

    Token name_token = consume_or_synthetic(TokenKind::Identifier, "Expected type name");
    builder.children.push_back(make_token_node(name_token));

    // Optional generic type args
    if (check(TokenKind::Less)) {
        SyntaxNode* type_args = parse_type_args();
        builder.children.push_back(type_args);
    }

    return finish_node(builder);
}

SyntaxNode* LspParser::parse_type_params() {
    auto builder = begin_node(SyntaxKind::NodeTypeParamList);

    Token less = consume_or_synthetic(TokenKind::Less, "Expected '<' for type parameters");
    builder.children.push_back(make_token_node(less));

    do {
        auto param_builder = begin_node(SyntaxKind::NodeTypeParam);
        Token name_token = consume_or_synthetic(TokenKind::Identifier, "Expected type parameter name");
        param_builder.children.push_back(make_token_node(name_token));

        // Optional trait bounds: <T: Trait1 + Trait2>
        if (match(TokenKind::Colon)) {
            param_builder.children.push_back(make_token_node(m_previous));
            do {
                SyntaxNode* bound = parse_type_expr();
                param_builder.children.push_back(bound);
            } while (match(TokenKind::Plus));
        }

        builder.children.push_back(finish_node(param_builder));
    } while (match(TokenKind::Comma));

    consume_closing_angle(builder);

    return finish_node(builder);
}

SyntaxNode* LspParser::parse_type_args() {
    auto builder = begin_node(SyntaxKind::NodeTypeArgList);

    Token less = consume_or_synthetic(TokenKind::Less, "Expected '<' for type arguments");
    builder.children.push_back(make_token_node(less));

    do {
        SyntaxNode* type_node = parse_type_expr();
        builder.children.push_back(type_node);
    } while (match(TokenKind::Comma));

    consume_closing_angle(builder);

    return finish_node(builder);
}

bool LspParser::try_parse_generic_args(NodeBuilder& parent_builder) {
    SavedState saved = save_state();

    advance(); // consume '<'

    auto type_arg_builder = begin_node(SyntaxKind::NodeTypeArgList);
    type_arg_builder.children.push_back(make_token_node(m_previous)); // '<'

    // Try parsing type expressions
    do {
        SyntaxNode* type_node = parse_type_expr();
        type_arg_builder.children.push_back(type_node);
    } while (match(TokenKind::Comma));

    // Try to consume closing '>'
    if (check(TokenKind::Greater)) {
        advance();
        type_arg_builder.children.push_back(make_token_node(m_previous));
    } else if (check(TokenKind::GreaterGreater)) {
        // Split >>
        Token first_greater;
        first_greater.kind = TokenKind::Greater;
        first_greater.loc = m_current.loc;
        first_greater.start = m_current.start;
        first_greater.length = 1;
        first_greater.int_value = 0;
        type_arg_builder.children.push_back(make_token_node(first_greater));

        m_previous = m_current;
        m_previous.length = 1;
        m_current.kind = TokenKind::Greater;
        m_current.start = m_current.start + 1;
        m_current.length = 1;
        m_current.loc.column += 1;
        m_current.loc.offset += 1;
    } else if (check(TokenKind::GreaterGreaterEqual)) {
        // Split >>=
        Token first_greater;
        first_greater.kind = TokenKind::Greater;
        first_greater.loc = m_current.loc;
        first_greater.start = m_current.start;
        first_greater.length = 1;
        first_greater.int_value = 0;
        type_arg_builder.children.push_back(make_token_node(first_greater));

        m_previous = m_current;
        m_previous.length = 1;
        m_current.kind = TokenKind::GreaterEqual;
        m_current.start = m_current.start + 1;
        m_current.length = 2;
        m_current.loc.column += 1;
        m_current.loc.offset += 1;
    } else {
        // Failed to parse closing angle — backtrack
        restore_state(saved);
        return false;
    }

    // Must be followed by '(' or '{' to confirm this is generic args
    if (check(TokenKind::LeftParen) || check(TokenKind::LeftBrace)) {
        parent_builder.children.push_back(finish_node(type_arg_builder));
        return true;
    }

    // Not followed by ( or { — this was a comparison
    restore_state(saved);
    return false;
}

bool LspParser::consume_closing_angle(NodeBuilder& builder) {
    if (match(TokenKind::Greater)) {
        builder.children.push_back(make_token_node(m_previous));
        return true;
    }
    if (check(TokenKind::GreaterGreater)) {
        // Split >> into > + >
        Token first_greater;
        first_greater.kind = TokenKind::Greater;
        first_greater.loc = m_current.loc;
        first_greater.start = m_current.start;
        first_greater.length = 1;
        first_greater.int_value = 0;
        builder.children.push_back(make_token_node(first_greater));

        m_previous = m_current;
        m_previous.length = 1;
        m_current.kind = TokenKind::Greater;
        m_current.start = m_current.start + 1;
        m_current.length = 1;
        m_current.loc.column += 1;
        m_current.loc.offset += 1;
        return true;
    }
    if (check(TokenKind::GreaterGreaterEqual)) {
        // Split >>= into > + >=
        Token first_greater;
        first_greater.kind = TokenKind::Greater;
        first_greater.loc = m_current.loc;
        first_greater.start = m_current.start;
        first_greater.length = 1;
        first_greater.int_value = 0;
        builder.children.push_back(make_token_node(first_greater));

        m_previous = m_current;
        m_previous.length = 1;
        m_current.kind = TokenKind::GreaterEqual;
        m_current.start = m_current.start + 1;
        m_current.length = 2;
        m_current.loc.column += 1;
        m_current.loc.offset += 1;
        return true;
    }

    Token synthetic = consume_or_synthetic(TokenKind::Greater, "Expected '>' after type parameters");
    builder.children.push_back(make_token_node(synthetic));
    return false;
}

// --- Parameter parsing ---

SyntaxNode* LspParser::parse_param_list() {
    auto builder = begin_node(SyntaxKind::NodeParamList);

    if (!check(TokenKind::RightParen)) {
        do {
            SyntaxNode* param = parse_param();
            builder.children.push_back(param);
        } while (match(TokenKind::Comma));
    }

    return finish_node(builder);
}

SyntaxNode* LspParser::parse_param() {
    auto builder = begin_node(SyntaxKind::NodeParam);

    Token name_token = consume_or_synthetic(TokenKind::Identifier, "Expected parameter name");
    builder.children.push_back(make_token_node(name_token));

    Token colon = consume_or_synthetic(TokenKind::Colon, "Expected ':' after parameter name");
    builder.children.push_back(make_token_node(colon));

    // Optional parameter modifier (out/inout) before the type
    if (match(TokenKind::KwOut) || match(TokenKind::KwInout)) {
        builder.children.push_back(make_token_node(m_previous));
    }

    SyntaxNode* type_node = parse_type_expr();
    builder.children.push_back(type_node);

    return finish_node(builder);
}

} // namespace rx
