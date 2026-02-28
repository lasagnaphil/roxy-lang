#include "roxy/lsp/indexer.hpp"

namespace rx {

// --- Helpers ---

SyntaxNode* FileIndexer::find_child(SyntaxNode* node, SyntaxKind kind) {
    for (u32 i = 0; i < node->children.size(); i++) {
        if (node->children[i]->kind == kind) return node->children[i];
    }
    return nullptr;
}

SyntaxNode* FileIndexer::find_child_after(SyntaxNode* node, SyntaxKind kind, u32 start_index) {
    for (u32 i = start_index; i < node->children.size(); i++) {
        if (node->children[i]->kind == kind) return node->children[i];
    }
    return nullptr;
}

StringView FileIndexer::get_identifier_text(SyntaxNode* node) {
    if (!node) return StringView("", 0);
    if (node->kind == SyntaxKind::TokenIdentifier) {
        return node->token.text();
    }
    return StringView("", 0);
}

TextRange FileIndexer::get_node_range(SyntaxNode* node) {
    if (!node) return TextRange{0, 0};
    return node->range;
}

bool FileIndexer::has_child(SyntaxNode* node, SyntaxKind kind) {
    return find_child(node, kind) != nullptr;
}

Vector<ParamStub> FileIndexer::extract_params(SyntaxNode* param_list) {
    Vector<ParamStub> params;
    if (!param_list) return params;

    for (u32 i = 0; i < param_list->children.size(); i++) {
        SyntaxNode* param_node = param_list->children[i];
        if (param_node->kind != SyntaxKind::NodeParam) continue;

        ParamStub param;
        param.range = param_node->range;

        // Find param name (first identifier)
        SyntaxNode* name_node = find_child(param_node, SyntaxKind::TokenIdentifier);
        if (name_node) {
            param.name = name_node->token.text();
        }

        // Find type
        SyntaxNode* type_node = find_child(param_node, SyntaxKind::NodeTypeExpr);
        if (type_node) {
            param.type = extract_type_ref(type_node);
        }

        params.push_back(std::move(param));
    }

    return params;
}

TypeRef FileIndexer::extract_type_ref(SyntaxNode* type_expr_node) {
    TypeRef ref;
    ref.range = TextRange{0, 0};
    if (!type_expr_node) return ref;

    ref.range = type_expr_node->range;

    // Find the first identifier inside the type expression for the type name
    SyntaxNode* ident = find_child(type_expr_node, SyntaxKind::TokenIdentifier);
    if (ident) {
        ref.name = ident->token.text();
    }

    return ref;
}

// --- Main index entry point ---

FileStubs FileIndexer::index(SyntaxNode* root) {
    FileStubs stubs;
    if (!root || root->kind != SyntaxKind::NodeProgram) return stubs;

    for (u32 i = 0; i < root->children.size(); i++) {
        SyntaxNode* child = root->children[i];
        switch (child->kind) {
            case SyntaxKind::NodeVarDecl:
                index_var_decl(child, stubs);
                break;
            case SyntaxKind::NodeFunDecl:
                index_fun_decl(child, stubs);
                break;
            case SyntaxKind::NodeMethodDecl:
                index_method_decl(child, stubs);
                break;
            case SyntaxKind::NodeConstructorDecl:
                index_constructor_decl(child, stubs);
                break;
            case SyntaxKind::NodeDestructorDecl:
                index_destructor_decl(child, stubs);
                break;
            case SyntaxKind::NodeStructDecl:
                index_struct_decl(child, stubs);
                break;
            case SyntaxKind::NodeEnumDecl:
                index_enum_decl(child, stubs);
                break;
            case SyntaxKind::NodeTraitDecl:
                index_trait_decl(child, stubs);
                break;
            case SyntaxKind::NodeImportDecl:
                index_import_decl(child, stubs);
                break;
            default:
                break;
        }
    }

    return stubs;
}

// --- Declaration indexers ---

void FileIndexer::index_var_decl(SyntaxNode* node, FileStubs& stubs) {
    GlobalVarStub stub;
    stub.range = node->range;
    stub.has_initializer = false;

    // Find name (first identifier that isn't part of modifier keywords)
    SyntaxNode* name_node = find_child(node, SyntaxKind::TokenIdentifier);
    if (name_node) {
        stub.name = name_node->token.text();
        stub.name_range = name_node->range;
    }

    // Find type
    SyntaxNode* type_node = find_child(node, SyntaxKind::NodeTypeExpr);
    if (type_node) {
        stub.type = extract_type_ref(type_node);
    }

    // Check for initializer (has '=' token)
    stub.has_initializer = has_child(node, SyntaxKind::TokenEqual);

    stubs.globals.push_back(std::move(stub));
}

void FileIndexer::index_fun_decl(SyntaxNode* node, FileStubs& stubs) {
    FunctionStub stub;
    stub.range = node->range;
    stub.is_pub = has_child(node, SyntaxKind::TokenKwPub);
    stub.is_native = has_child(node, SyntaxKind::TokenKwNative);
    stub.has_body = has_child(node, SyntaxKind::NodeBlockStmt);

    // Find name (first identifier)
    SyntaxNode* name_node = find_child(node, SyntaxKind::TokenIdentifier);
    if (name_node) {
        stub.name = name_node->token.text();
        stub.name_range = name_node->range;
    }

    // Extract params
    SyntaxNode* param_list = find_child(node, SyntaxKind::NodeParamList);
    if (param_list) {
        stub.params = extract_params(param_list);
    }

    // Find return type — NodeTypeExpr after the ')' token
    // Look for NodeTypeExpr that comes after the param list
    SyntaxNode* return_type_node = nullptr;
    bool past_rparen = false;
    for (u32 i = 0; i < node->children.size(); i++) {
        if (node->children[i]->kind == SyntaxKind::TokenRightParen) {
            past_rparen = true;
        } else if (past_rparen && node->children[i]->kind == SyntaxKind::NodeTypeExpr) {
            return_type_node = node->children[i];
            break;
        }
    }
    if (return_type_node) {
        stub.return_type = extract_type_ref(return_type_node);
    }

    stubs.functions.push_back(std::move(stub));
}

void FileIndexer::index_method_decl(SyntaxNode* node, FileStubs& stubs) {
    MethodStub stub;
    stub.range = node->range;
    stub.is_pub = has_child(node, SyntaxKind::TokenKwPub);
    stub.is_native = has_child(node, SyntaxKind::TokenKwNative);
    stub.has_body = has_child(node, SyntaxKind::NodeBlockStmt);
    stub.trait_name = StringView("", 0);

    // Structure: [?pub, ?native, struct_name, ?type_params, '.', method_name, '(', params, ')', ...]
    // Find struct_name = first identifier
    SyntaxNode* struct_name_node = find_child(node, SyntaxKind::TokenIdentifier);
    if (struct_name_node) {
        stub.struct_name = struct_name_node->token.text();
    }

    // Find method_name = identifier after '.'
    bool past_dot = false;
    SyntaxNode* method_name_node = nullptr;
    for (u32 i = 0; i < node->children.size(); i++) {
        if (node->children[i]->kind == SyntaxKind::TokenDot) {
            past_dot = true;
        } else if (past_dot) {
            // Method name could be an identifier, 'new', or 'delete'
            if (node->children[i]->kind == SyntaxKind::TokenIdentifier ||
                node->children[i]->kind == SyntaxKind::TokenKwNew ||
                node->children[i]->kind == SyntaxKind::TokenKwDelete) {
                method_name_node = node->children[i];
            }
            break;
        }
    }
    if (method_name_node) {
        stub.method_name = method_name_node->token.text();
        stub.name_range = method_name_node->range;
    }

    // Extract params
    SyntaxNode* param_list = find_child(node, SyntaxKind::NodeParamList);
    if (param_list) {
        stub.params = extract_params(param_list);
    }

    // Find return type
    SyntaxNode* return_type_node = nullptr;
    bool past_rparen = false;
    for (u32 i = 0; i < node->children.size(); i++) {
        if (node->children[i]->kind == SyntaxKind::TokenRightParen) {
            past_rparen = true;
        } else if (past_rparen && node->children[i]->kind == SyntaxKind::NodeTypeExpr) {
            return_type_node = node->children[i];
            break;
        }
    }
    if (return_type_node) {
        stub.return_type = extract_type_ref(return_type_node);
    }

    // Check for "for Trait" clause
    bool past_for = false;
    for (u32 i = 0; i < node->children.size(); i++) {
        if (node->children[i]->kind == SyntaxKind::TokenKwFor) {
            past_for = true;
        } else if (past_for && node->children[i]->kind == SyntaxKind::TokenIdentifier) {
            stub.trait_name = node->children[i]->token.text();
            break;
        }
    }

    stubs.methods.push_back(std::move(stub));
}

void FileIndexer::index_constructor_decl(SyntaxNode* node, FileStubs& stubs) {
    ConstructorStub stub;
    stub.range = node->range;
    stub.is_pub = has_child(node, SyntaxKind::TokenKwPub);
    stub.constructor_name = StringView("", 0);

    // Structure: [?pub, struct_name, ?'.', ?constructor_name, '(', params, ')', ...]
    SyntaxNode* struct_name_node = find_child(node, SyntaxKind::TokenIdentifier);
    if (struct_name_node) {
        stub.struct_name = struct_name_node->token.text();
        stub.name_range = struct_name_node->range;
    }

    // Check for named constructor (has '.' followed by identifier)
    bool past_dot = false;
    for (u32 i = 0; i < node->children.size(); i++) {
        if (node->children[i]->kind == SyntaxKind::TokenDot) {
            past_dot = true;
        } else if (past_dot && node->children[i]->kind == SyntaxKind::TokenIdentifier) {
            stub.constructor_name = node->children[i]->token.text();
            stub.name_range = node->children[i]->range;
            break;
        }
    }

    // Extract params
    SyntaxNode* param_list = find_child(node, SyntaxKind::NodeParamList);
    if (param_list) {
        stub.params = extract_params(param_list);
    }

    stubs.constructors.push_back(std::move(stub));
}

void FileIndexer::index_destructor_decl(SyntaxNode* node, FileStubs& stubs) {
    DestructorStub stub;
    stub.range = node->range;
    stub.is_pub = has_child(node, SyntaxKind::TokenKwPub);

    // Structure: [?pub, struct_name, ...]
    SyntaxNode* struct_name_node = find_child(node, SyntaxKind::TokenIdentifier);
    if (struct_name_node) {
        stub.struct_name = struct_name_node->token.text();
        stub.name_range = struct_name_node->range;
    }

    // Extract params
    SyntaxNode* param_list = find_child(node, SyntaxKind::NodeParamList);
    if (param_list) {
        stub.params = extract_params(param_list);
    }

    stubs.destructors.push_back(std::move(stub));
}

void FileIndexer::index_struct_decl(SyntaxNode* node, FileStubs& stubs) {
    StructStub stub;
    stub.range = node->range;
    stub.is_pub = has_child(node, SyntaxKind::TokenKwPub);

    // Find name
    SyntaxNode* name_node = find_child(node, SyntaxKind::TokenIdentifier);
    if (name_node) {
        stub.name = name_node->token.text();
        stub.name_range = name_node->range;
    }

    // Find parent name: identifier after ':'
    bool past_colon = false;
    for (u32 i = 0; i < node->children.size(); i++) {
        if (node->children[i]->kind == SyntaxKind::TokenColon) {
            past_colon = true;
        } else if (past_colon && node->children[i]->kind == SyntaxKind::TokenIdentifier) {
            stub.parent_name = node->children[i]->token.text();
            break;
        } else if (node->children[i]->kind == SyntaxKind::TokenLeftBrace) {
            break; // Stop at '{'
        }
    }

    // Extract fields and nested methods
    for (u32 i = 0; i < node->children.size(); i++) {
        SyntaxNode* child = node->children[i];
        if (child->kind == SyntaxKind::NodeFieldDecl) {
            FieldStub field;
            field.range = child->range;
            field.is_pub = has_child(child, SyntaxKind::TokenKwPub);
            field.has_default = has_child(child, SyntaxKind::TokenEqual);

            SyntaxNode* field_name = find_child(child, SyntaxKind::TokenIdentifier);
            if (field_name) {
                field.name = field_name->token.text();
                field.name_range = field_name->range;
            }

            SyntaxNode* field_type = find_child(child, SyntaxKind::NodeTypeExpr);
            if (field_type) {
                field.type = extract_type_ref(field_type);
            }

            stub.fields.push_back(std::move(field));
        } else if (child->kind == SyntaxKind::NodeFunDecl) {
            // Method defined inside struct body — add to top-level functions
            index_fun_decl(child, stubs);
        } else if (child->kind == SyntaxKind::NodeMethodDecl) {
            index_method_decl(child, stubs);
        }
    }

    stubs.structs.push_back(std::move(stub));
}

void FileIndexer::index_enum_decl(SyntaxNode* node, FileStubs& stubs) {
    EnumStub stub;
    stub.range = node->range;
    stub.is_pub = has_child(node, SyntaxKind::TokenKwPub);

    SyntaxNode* name_node = find_child(node, SyntaxKind::TokenIdentifier);
    if (name_node) {
        stub.name = name_node->token.text();
        stub.name_range = name_node->range;
    }

    // Extract variants
    for (u32 i = 0; i < node->children.size(); i++) {
        SyntaxNode* child = node->children[i];
        if (child->kind == SyntaxKind::NodeEnumVariant) {
            EnumVariantStub variant;
            variant.range = child->range;

            SyntaxNode* variant_name = find_child(child, SyntaxKind::TokenIdentifier);
            if (variant_name) {
                variant.name = variant_name->token.text();
                variant.name_range = variant_name->range;
            }

            stub.variants.push_back(std::move(variant));
        }
    }

    stubs.enums.push_back(std::move(stub));
}

void FileIndexer::index_trait_decl(SyntaxNode* node, FileStubs& stubs) {
    TraitStub stub;
    stub.range = node->range;
    stub.is_pub = has_child(node, SyntaxKind::TokenKwPub);

    SyntaxNode* name_node = find_child(node, SyntaxKind::TokenIdentifier);
    if (name_node) {
        stub.name = name_node->token.text();
        stub.name_range = name_node->range;
    }

    // Find parent name: identifier after ':'
    bool past_colon = false;
    for (u32 i = 0; i < node->children.size(); i++) {
        if (node->children[i]->kind == SyntaxKind::TokenColon) {
            past_colon = true;
        } else if (past_colon && node->children[i]->kind == SyntaxKind::TokenIdentifier) {
            stub.parent_name = node->children[i]->token.text();
            break;
        }
    }

    stubs.traits.push_back(std::move(stub));
}

void FileIndexer::index_import_decl(SyntaxNode* node, FileStubs& stubs) {
    ImportStub stub;
    stub.range = node->range;
    stub.module_path = StringView("", 0);
    stub.is_from_import = has_child(node, SyntaxKind::TokenKwFrom);

    if (stub.is_from_import) {
        // from pkg.sub import name1, name2;
        // Children: 'from', id, '.', id, 'import', ImportName, ImportName, ';'
        bool past_from = false;
        bool reached_import = false;
        for (u32 i = 0; i < node->children.size(); i++) {
            SyntaxNode* child = node->children[i];
            if (child->kind == SyntaxKind::TokenKwFrom) {
                past_from = true;
            } else if (child->kind == SyntaxKind::TokenKwImport) {
                reached_import = true;
            } else if (past_from && !reached_import && child->kind == SyntaxKind::TokenIdentifier) {
                if (stub.module_path.empty()) {
                    stub.module_path = child->token.text();
                }
            } else if (child->kind == SyntaxKind::NodeImportName) {
                SyntaxNode* import_name = find_child(child, SyntaxKind::TokenIdentifier);
                if (import_name) {
                    stub.imported_names.push_back(import_name->token.text());
                }
            }
        }
    } else {
        // import pkg.sub;
        // Children: 'import', id, '.', id, ';'
        bool past_import = false;
        for (u32 i = 0; i < node->children.size(); i++) {
            SyntaxNode* child = node->children[i];
            if (child->kind == SyntaxKind::TokenKwImport) {
                past_import = true;
            } else if (past_import && child->kind == SyntaxKind::TokenIdentifier) {
                if (stub.module_path.empty()) {
                    stub.module_path = child->token.text();
                }
            }
        }
    }

    stubs.imports.push_back(std::move(stub));
}

} // namespace rx
