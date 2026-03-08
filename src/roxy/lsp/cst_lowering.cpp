#include "roxy/lsp/cst_lowering.hpp"

#include <cstring>

namespace rx {

// --- Constructor ---

CstLowering::CstLowering(BumpAllocator& allocator)
    : m_allocator(allocator) {}

// --- Helpers ---

template<typename T>
T* CstLowering::alloc() {
    u8* raw = m_allocator.alloc_bytes(sizeof(T), alignof(T));
    memset(raw, 0, sizeof(T));
    return reinterpret_cast<T*>(raw);
}

SyntaxNode* CstLowering::find_child(SyntaxNode* node, SyntaxKind kind) {
    for (u32 i = 0; i < node->children.size(); i++) {
        if (node->children[i]->kind == kind) return node->children[i];
    }
    return nullptr;
}

SyntaxNode* CstLowering::find_child_after(SyntaxNode* node, SyntaxKind kind, u32 start) {
    for (u32 i = start; i < node->children.size(); i++) {
        if (node->children[i]->kind == kind) return node->children[i];
    }
    return nullptr;
}

bool CstLowering::has_child(SyntaxNode* node, SyntaxKind kind) {
    return find_child(node, kind) != nullptr;
}

SourceLocation CstLowering::make_loc(SyntaxNode* node) {
    if (!node) return SourceLocation{0, 0, 0, 0};
    return SourceLocation{node->range.start, node->range.end, 0, 0};
}

SourceLocation CstLowering::make_loc(TextRange range) {
    return SourceLocation{range.start, range.end, 0, 0};
}

// --- Binary/Unary/Assign operator mapping ---

static BinaryOp syntax_kind_to_binary_op(SyntaxKind kind) {
    switch (kind) {
        case SyntaxKind::TokenPlus:            return BinaryOp::Add;
        case SyntaxKind::TokenMinus:           return BinaryOp::Sub;
        case SyntaxKind::TokenStar:            return BinaryOp::Mul;
        case SyntaxKind::TokenSlash:           return BinaryOp::Div;
        case SyntaxKind::TokenPercent:         return BinaryOp::Mod;
        case SyntaxKind::TokenEqualEqual:      return BinaryOp::Equal;
        case SyntaxKind::TokenBangEqual:       return BinaryOp::NotEqual;
        case SyntaxKind::TokenLess:            return BinaryOp::Less;
        case SyntaxKind::TokenLessEqual:       return BinaryOp::LessEq;
        case SyntaxKind::TokenGreater:         return BinaryOp::Greater;
        case SyntaxKind::TokenGreaterEqual:    return BinaryOp::GreaterEq;
        case SyntaxKind::TokenAmpAmp:          return BinaryOp::And;
        case SyntaxKind::TokenPipePipe:        return BinaryOp::Or;
        case SyntaxKind::TokenAmp:             return BinaryOp::BitAnd;
        case SyntaxKind::TokenPipe:            return BinaryOp::BitOr;
        case SyntaxKind::TokenCaret:           return BinaryOp::BitXor;
        case SyntaxKind::TokenLessLess:        return BinaryOp::Shl;
        case SyntaxKind::TokenGreaterGreater:  return BinaryOp::Shr;
        default:                               return BinaryOp::Add;
    }
}

static UnaryOp syntax_kind_to_unary_op(SyntaxKind kind) {
    switch (kind) {
        case SyntaxKind::TokenMinus: return UnaryOp::Negate;
        case SyntaxKind::TokenBang:  return UnaryOp::Not;
        case SyntaxKind::TokenTilde: return UnaryOp::BitNot;
        default:                     return UnaryOp::Negate;
    }
}

static AssignOp syntax_kind_to_assign_op(SyntaxKind kind) {
    switch (kind) {
        case SyntaxKind::TokenEqual:                return AssignOp::Assign;
        case SyntaxKind::TokenPlusEqual:            return AssignOp::AddAssign;
        case SyntaxKind::TokenMinusEqual:           return AssignOp::SubAssign;
        case SyntaxKind::TokenStarEqual:            return AssignOp::MulAssign;
        case SyntaxKind::TokenSlashEqual:           return AssignOp::DivAssign;
        case SyntaxKind::TokenPercentEqual:         return AssignOp::ModAssign;
        case SyntaxKind::TokenAmpEqual:             return AssignOp::BitAndAssign;
        case SyntaxKind::TokenPipeEqual:            return AssignOp::BitOrAssign;
        case SyntaxKind::TokenCaretEqual:           return AssignOp::BitXorAssign;
        case SyntaxKind::TokenLessLessEqual:        return AssignOp::ShlAssign;
        case SyntaxKind::TokenGreaterGreaterEqual:  return AssignOp::ShrAssign;
        default:                                    return AssignOp::Assign;
    }
}

static ParamModifier syntax_kind_to_param_modifier(SyntaxKind kind) {
    switch (kind) {
        case SyntaxKind::TokenKwOut:   return ParamModifier::Out;
        case SyntaxKind::TokenKwInout: return ParamModifier::Inout;
        default:                       return ParamModifier::None;
    }
}

static RefKind syntax_kind_to_ref_kind(SyntaxKind kind) {
    switch (kind) {
        case SyntaxKind::TokenKwUniq: return RefKind::Uniq;
        case SyntaxKind::TokenKwRef:  return RefKind::Ref;
        case SyntaxKind::TokenKwWeak: return RefKind::Weak;
        default:                      return RefKind::None;
    }
}

// --- Program lowering ---

Program* CstLowering::lower(SyntaxNode* root) {
    if (!root || root->kind != SyntaxKind::NodeProgram) return nullptr;

    Vector<Decl*> declarations;
    for (u32 i = 0; i < root->children.size(); i++) {
        Decl* decl = lower_top_level_node(root->children[i]);
        if (decl) declarations.push_back(decl);
    }

    Program* program = m_allocator.emplace<Program>();
    program->module_name = StringView();
    program->declarations = m_allocator.alloc_span(declarations);
    return program;
}

Decl* CstLowering::lower_decl(SyntaxNode* node) {
    if (!node) return nullptr;
    return lower_top_level_node(node);
}

Decl* CstLowering::lower_top_level_node(SyntaxNode* node) {
    if (!node || node->kind == SyntaxKind::Error) return nullptr;

    switch (node->kind) {
        case SyntaxKind::NodeVarDecl:          return lower_var_decl(node);
        case SyntaxKind::NodeFunDecl:          return lower_fun_decl(node);
        case SyntaxKind::NodeMethodDecl:       return lower_method_decl(node);
        case SyntaxKind::NodeConstructorDecl:  return lower_constructor_decl(node);
        case SyntaxKind::NodeDestructorDecl:   return lower_destructor_decl(node);
        case SyntaxKind::NodeStructDecl:       return lower_struct_decl(node);
        case SyntaxKind::NodeEnumDecl:         return lower_enum_decl(node);
        case SyntaxKind::NodeTraitDecl:        return lower_trait_decl(node);
        case SyntaxKind::NodeImportDecl:       return lower_import_decl(node);
        default: {
            // Statements wrapped as declarations
            Stmt* stmt = lower_stmt(node);
            if (!stmt) return nullptr;
            Decl* decl = alloc<Decl>();
            decl->kind = stmt->kind;
            decl->loc = stmt->loc;
            decl->stmt = *stmt;
            return decl;
        }
    }
}

// --- Declaration lowering ---

Decl* CstLowering::lower_var_decl(SyntaxNode* node) {
    Decl* decl = alloc<Decl>();
    decl->kind = AstKind::DeclVar;
    decl->loc = make_loc(node);

    bool is_pub = has_child(node, SyntaxKind::TokenKwPub);

    // Find name
    SyntaxNode* name_node = find_child(node, SyntaxKind::TokenIdentifier);
    StringView name = name_node ? name_node->token.text() : StringView();

    // Find type
    TypeExpr* type = nullptr;
    SyntaxNode* type_node = find_child(node, SyntaxKind::NodeTypeExpr);
    if (type_node) type = lower_type_expr(type_node);

    // Find initializer (expression after '=')
    Expr* initializer = nullptr;
    bool past_equal = false;
    for (u32 i = 0; i < node->children.size(); i++) {
        if (node->children[i]->kind == SyntaxKind::TokenEqual) {
            past_equal = true;
        } else if (past_equal && node->children[i]->kind != SyntaxKind::TokenSemicolon) {
            initializer = lower_expr(node->children[i]);
            break;
        }
    }

    decl->var_decl.name = name;
    decl->var_decl.type = type;
    decl->var_decl.initializer = initializer;
    decl->var_decl.is_pub = is_pub;
    decl->var_decl.resolved_type = nullptr;
    return decl;
}

Decl* CstLowering::lower_fun_decl(SyntaxNode* node) {
    Decl* decl = alloc<Decl>();
    decl->kind = AstKind::DeclFun;
    decl->loc = make_loc(node);

    bool is_pub = has_child(node, SyntaxKind::TokenKwPub);
    bool is_native = has_child(node, SyntaxKind::TokenKwNative);

    // Find name (first identifier)
    SyntaxNode* name_node = find_child(node, SyntaxKind::TokenIdentifier);
    StringView name = name_node ? name_node->token.text() : StringView();

    // Type params
    SyntaxNode* type_param_node = find_child(node, SyntaxKind::NodeTypeParamList);
    Span<TypeParam> type_params = lower_type_param_list(type_param_node);

    // Params
    SyntaxNode* param_list = find_child(node, SyntaxKind::NodeParamList);
    Span<Param> params = lower_param_list(param_list);

    // Return type (NodeTypeExpr after ')')
    TypeExpr* return_type = nullptr;
    bool past_rparen = false;
    for (u32 i = 0; i < node->children.size(); i++) {
        if (node->children[i]->kind == SyntaxKind::TokenRightParen) {
            past_rparen = true;
        } else if (past_rparen && node->children[i]->kind == SyntaxKind::NodeTypeExpr) {
            return_type = lower_type_expr(node->children[i]);
            break;
        }
    }

    // Body
    Stmt* body = nullptr;
    SyntaxNode* block_node = find_child(node, SyntaxKind::NodeBlockStmt);
    if (block_node) body = lower_block_stmt(block_node);

    decl->fun_decl.name = name;
    decl->fun_decl.type_params = type_params;
    decl->fun_decl.params = params;
    decl->fun_decl.return_type = return_type;
    decl->fun_decl.body = body;
    decl->fun_decl.is_pub = is_pub;
    decl->fun_decl.is_native = is_native;
    return decl;
}

Decl* CstLowering::lower_method_decl(SyntaxNode* node) {
    Decl* decl = alloc<Decl>();
    decl->kind = AstKind::DeclMethod;
    decl->loc = make_loc(node);

    bool is_pub = has_child(node, SyntaxKind::TokenKwPub);
    bool is_native = has_child(node, SyntaxKind::TokenKwNative);

    // Find struct name (first identifier)
    SyntaxNode* struct_name_node = find_child(node, SyntaxKind::TokenIdentifier);
    StringView struct_name = struct_name_node ? struct_name_node->token.text() : StringView();

    // Find method name (identifier after '.')
    StringView method_name;
    bool past_dot = false;
    for (u32 i = 0; i < node->children.size(); i++) {
        if (node->children[i]->kind == SyntaxKind::TokenDot) {
            past_dot = true;
        } else if (past_dot) {
            if (node->children[i]->kind == SyntaxKind::TokenIdentifier ||
                node->children[i]->kind == SyntaxKind::TokenKwNew ||
                node->children[i]->kind == SyntaxKind::TokenKwDelete) {
                method_name = node->children[i]->token.text();
            }
            break;
        }
    }

    // Type params
    SyntaxNode* type_param_node = find_child(node, SyntaxKind::NodeTypeParamList);
    Span<TypeParam> type_params = lower_type_param_list(type_param_node);

    // Params
    SyntaxNode* param_list = find_child(node, SyntaxKind::NodeParamList);
    Span<Param> params = lower_param_list(param_list);

    // Return type
    TypeExpr* return_type = nullptr;
    bool past_rparen = false;
    for (u32 i = 0; i < node->children.size(); i++) {
        if (node->children[i]->kind == SyntaxKind::TokenRightParen) {
            past_rparen = true;
        } else if (past_rparen && node->children[i]->kind == SyntaxKind::NodeTypeExpr) {
            return_type = lower_type_expr(node->children[i]);
            break;
        }
    }

    // Trait name (from "for Trait" clause)
    StringView trait_name;
    Span<TypeExpr*> trait_type_args;
    bool past_for = false;
    for (u32 i = 0; i < node->children.size(); i++) {
        if (node->children[i]->kind == SyntaxKind::TokenKwFor) {
            past_for = true;
        } else if (past_for && node->children[i]->kind == SyntaxKind::TokenIdentifier) {
            trait_name = node->children[i]->token.text();
            break;
        }
    }

    // Body
    Stmt* body = nullptr;
    SyntaxNode* block_node = find_child(node, SyntaxKind::NodeBlockStmt);
    if (block_node) body = lower_block_stmt(block_node);

    decl->method_decl.struct_name = struct_name;
    decl->method_decl.name = method_name;
    decl->method_decl.type_params = type_params;
    decl->method_decl.params = params;
    decl->method_decl.return_type = return_type;
    decl->method_decl.body = body;
    decl->method_decl.is_pub = is_pub;
    decl->method_decl.is_native = is_native;
    decl->method_decl.trait_name = trait_name;
    decl->method_decl.trait_type_args = trait_type_args;
    return decl;
}

Decl* CstLowering::lower_constructor_decl(SyntaxNode* node) {
    Decl* decl = alloc<Decl>();
    decl->kind = AstKind::DeclConstructor;
    decl->loc = make_loc(node);

    bool is_pub = has_child(node, SyntaxKind::TokenKwPub);

    // Find struct name (first identifier)
    SyntaxNode* struct_name_node = find_child(node, SyntaxKind::TokenIdentifier);
    StringView struct_name = struct_name_node ? struct_name_node->token.text() : StringView();

    // Named constructor (identifier after '.')
    StringView ctor_name;
    bool past_dot = false;
    for (u32 i = 0; i < node->children.size(); i++) {
        if (node->children[i]->kind == SyntaxKind::TokenDot) {
            past_dot = true;
        } else if (past_dot && node->children[i]->kind == SyntaxKind::TokenIdentifier) {
            ctor_name = node->children[i]->token.text();
            break;
        }
    }

    // Params
    SyntaxNode* param_list = find_child(node, SyntaxKind::NodeParamList);
    Span<Param> params = lower_param_list(param_list);

    // Body
    Stmt* body = nullptr;
    SyntaxNode* block_node = find_child(node, SyntaxKind::NodeBlockStmt);
    if (block_node) body = lower_block_stmt(block_node);

    decl->constructor_decl.struct_name = struct_name;
    decl->constructor_decl.name = ctor_name;
    decl->constructor_decl.params = params;
    decl->constructor_decl.body = body;
    decl->constructor_decl.is_pub = is_pub;
    return decl;
}

Decl* CstLowering::lower_destructor_decl(SyntaxNode* node) {
    Decl* decl = alloc<Decl>();
    decl->kind = AstKind::DeclDestructor;
    decl->loc = make_loc(node);

    bool is_pub = has_child(node, SyntaxKind::TokenKwPub);

    // Find struct name
    SyntaxNode* struct_name_node = find_child(node, SyntaxKind::TokenIdentifier);
    StringView struct_name = struct_name_node ? struct_name_node->token.text() : StringView();

    // Named destructor
    StringView dtor_name;
    bool past_dot = false;
    for (u32 i = 0; i < node->children.size(); i++) {
        if (node->children[i]->kind == SyntaxKind::TokenDot) {
            past_dot = true;
        } else if (past_dot && node->children[i]->kind == SyntaxKind::TokenIdentifier) {
            dtor_name = node->children[i]->token.text();
            break;
        }
    }

    // Params
    SyntaxNode* param_list = find_child(node, SyntaxKind::NodeParamList);
    Span<Param> params = lower_param_list(param_list);

    // Body
    Stmt* body = nullptr;
    SyntaxNode* block_node = find_child(node, SyntaxKind::NodeBlockStmt);
    if (block_node) body = lower_block_stmt(block_node);

    decl->destructor_decl.struct_name = struct_name;
    decl->destructor_decl.name = dtor_name;
    decl->destructor_decl.params = params;
    decl->destructor_decl.body = body;
    decl->destructor_decl.is_pub = is_pub;
    return decl;
}

Decl* CstLowering::lower_struct_decl(SyntaxNode* node) {
    Decl* decl = alloc<Decl>();
    decl->kind = AstKind::DeclStruct;
    decl->loc = make_loc(node);

    bool is_pub = has_child(node, SyntaxKind::TokenKwPub);

    // Name
    SyntaxNode* name_node = find_child(node, SyntaxKind::TokenIdentifier);
    StringView name = name_node ? name_node->token.text() : StringView();

    // Type params
    SyntaxNode* type_param_node = find_child(node, SyntaxKind::NodeTypeParamList);
    Span<TypeParam> type_params = lower_type_param_list(type_param_node);

    // Parent name (identifier after ':')
    StringView parent_name;
    bool past_colon = false;
    for (u32 i = 0; i < node->children.size(); i++) {
        if (node->children[i]->kind == SyntaxKind::TokenColon) {
            past_colon = true;
        } else if (past_colon && node->children[i]->kind == SyntaxKind::TokenIdentifier) {
            parent_name = node->children[i]->token.text();
            break;
        } else if (node->children[i]->kind == SyntaxKind::TokenLeftBrace) {
            break;
        }
    }

    // Fields
    Vector<FieldDecl> fields;
    for (u32 i = 0; i < node->children.size(); i++) {
        SyntaxNode* child = node->children[i];
        if (child->kind == SyntaxKind::NodeFieldDecl) {
            FieldDecl field;
            field.loc = make_loc(child);
            field.is_pub = has_child(child, SyntaxKind::TokenKwPub);
            field.default_value = nullptr;

            SyntaxNode* field_name = find_child(child, SyntaxKind::TokenIdentifier);
            field.name = field_name ? field_name->token.text() : StringView();

            SyntaxNode* field_type = find_child(child, SyntaxKind::NodeTypeExpr);
            field.type = field_type ? lower_type_expr(field_type) : nullptr;

            // Default value
            bool field_past_equal = false;
            for (u32 j = 0; j < child->children.size(); j++) {
                if (child->children[j]->kind == SyntaxKind::TokenEqual) {
                    field_past_equal = true;
                } else if (field_past_equal && child->children[j]->kind != SyntaxKind::TokenSemicolon) {
                    field.default_value = lower_expr(child->children[j]);
                    break;
                }
            }

            fields.push_back(field);
        }
    }

    decl->struct_decl.name = name;
    decl->struct_decl.type_params = type_params;
    decl->struct_decl.parent_name = parent_name;
    decl->struct_decl.fields = m_allocator.alloc_span(fields);
    decl->struct_decl.when_clauses = Span<WhenFieldDecl>();
    decl->struct_decl.methods = Span<FunDecl*>();
    decl->struct_decl.is_pub = is_pub;
    return decl;
}

Decl* CstLowering::lower_enum_decl(SyntaxNode* node) {
    Decl* decl = alloc<Decl>();
    decl->kind = AstKind::DeclEnum;
    decl->loc = make_loc(node);

    bool is_pub = has_child(node, SyntaxKind::TokenKwPub);

    SyntaxNode* name_node = find_child(node, SyntaxKind::TokenIdentifier);
    StringView name = name_node ? name_node->token.text() : StringView();

    Vector<EnumVariant> variants;
    for (u32 i = 0; i < node->children.size(); i++) {
        if (node->children[i]->kind == SyntaxKind::NodeEnumVariant) {
            SyntaxNode* variant_node = node->children[i];
            EnumVariant variant;
            variant.value = nullptr;

            SyntaxNode* variant_name = find_child(variant_node, SyntaxKind::TokenIdentifier);
            variant.name = variant_name ? variant_name->token.text() : StringView();
            variant.loc = make_loc(variant_node);

            variants.push_back(variant);
        }
    }

    decl->enum_decl.name = name;
    decl->enum_decl.variants = m_allocator.alloc_span(variants);
    decl->enum_decl.is_pub = is_pub;
    return decl;
}

Decl* CstLowering::lower_trait_decl(SyntaxNode* node) {
    Decl* decl = alloc<Decl>();
    decl->kind = AstKind::DeclTrait;
    decl->loc = make_loc(node);

    bool is_pub = has_child(node, SyntaxKind::TokenKwPub);

    SyntaxNode* name_node = find_child(node, SyntaxKind::TokenIdentifier);
    StringView name = name_node ? name_node->token.text() : StringView();

    // Type params
    SyntaxNode* type_param_node = find_child(node, SyntaxKind::NodeTypeParamList);
    Span<TypeParam> type_params = lower_type_param_list(type_param_node);

    // Parent
    StringView parent_name;
    bool past_colon = false;
    for (u32 i = 0; i < node->children.size(); i++) {
        if (node->children[i]->kind == SyntaxKind::TokenColon) {
            past_colon = true;
        } else if (past_colon && node->children[i]->kind == SyntaxKind::TokenIdentifier) {
            parent_name = node->children[i]->token.text();
            break;
        }
    }

    decl->trait_decl.name = name;
    decl->trait_decl.type_params = type_params;
    decl->trait_decl.parent_name = parent_name;
    decl->trait_decl.is_pub = is_pub;
    return decl;
}

Decl* CstLowering::lower_import_decl(SyntaxNode* node) {
    Decl* decl = alloc<Decl>();
    decl->kind = AstKind::DeclImport;
    decl->loc = make_loc(node);

    bool is_from_import = has_child(node, SyntaxKind::TokenKwFrom);

    // Module path: first identifier after 'import' or 'from'
    StringView module_path;
    bool past_keyword = false;
    for (u32 i = 0; i < node->children.size(); i++) {
        SyntaxNode* child = node->children[i];
        if (child->kind == SyntaxKind::TokenKwImport || child->kind == SyntaxKind::TokenKwFrom) {
            past_keyword = true;
        } else if (past_keyword && child->kind == SyntaxKind::TokenIdentifier) {
            module_path = child->token.text();
            break;
        }
    }

    // Import names
    Vector<ImportName> names;
    for (u32 i = 0; i < node->children.size(); i++) {
        if (node->children[i]->kind == SyntaxKind::NodeImportName) {
            SyntaxNode* import_name_node = node->children[i];
            ImportName import_name;
            import_name.alias = StringView();

            SyntaxNode* name_ident = find_child(import_name_node, SyntaxKind::TokenIdentifier);
            import_name.name = name_ident ? name_ident->token.text() : StringView();
            import_name.loc = make_loc(import_name_node);

            names.push_back(import_name);
        }
    }

    decl->import_decl.module_path = module_path;
    decl->import_decl.names = m_allocator.alloc_span(names);
    decl->import_decl.is_from_import = is_from_import;
    return decl;
}

// --- Statement lowering ---

Stmt* CstLowering::lower_stmt(SyntaxNode* node) {
    if (!node || node->kind == SyntaxKind::Error) return nullptr;

    switch (node->kind) {
        case SyntaxKind::NodeBlockStmt:    return lower_block_stmt(node);
        case SyntaxKind::NodeIfStmt:       return lower_if_stmt(node);
        case SyntaxKind::NodeWhileStmt:    return lower_while_stmt(node);
        case SyntaxKind::NodeForStmt:      return lower_for_stmt(node);
        case SyntaxKind::NodeReturnStmt:   return lower_return_stmt(node);
        case SyntaxKind::NodeWhenStmt:     return lower_when_stmt(node);
        case SyntaxKind::NodeTryStmt:      return lower_try_stmt(node);
        case SyntaxKind::NodeThrowStmt:    return lower_throw_stmt(node);
        case SyntaxKind::NodeDeleteStmt:   return lower_delete_stmt(node);
        case SyntaxKind::NodeExprStmt:     return lower_expr_stmt(node);
        case SyntaxKind::NodeBreakStmt: {
            Stmt* stmt = alloc<Stmt>();
            stmt->kind = AstKind::StmtBreak;
            stmt->loc = make_loc(node);
            return stmt;
        }
        case SyntaxKind::NodeContinueStmt: {
            Stmt* stmt = alloc<Stmt>();
            stmt->kind = AstKind::StmtContinue;
            stmt->loc = make_loc(node);
            return stmt;
        }
        default: return nullptr;
    }
}

Stmt* CstLowering::lower_block_stmt(SyntaxNode* node) {
    if (!node) return nullptr;

    Stmt* stmt = alloc<Stmt>();
    stmt->kind = AstKind::StmtBlock;
    stmt->loc = make_loc(node);

    Vector<Decl*> declarations;
    for (u32 i = 0; i < node->children.size(); i++) {
        SyntaxNode* child = node->children[i];
        // Skip brace tokens
        if (child->kind == SyntaxKind::TokenLeftBrace ||
            child->kind == SyntaxKind::TokenRightBrace) continue;

        Decl* decl = lower_top_level_node(child);
        if (decl) declarations.push_back(decl);
    }

    stmt->block.declarations = m_allocator.alloc_span(declarations);
    return stmt;
}

Stmt* CstLowering::lower_if_stmt(SyntaxNode* node) {
    Stmt* stmt = alloc<Stmt>();
    stmt->kind = AstKind::StmtIf;
    stmt->loc = make_loc(node);
    stmt->if_stmt.condition = nullptr;
    stmt->if_stmt.then_branch = nullptr;
    stmt->if_stmt.else_branch = nullptr;

    // Children: 'if', '(', condition_expr, ')', then_stmt, [?'else', else_stmt]
    // Find expression children (skip keywords and parens)
    u32 expr_index = 0;
    u32 stmt_index = 0;
    for (u32 i = 0; i < node->children.size(); i++) {
        SyntaxNode* child = node->children[i];
        if (child->kind == SyntaxKind::TokenKwIf ||
            child->kind == SyntaxKind::TokenLeftParen ||
            child->kind == SyntaxKind::TokenRightParen ||
            child->kind == SyntaxKind::TokenKwElse) continue;

        if (expr_index == 0) {
            // First non-token is the condition expression
            stmt->if_stmt.condition = lower_expr(child);
            expr_index++;
        } else if (stmt_index == 0) {
            stmt->if_stmt.then_branch = lower_stmt(child);
            stmt_index++;
        } else if (stmt_index == 1) {
            stmt->if_stmt.else_branch = lower_stmt(child);
            stmt_index++;
        }
    }

    return stmt;
}

Stmt* CstLowering::lower_while_stmt(SyntaxNode* node) {
    Stmt* stmt = alloc<Stmt>();
    stmt->kind = AstKind::StmtWhile;
    stmt->loc = make_loc(node);
    stmt->while_stmt.condition = nullptr;
    stmt->while_stmt.body = nullptr;

    u32 part_index = 0;
    for (u32 i = 0; i < node->children.size(); i++) {
        SyntaxNode* child = node->children[i];
        if (child->kind == SyntaxKind::TokenKwWhile ||
            child->kind == SyntaxKind::TokenLeftParen ||
            child->kind == SyntaxKind::TokenRightParen) continue;

        if (part_index == 0) {
            stmt->while_stmt.condition = lower_expr(child);
            part_index++;
        } else if (part_index == 1) {
            stmt->while_stmt.body = lower_stmt(child);
            part_index++;
        }
    }

    return stmt;
}

Stmt* CstLowering::lower_for_stmt(SyntaxNode* node) {
    Stmt* stmt = alloc<Stmt>();
    stmt->kind = AstKind::StmtFor;
    stmt->loc = make_loc(node);
    stmt->for_stmt.initializer = nullptr;
    stmt->for_stmt.condition = nullptr;
    stmt->for_stmt.increment = nullptr;
    stmt->for_stmt.body = nullptr;

    // For statement CST: 'for', '(', init, cond, ';', incr, ')', body
    // The init can be a VarDecl, ExprStmt, or just ';'
    // We need to parse based on the child structure
    u32 semi_count = 0;
    u32 part_index = 0;
    bool past_lparen = false;
    bool past_rparen = false;
    for (u32 i = 0; i < node->children.size(); i++) {
        SyntaxNode* child = node->children[i];
        if (child->kind == SyntaxKind::TokenKwFor) continue;
        if (child->kind == SyntaxKind::TokenLeftParen) { past_lparen = true; continue; }
        if (child->kind == SyntaxKind::TokenRightParen) { past_rparen = true; continue; }
        if (!past_lparen) continue;

        if (past_rparen) {
            // Body
            stmt->for_stmt.body = lower_stmt(child);
            break;
        }

        if (child->kind == SyntaxKind::TokenSemicolon) {
            semi_count++;
            continue;
        }

        // Map parts by semicolon count
        if (semi_count == 0) {
            // Initializer
            stmt->for_stmt.initializer = lower_top_level_node(child);
        } else if (semi_count == 1) {
            // Condition
            stmt->for_stmt.condition = lower_expr(child);
        } else {
            // Increment
            stmt->for_stmt.increment = lower_expr(child);
        }
    }

    return stmt;
}

Stmt* CstLowering::lower_return_stmt(SyntaxNode* node) {
    Stmt* stmt = alloc<Stmt>();
    stmt->kind = AstKind::StmtReturn;
    stmt->loc = make_loc(node);
    stmt->return_stmt.value = nullptr;

    for (u32 i = 0; i < node->children.size(); i++) {
        SyntaxNode* child = node->children[i];
        if (child->kind == SyntaxKind::TokenKwReturn ||
            child->kind == SyntaxKind::TokenSemicolon) continue;
        stmt->return_stmt.value = lower_expr(child);
        break;
    }

    return stmt;
}

Stmt* CstLowering::lower_when_stmt(SyntaxNode* node) {
    Stmt* stmt = alloc<Stmt>();
    stmt->kind = AstKind::StmtWhen;
    stmt->loc = make_loc(node);
    stmt->when_stmt.discriminant = nullptr;
    stmt->when_stmt.else_body = Span<Decl*>();
    stmt->when_stmt.else_loc = SourceLocation{0, 0, 0, 0};

    // Find discriminant: identifier(s) after 'when'
    bool past_when = false;
    for (u32 i = 0; i < node->children.size(); i++) {
        SyntaxNode* child = node->children[i];
        if (child->kind == SyntaxKind::TokenKwWhen) { past_when = true; continue; }
        if (child->kind == SyntaxKind::TokenLeftBrace) break;
        if (past_when && child->kind == SyntaxKind::TokenIdentifier) {
            // First identifier = discriminant
            Expr* expr = alloc<Expr>();
            expr->kind = AstKind::ExprIdentifier;
            expr->loc = make_loc(child);
            expr->identifier.name = child->token.text();
            stmt->when_stmt.discriminant = expr;
            break;
        }
    }

    // Collect cases
    Vector<WhenCase> cases;
    for (u32 i = 0; i < node->children.size(); i++) {
        if (node->children[i]->kind != SyntaxKind::NodeWhenCase) continue;
        SyntaxNode* case_node = node->children[i];

        // Check if this is an else case
        bool is_else = has_child(case_node, SyntaxKind::TokenKwElse);
        if (is_else) {
            // Else body
            Vector<Decl*> else_decls;
            for (u32 j = 0; j < case_node->children.size(); j++) {
                SyntaxNode* case_child = case_node->children[j];
                if (case_child->kind == SyntaxKind::TokenKwElse ||
                    case_child->kind == SyntaxKind::TokenColon) continue;
                Decl* decl = lower_top_level_node(case_child);
                if (decl) else_decls.push_back(decl);
            }
            stmt->when_stmt.else_body = m_allocator.alloc_span(else_decls);
            stmt->when_stmt.else_loc = make_loc(case_node);
            continue;
        }

        WhenCase wc;
        wc.loc = make_loc(case_node);

        // Case names
        Vector<StringView> case_names;
        bool past_case = false;
        for (u32 j = 0; j < case_node->children.size(); j++) {
            SyntaxNode* case_child = case_node->children[j];
            if (case_child->kind == SyntaxKind::TokenKwCase) { past_case = true; continue; }
            if (case_child->kind == SyntaxKind::TokenColon) break;
            if (past_case && case_child->kind == SyntaxKind::TokenIdentifier) {
                case_names.push_back(case_child->token.text());
            }
        }
        wc.case_names = m_allocator.alloc_span(case_names);

        // Case body
        Vector<Decl*> body_decls;
        bool past_colon = false;
        for (u32 j = 0; j < case_node->children.size(); j++) {
            SyntaxNode* case_child = case_node->children[j];
            if (case_child->kind == SyntaxKind::TokenColon) { past_colon = true; continue; }
            if (!past_colon) continue;
            Decl* decl = lower_top_level_node(case_child);
            if (decl) body_decls.push_back(decl);
        }
        wc.body = m_allocator.alloc_span(body_decls);

        cases.push_back(wc);
    }

    stmt->when_stmt.cases = m_allocator.alloc_span(cases);
    return stmt;
}

Stmt* CstLowering::lower_try_stmt(SyntaxNode* node) {
    Stmt* stmt = alloc<Stmt>();
    stmt->kind = AstKind::StmtTry;
    stmt->loc = make_loc(node);
    stmt->try_stmt.try_body = nullptr;
    stmt->try_stmt.catches = Span<CatchClause>();
    stmt->try_stmt.finally_body = nullptr;

    // Find try body (first block stmt)
    SyntaxNode* try_block = find_child(node, SyntaxKind::NodeBlockStmt);
    if (try_block) stmt->try_stmt.try_body = lower_block_stmt(try_block);

    // Catch clauses
    Vector<CatchClause> catches;
    for (u32 i = 0; i < node->children.size(); i++) {
        if (node->children[i]->kind != SyntaxKind::NodeCatchClause) continue;
        SyntaxNode* catch_node = node->children[i];

        CatchClause cc;
        cc.loc = make_loc(catch_node);
        cc.var_name = StringView();
        cc.exception_type = nullptr;
        cc.body = nullptr;
        cc.resolved_type = nullptr;

        // Find variable name
        SyntaxNode* var_name_node = find_child(catch_node, SyntaxKind::TokenIdentifier);
        if (var_name_node) cc.var_name = var_name_node->token.text();

        // Find exception type
        SyntaxNode* type_node = find_child(catch_node, SyntaxKind::NodeTypeExpr);
        if (type_node) cc.exception_type = lower_type_expr(type_node);

        // Find body (block stmt)
        SyntaxNode* body_block = find_child(catch_node, SyntaxKind::NodeBlockStmt);
        if (body_block) cc.body = lower_block_stmt(body_block);

        catches.push_back(cc);
    }
    stmt->try_stmt.catches = m_allocator.alloc_span(catches);

    // Finally: look for block after 'finally' keyword
    bool past_finally = false;
    for (u32 i = 0; i < node->children.size(); i++) {
        if (node->children[i]->kind == SyntaxKind::TokenKwFinally) {
            past_finally = true;
        } else if (past_finally && node->children[i]->kind == SyntaxKind::NodeBlockStmt) {
            stmt->try_stmt.finally_body = lower_block_stmt(node->children[i]);
            break;
        }
    }

    return stmt;
}

Stmt* CstLowering::lower_throw_stmt(SyntaxNode* node) {
    Stmt* stmt = alloc<Stmt>();
    stmt->kind = AstKind::StmtThrow;
    stmt->loc = make_loc(node);
    stmt->throw_stmt.expr = nullptr;

    for (u32 i = 0; i < node->children.size(); i++) {
        SyntaxNode* child = node->children[i];
        if (child->kind == SyntaxKind::TokenKwThrow ||
            child->kind == SyntaxKind::TokenSemicolon) continue;
        stmt->throw_stmt.expr = lower_expr(child);
        break;
    }

    return stmt;
}

Stmt* CstLowering::lower_delete_stmt(SyntaxNode* node) {
    Stmt* stmt = alloc<Stmt>();
    stmt->kind = AstKind::StmtDelete;
    stmt->loc = make_loc(node);
    stmt->delete_stmt.expr = nullptr;
    stmt->delete_stmt.destructor_name = StringView();
    stmt->delete_stmt.arguments = Span<CallArg>();

    for (u32 i = 0; i < node->children.size(); i++) {
        SyntaxNode* child = node->children[i];
        if (child->kind == SyntaxKind::TokenKwDelete ||
            child->kind == SyntaxKind::TokenSemicolon) continue;
        stmt->delete_stmt.expr = lower_expr(child);
        break;
    }

    return stmt;
}

Stmt* CstLowering::lower_yield_stmt(SyntaxNode* node) {
    Stmt* stmt = alloc<Stmt>();
    stmt->kind = AstKind::StmtYield;
    stmt->loc = make_loc(node);
    stmt->yield_stmt.value = nullptr;

    for (u32 i = 0; i < node->children.size(); i++) {
        SyntaxNode* child = node->children[i];
        if (child->kind == SyntaxKind::TokenKwYield ||
            child->kind == SyntaxKind::TokenSemicolon) continue;
        stmt->yield_stmt.value = lower_expr(child);
        break;
    }

    return stmt;
}

Stmt* CstLowering::lower_expr_stmt(SyntaxNode* node) {
    Stmt* stmt = alloc<Stmt>();
    stmt->kind = AstKind::StmtExpr;
    stmt->loc = make_loc(node);
    stmt->expr_stmt.expr = nullptr;

    for (u32 i = 0; i < node->children.size(); i++) {
        SyntaxNode* child = node->children[i];
        if (child->kind == SyntaxKind::TokenSemicolon) continue;
        stmt->expr_stmt.expr = lower_expr(child);
        break;
    }

    return stmt;
}

// --- Expression lowering ---

Expr* CstLowering::lower_expr(SyntaxNode* node) {
    if (!node || node->kind == SyntaxKind::Error) return nullptr;

    switch (node->kind) {
        case SyntaxKind::NodeLiteralExpr:         return lower_literal_expr(node);
        case SyntaxKind::NodeIdentifierExpr:      return lower_identifier_expr(node);
        case SyntaxKind::NodeUnaryExpr:           return lower_unary_expr(node);
        case SyntaxKind::NodeBinaryExpr:          return lower_binary_expr(node);
        case SyntaxKind::NodeTernaryExpr:         return lower_ternary_expr(node);
        case SyntaxKind::NodeCallExpr:            return lower_call_expr(node);
        case SyntaxKind::NodeIndexExpr:           return lower_index_expr(node);
        case SyntaxKind::NodeGetExpr:             return lower_get_expr(node);
        case SyntaxKind::NodeStaticGetExpr:       return lower_static_get_expr(node);
        case SyntaxKind::NodeAssignExpr:          return lower_assign_expr(node);
        case SyntaxKind::NodeGroupingExpr:        return lower_grouping_expr(node);
        case SyntaxKind::NodeSelfExpr:            return lower_self_expr(node);
        case SyntaxKind::NodeSuperExpr:           return lower_super_expr(node);
        case SyntaxKind::NodeStructLiteralExpr:   return lower_struct_literal_expr(node);
        case SyntaxKind::NodeStringInterpExpr:    return lower_string_interp_expr(node);
        case SyntaxKind::NodeUniqExpr:            return lower_call_expr(node); // uniq calls
        case SyntaxKind::NodeRefExpr:            return lower_ref_expr(node);
        default: return nullptr;
    }
}

Expr* CstLowering::lower_literal_expr(SyntaxNode* node) {
    Expr* expr = alloc<Expr>();
    expr->kind = AstKind::ExprLiteral;
    expr->loc = make_loc(node);

    SyntaxNode* token_node = nullptr;
    for (u32 i = 0; i < node->children.size(); i++) {
        token_node = node->children[i];
        break;
    }

    if (!token_node) {
        expr->literal.literal_kind = LiteralKind::Nil;
        return expr;
    }

    switch (token_node->kind) {
        case SyntaxKind::TokenKwNil:
            expr->literal.literal_kind = LiteralKind::Nil;
            break;
        case SyntaxKind::TokenKwTrue:
            expr->literal.literal_kind = LiteralKind::Bool;
            expr->literal.bool_value = true;
            break;
        case SyntaxKind::TokenKwFalse:
            expr->literal.literal_kind = LiteralKind::Bool;
            expr->literal.bool_value = false;
            break;
        case SyntaxKind::TokenIntLiteral:
            expr->literal.literal_kind = LiteralKind::I32;
            expr->literal.int_value = token_node->token.int_value;
            break;
        case SyntaxKind::TokenFloatLiteral:
            expr->literal.literal_kind = LiteralKind::F64;
            expr->literal.float_value = token_node->token.float_value;
            break;
        case SyntaxKind::TokenStringLiteral:
            expr->literal.literal_kind = LiteralKind::String;
            expr->literal.string_value = token_node->token.text();
            break;
        default:
            expr->literal.literal_kind = LiteralKind::Nil;
            break;
    }

    return expr;
}

Expr* CstLowering::lower_identifier_expr(SyntaxNode* node) {
    Expr* expr = alloc<Expr>();
    expr->kind = AstKind::ExprIdentifier;
    expr->loc = make_loc(node);

    SyntaxNode* ident = find_child(node, SyntaxKind::TokenIdentifier);
    expr->identifier.name = ident ? ident->token.text() : StringView();

    return expr;
}

Expr* CstLowering::lower_unary_expr(SyntaxNode* node) {
    Expr* expr = alloc<Expr>();
    expr->kind = AstKind::ExprUnary;
    expr->loc = make_loc(node);
    expr->unary.operand = nullptr;

    if (node->children.size() >= 2) {
        expr->unary.op = syntax_kind_to_unary_op(node->children[0]->kind);
        expr->unary.operand = lower_expr(node->children[1]);
    }

    return expr;
}

Expr* CstLowering::lower_ref_expr(SyntaxNode* node) {
    Expr* expr = alloc<Expr>();
    expr->kind = AstKind::ExprUnary;
    expr->loc = make_loc(node);
    expr->unary.op = UnaryOp::Ref;
    expr->unary.operand = nullptr;

    // children[0] is the 'ref' token, children[1] is the operand expression
    if (node->children.size() >= 2) {
        expr->unary.operand = lower_expr(node->children[1]);
    }

    return expr;
}

Expr* CstLowering::lower_binary_expr(SyntaxNode* node) {
    Expr* expr = alloc<Expr>();
    expr->kind = AstKind::ExprBinary;
    expr->loc = make_loc(node);
    expr->binary.left = nullptr;
    expr->binary.right = nullptr;

    // Children: left, operator, right
    if (node->children.size() >= 3) {
        expr->binary.left = lower_expr(node->children[0]);
        expr->binary.op = syntax_kind_to_binary_op(node->children[1]->kind);
        expr->binary.right = lower_expr(node->children[2]);
    }

    return expr;
}

Expr* CstLowering::lower_ternary_expr(SyntaxNode* node) {
    Expr* expr = alloc<Expr>();
    expr->kind = AstKind::ExprTernary;
    expr->loc = make_loc(node);
    expr->ternary.condition = nullptr;
    expr->ternary.then_expr = nullptr;
    expr->ternary.else_expr = nullptr;

    // Children: condition, '?', then_expr, ':', else_expr
    u32 expr_index = 0;
    for (u32 i = 0; i < node->children.size(); i++) {
        SyntaxNode* child = node->children[i];
        if (child->kind == SyntaxKind::TokenQuestion ||
            child->kind == SyntaxKind::TokenColon) continue;

        if (expr_index == 0) {
            expr->ternary.condition = lower_expr(child);
        } else if (expr_index == 1) {
            expr->ternary.then_expr = lower_expr(child);
        } else if (expr_index == 2) {
            expr->ternary.else_expr = lower_expr(child);
        }
        expr_index++;
    }

    return expr;
}

Expr* CstLowering::lower_call_expr(SyntaxNode* node) {
    Expr* expr = alloc<Expr>();
    expr->kind = AstKind::ExprCall;
    expr->loc = make_loc(node);
    expr->call.callee = nullptr;
    expr->call.arguments = Span<CallArg>();
    expr->call.type_args = Span<TypeExpr*>();
    expr->call.constructor_name = StringView();
    expr->call.mangled_name = StringView();
    expr->call.is_heap = false;

    // Children: callee, [type_args,] '(', [args...], ')'
    // For uniq: 'uniq', type_name, ['.', ctor_name], '(', [args...], ')'

    // Check for uniq prefix
    bool is_uniq = has_child(node, SyntaxKind::TokenKwUniq);

    if (is_uniq) {
        expr->call.is_heap = true;
        // Find the type name (first identifier after 'uniq')
        bool past_uniq = false;
        for (u32 i = 0; i < node->children.size(); i++) {
            SyntaxNode* child = node->children[i];
            if (child->kind == SyntaxKind::TokenKwUniq) { past_uniq = true; continue; }
            if (past_uniq && child->kind == SyntaxKind::TokenIdentifier) {
                Expr* callee = alloc<Expr>();
                callee->kind = AstKind::ExprIdentifier;
                callee->loc = make_loc(child);
                callee->identifier.name = child->token.text();
                expr->call.callee = callee;
                break;
            }
        }

        // Check for named constructor
        bool past_dot = false;
        for (u32 i = 0; i < node->children.size(); i++) {
            if (node->children[i]->kind == SyntaxKind::TokenDot) {
                past_dot = true;
            } else if (past_dot && node->children[i]->kind == SyntaxKind::TokenIdentifier) {
                expr->call.constructor_name = node->children[i]->token.text();
                break;
            }
        }
    } else {
        // Regular call: first child is the callee expression
        if (node->children.size() > 0) {
            SyntaxNode* first = node->children[0];
            if (first->kind != SyntaxKind::TokenLeftParen &&
                first->kind != SyntaxKind::NodeTypeArgList) {
                expr->call.callee = lower_expr(first);
            }
        }
    }

    // Collect call arguments
    Vector<CallArg> args;
    for (u32 i = 0; i < node->children.size(); i++) {
        if (node->children[i]->kind != SyntaxKind::NodeCallArg) continue;
        SyntaxNode* arg_node = node->children[i];

        CallArg arg;
        arg.expr = nullptr;
        arg.modifier = ParamModifier::None;
        arg.modifier_loc = SourceLocation{0, 0, 0, 0};

        for (u32 j = 0; j < arg_node->children.size(); j++) {
            SyntaxNode* arg_child = arg_node->children[j];
            if (arg_child->kind == SyntaxKind::TokenKwOut ||
                arg_child->kind == SyntaxKind::TokenKwInout) {
                arg.modifier = syntax_kind_to_param_modifier(arg_child->kind);
                arg.modifier_loc = make_loc(arg_child);
            } else {
                arg.expr = lower_expr(arg_child);
            }
        }

        args.push_back(arg);
    }
    expr->call.arguments = m_allocator.alloc_span(args);

    // Type args
    SyntaxNode* type_arg_list = find_child(node, SyntaxKind::NodeTypeArgList);
    if (type_arg_list) {
        Vector<TypeExpr*> type_args;
        for (u32 i = 0; i < type_arg_list->children.size(); i++) {
            if (type_arg_list->children[i]->kind == SyntaxKind::NodeTypeArg) {
                SyntaxNode* type_arg_node = type_arg_list->children[i];
                SyntaxNode* type_expr_node = find_child(type_arg_node, SyntaxKind::NodeTypeExpr);
                if (type_expr_node) {
                    TypeExpr* te = lower_type_expr(type_expr_node);
                    if (te) type_args.push_back(te);
                }
            }
        }
        expr->call.type_args = m_allocator.alloc_span(type_args);
    }

    return expr;
}

Expr* CstLowering::lower_index_expr(SyntaxNode* node) {
    Expr* expr = alloc<Expr>();
    expr->kind = AstKind::ExprIndex;
    expr->loc = make_loc(node);
    expr->index.object = nullptr;
    expr->index.index = nullptr;

    // Children: object, '[', index_expr, ']'
    u32 expr_index = 0;
    for (u32 i = 0; i < node->children.size(); i++) {
        SyntaxNode* child = node->children[i];
        if (child->kind == SyntaxKind::TokenLeftBracket ||
            child->kind == SyntaxKind::TokenRightBracket) continue;

        if (expr_index == 0) {
            expr->index.object = lower_expr(child);
        } else if (expr_index == 1) {
            expr->index.index = lower_expr(child);
        }
        expr_index++;
    }

    return expr;
}

Expr* CstLowering::lower_get_expr(SyntaxNode* node) {
    Expr* expr = alloc<Expr>();
    expr->kind = AstKind::ExprGet;
    expr->loc = make_loc(node);
    expr->get.object = nullptr;
    expr->get.name = StringView();

    // Children: object, '.', member_name
    if (node->children.size() >= 3) {
        expr->get.object = lower_expr(node->children[0]);
        // Last child is the member name token
        SyntaxNode* name_child = node->children[node->children.size() - 1];
        if (name_child->kind == SyntaxKind::TokenIdentifier) {
            expr->get.name = name_child->token.text();
        }
    }

    return expr;
}

Expr* CstLowering::lower_static_get_expr(SyntaxNode* node) {
    Expr* expr = alloc<Expr>();
    expr->kind = AstKind::ExprStaticGet;
    expr->loc = make_loc(node);
    expr->static_get.type_name = StringView();
    expr->static_get.member_name = StringView();

    // Children: type_name, '::', member_name
    u32 ident_index = 0;
    for (u32 i = 0; i < node->children.size(); i++) {
        if (node->children[i]->kind == SyntaxKind::TokenIdentifier) {
            if (ident_index == 0) {
                expr->static_get.type_name = node->children[i]->token.text();
            } else {
                expr->static_get.member_name = node->children[i]->token.text();
            }
            ident_index++;
        }
    }

    return expr;
}

Expr* CstLowering::lower_assign_expr(SyntaxNode* node) {
    Expr* expr = alloc<Expr>();
    expr->kind = AstKind::ExprAssign;
    expr->loc = make_loc(node);
    expr->assign.target = nullptr;
    expr->assign.value = nullptr;
    expr->assign.op = AssignOp::Assign;

    // Children: target, op_token, value
    if (node->children.size() >= 3) {
        expr->assign.target = lower_expr(node->children[0]);
        expr->assign.op = syntax_kind_to_assign_op(node->children[1]->kind);
        expr->assign.value = lower_expr(node->children[2]);
    }

    return expr;
}

Expr* CstLowering::lower_grouping_expr(SyntaxNode* node) {
    Expr* expr = alloc<Expr>();
    expr->kind = AstKind::ExprGrouping;
    expr->loc = make_loc(node);
    expr->grouping.expr = nullptr;

    // Children: '(', inner_expr, ')'
    for (u32 i = 0; i < node->children.size(); i++) {
        SyntaxNode* child = node->children[i];
        if (child->kind == SyntaxKind::TokenLeftParen ||
            child->kind == SyntaxKind::TokenRightParen) continue;
        expr->grouping.expr = lower_expr(child);
        break;
    }

    return expr;
}

Expr* CstLowering::lower_self_expr(SyntaxNode* node) {
    Expr* expr = alloc<Expr>();
    expr->kind = AstKind::ExprThis;
    expr->loc = make_loc(node);
    return expr;
}

Expr* CstLowering::lower_super_expr(SyntaxNode* node) {
    Expr* expr = alloc<Expr>();
    expr->kind = AstKind::ExprSuper;
    expr->loc = make_loc(node);
    expr->super_expr.method_name = StringView();

    // Children: 'super', ['.', method_name]
    bool past_dot = false;
    for (u32 i = 0; i < node->children.size(); i++) {
        if (node->children[i]->kind == SyntaxKind::TokenDot) {
            past_dot = true;
        } else if (past_dot && node->children[i]->kind == SyntaxKind::TokenIdentifier) {
            expr->super_expr.method_name = node->children[i]->token.text();
            break;
        }
    }

    return expr;
}

Expr* CstLowering::lower_struct_literal_expr(SyntaxNode* node) {
    Expr* expr = alloc<Expr>();
    expr->kind = AstKind::ExprStructLiteral;
    expr->loc = make_loc(node);
    expr->struct_literal.type_name = StringView();
    expr->struct_literal.fields = Span<FieldInit>();
    expr->struct_literal.type_args = Span<TypeExpr*>();
    expr->struct_literal.mangled_name = StringView();
    expr->struct_literal.is_heap = has_child(node, SyntaxKind::TokenKwUniq);

    // Find type name (first identifier)
    SyntaxNode* type_ident = find_child(node, SyntaxKind::TokenIdentifier);
    if (type_ident) expr->struct_literal.type_name = type_ident->token.text();

    // Collect field initializers
    Vector<FieldInit> fields;
    for (u32 i = 0; i < node->children.size(); i++) {
        if (node->children[i]->kind != SyntaxKind::NodeFieldInit) continue;
        SyntaxNode* field_init_node = node->children[i];

        FieldInit field;
        field.value = nullptr;
        field.loc = make_loc(field_init_node);

        // Children: name, '=', value_expr
        SyntaxNode* field_name = find_child(field_init_node, SyntaxKind::TokenIdentifier);
        field.name = field_name ? field_name->token.text() : StringView();

        // Value: expression after '='
        bool past_equal = false;
        for (u32 j = 0; j < field_init_node->children.size(); j++) {
            if (field_init_node->children[j]->kind == SyntaxKind::TokenEqual) {
                past_equal = true;
            } else if (past_equal) {
                field.value = lower_expr(field_init_node->children[j]);
                break;
            }
        }

        fields.push_back(field);
    }
    expr->struct_literal.fields = m_allocator.alloc_span(fields);

    // Type args
    SyntaxNode* type_arg_list = find_child(node, SyntaxKind::NodeTypeArgList);
    if (type_arg_list) {
        Vector<TypeExpr*> type_args;
        for (u32 i = 0; i < type_arg_list->children.size(); i++) {
            if (type_arg_list->children[i]->kind == SyntaxKind::NodeTypeArg) {
                SyntaxNode* type_arg_node = type_arg_list->children[i];
                SyntaxNode* type_expr_node = find_child(type_arg_node, SyntaxKind::NodeTypeExpr);
                if (type_expr_node) {
                    TypeExpr* te = lower_type_expr(type_expr_node);
                    if (te) type_args.push_back(te);
                }
            }
        }
        expr->struct_literal.type_args = m_allocator.alloc_span(type_args);
    }

    return expr;
}

Expr* CstLowering::lower_string_interp_expr(SyntaxNode* node) {
    Expr* expr = alloc<Expr>();
    expr->kind = AstKind::ExprStringInterp;
    expr->loc = make_loc(node);

    Vector<StringView> parts;
    Vector<Expr*> expressions;

    for (u32 i = 0; i < node->children.size(); i++) {
        SyntaxNode* child = node->children[i];
        if (child->kind == SyntaxKind::TokenFStringBegin ||
            child->kind == SyntaxKind::TokenFStringMid ||
            child->kind == SyntaxKind::TokenFStringEnd) {
            parts.push_back(child->token.text());
        } else {
            Expr* interp = lower_expr(child);
            if (interp) expressions.push_back(interp);
        }
    }

    expr->string_interp.parts = m_allocator.alloc_span(parts);
    expr->string_interp.expressions = m_allocator.alloc_span(expressions);
    return expr;
}

// --- Type expression lowering ---

TypeExpr* CstLowering::lower_type_expr(SyntaxNode* node) {
    if (!node || node->kind != SyntaxKind::NodeTypeExpr) return nullptr;

    TypeExpr* type_expr = m_allocator.emplace<TypeExpr>();
    type_expr->name = StringView();
    type_expr->loc = make_loc(node);
    type_expr->ref_kind = RefKind::None;
    type_expr->type_args = Span<TypeExpr*>();

    // Check for ref kind (uniq, ref, weak)
    for (u32 i = 0; i < node->children.size(); i++) {
        SyntaxNode* child = node->children[i];
        if (child->kind == SyntaxKind::TokenKwUniq ||
            child->kind == SyntaxKind::TokenKwRef ||
            child->kind == SyntaxKind::TokenKwWeak) {
            type_expr->ref_kind = syntax_kind_to_ref_kind(child->kind);
            break;
        }
    }

    // Find type name (identifier)
    SyntaxNode* ident = find_child(node, SyntaxKind::TokenIdentifier);
    if (ident) type_expr->name = ident->token.text();

    // Type args
    SyntaxNode* type_arg_list = find_child(node, SyntaxKind::NodeTypeArgList);
    if (type_arg_list) {
        Vector<TypeExpr*> type_args;
        for (u32 i = 0; i < type_arg_list->children.size(); i++) {
            if (type_arg_list->children[i]->kind == SyntaxKind::NodeTypeArg) {
                SyntaxNode* type_arg_node = type_arg_list->children[i];
                SyntaxNode* inner_type = find_child(type_arg_node, SyntaxKind::NodeTypeExpr);
                if (inner_type) {
                    TypeExpr* te = lower_type_expr(inner_type);
                    if (te) type_args.push_back(te);
                }
            }
        }
        type_expr->type_args = m_allocator.alloc_span(type_args);
    }

    return type_expr;
}

Span<Param> CstLowering::lower_param_list(SyntaxNode* node) {
    if (!node) return Span<Param>();

    Vector<Param> params;
    for (u32 i = 0; i < node->children.size(); i++) {
        SyntaxNode* param_node = node->children[i];
        if (param_node->kind != SyntaxKind::NodeParam) continue;

        Param param;
        param.modifier = ParamModifier::None;
        param.type = nullptr;
        param.resolved_type = nullptr;
        param.loc = make_loc(param_node);

        // Check for modifier (out/inout)
        for (u32 j = 0; j < param_node->children.size(); j++) {
            SyntaxNode* child = param_node->children[j];
            if (child->kind == SyntaxKind::TokenKwOut ||
                child->kind == SyntaxKind::TokenKwInout) {
                param.modifier = syntax_kind_to_param_modifier(child->kind);
                break;
            }
        }

        // Name
        SyntaxNode* name_node = find_child(param_node, SyntaxKind::TokenIdentifier);
        param.name = name_node ? name_node->token.text() : StringView();

        // Type
        SyntaxNode* type_node = find_child(param_node, SyntaxKind::NodeTypeExpr);
        if (type_node) param.type = lower_type_expr(type_node);

        params.push_back(param);
    }

    return m_allocator.alloc_span(params);
}

Span<TypeParam> CstLowering::lower_type_param_list(SyntaxNode* node) {
    if (!node) return Span<TypeParam>();

    Vector<TypeParam> type_params;
    for (u32 i = 0; i < node->children.size(); i++) {
        SyntaxNode* tp_node = node->children[i];
        if (tp_node->kind != SyntaxKind::NodeTypeParam) continue;

        TypeParam tp;
        tp.bounds = Span<TypeExpr*>();
        tp.loc = make_loc(tp_node);

        SyntaxNode* name_node = find_child(tp_node, SyntaxKind::TokenIdentifier);
        tp.name = name_node ? name_node->token.text() : StringView();

        type_params.push_back(tp);
    }

    return m_allocator.alloc_span(type_params);
}

} // namespace rx
