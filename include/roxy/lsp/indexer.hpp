#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/string_view.hpp"
#include "roxy/core/vector.hpp"
#include "roxy/lsp/syntax_tree.hpp"

namespace rx {

struct TypeRef {
    StringView name;
    TextRange range;
};

struct ParamStub {
    StringView name;
    TypeRef type;
    TextRange range;
};

struct FieldStub {
    StringView name;
    TypeRef type;
    bool is_pub;
    bool has_default;
    TextRange range;
    TextRange name_range;
};

struct FunctionStub {
    StringView name;
    TextRange range;
    TextRange name_range;
    bool is_pub;
    bool is_native;
    bool has_body;
    Vector<ParamStub> params;
    TypeRef return_type;
};

struct MethodStub {
    StringView struct_name;
    StringView method_name;
    TextRange range;
    TextRange name_range;
    bool is_pub;
    bool is_native;
    bool has_body;
    Vector<ParamStub> params;
    TypeRef return_type;
    StringView trait_name;
};

struct ConstructorStub {
    StringView struct_name;
    StringView constructor_name;
    TextRange range;
    TextRange name_range;
    bool is_pub;
    Vector<ParamStub> params;
};

struct DestructorStub {
    StringView struct_name;
    TextRange range;
    TextRange name_range;
    bool is_pub;
    Vector<ParamStub> params;
};

struct EnumVariantStub {
    StringView name;
    TextRange range;
    TextRange name_range;
};

struct StructStub {
    StringView name;
    TextRange range;
    TextRange name_range;
    bool is_pub;
    StringView parent_name;
    Vector<FieldStub> fields;
};

struct EnumStub {
    StringView name;
    TextRange range;
    TextRange name_range;
    bool is_pub;
    Vector<EnumVariantStub> variants;
};

struct TraitStub {
    StringView name;
    TextRange range;
    TextRange name_range;
    bool is_pub;
    StringView parent_name;
};

struct ImportStub {
    StringView module_path;
    Vector<StringView> imported_names;
    bool is_from_import;
    TextRange range;
};

struct GlobalVarStub {
    StringView name;
    TypeRef type;
    bool has_initializer;
    TextRange range;
    TextRange name_range;
};

struct FileStubs {
    Vector<StructStub> structs;
    Vector<EnumStub> enums;
    Vector<FunctionStub> functions;
    Vector<MethodStub> methods;
    Vector<ConstructorStub> constructors;
    Vector<DestructorStub> destructors;
    Vector<TraitStub> traits;
    Vector<ImportStub> imports;
    Vector<GlobalVarStub> globals;
};

class FileIndexer {
public:
    FileStubs index(SyntaxNode* root);

private:
    void index_var_decl(SyntaxNode* node, FileStubs& stubs);
    void index_fun_decl(SyntaxNode* node, FileStubs& stubs);
    void index_method_decl(SyntaxNode* node, FileStubs& stubs);
    void index_constructor_decl(SyntaxNode* node, FileStubs& stubs);
    void index_destructor_decl(SyntaxNode* node, FileStubs& stubs);
    void index_struct_decl(SyntaxNode* node, FileStubs& stubs);
    void index_enum_decl(SyntaxNode* node, FileStubs& stubs);
    void index_trait_decl(SyntaxNode* node, FileStubs& stubs);
    void index_import_decl(SyntaxNode* node, FileStubs& stubs);

    // Helpers
    SyntaxNode* find_child(SyntaxNode* node, SyntaxKind kind);
    SyntaxNode* find_child_after(SyntaxNode* node, SyntaxKind kind, u32 start_index);
    StringView get_identifier_text(SyntaxNode* node);
    TextRange get_node_range(SyntaxNode* node);
    bool has_child(SyntaxNode* node, SyntaxKind kind);
    Vector<ParamStub> extract_params(SyntaxNode* param_list);
    TypeRef extract_type_ref(SyntaxNode* type_expr_node);
};

} // namespace rx
