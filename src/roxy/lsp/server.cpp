#include "roxy/lsp/server.hpp"
#include "roxy/lsp/lsp_parser.hpp"
#include "roxy/lsp/indexer.hpp"
#include "roxy/lsp/cst_lowering.hpp"
#include "roxy/lsp/lsp_type_resolver.hpp"
#include "roxy/core/bump_allocator.hpp"
#include "roxy/core/format.hpp"
#include "roxy/core/file.hpp"

#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

namespace rx {

void LspServer::run() {
    String message;
    while (m_transport.read_message(message)) {
        // json_parse requires mutable buffer
        dispatch(message.data(), message.size());

        if (m_shutdown_requested) {
            // Wait for 'exit' notification
        }
    }
}

void LspServer::dispatch(char* message_buf, u32 message_length) {
    BumpAllocator json_allocator(4096);
    JsonValue root;
    JsonParseError json_error;

    if (!json_parse(message_buf, message_length, json_allocator, root, &json_error)) {
        // Invalid JSON — send parse error
        m_transport.write_error_response(0, -32700, "Parse error");
        return;
    }

    if (!root.is_object()) {
        m_transport.write_error_response(0, -32600, "Invalid Request");
        return;
    }

    // Extract method
    const JsonValue* method_val = root.find("method");
    if (!method_val || !method_val->is_string()) {
        // If there's no method but there's an id, it might be a response — ignore
        return;
    }
    StringView method = method_val->as_string();

    // Extract id (optional — notifications don't have one)
    i64 request_id = 0;
    bool has_id = false;
    const JsonValue* id_val = root.find("id");
    if (id_val) {
        if (id_val->is_int()) {
            request_id = id_val->as_int();
            has_id = true;
        }
    }

    // Extract params
    const JsonValue* params_val = root.find("params");
    JsonValue params;
    if (params_val) {
        params = *params_val;
    }

    // Dispatch by method name
    if (method == StringView("initialize")) {
        handle_initialize(params, request_id);
    } else if (method == StringView("initialized")) {
        handle_initialized();
    } else if (method == StringView("shutdown")) {
        handle_shutdown(request_id);
    } else if (method == StringView("exit")) {
        handle_exit();
    } else if (method == StringView("textDocument/didOpen")) {
        handle_did_open(params);
    } else if (method == StringView("textDocument/didChange")) {
        handle_did_change(params);
    } else if (method == StringView("textDocument/didClose")) {
        handle_did_close(params);
    } else if (method == StringView("textDocument/documentSymbol")) {
        handle_document_symbol(params, request_id);
    } else if (method == StringView("textDocument/definition")) {
        handle_definition(params, request_id);
    } else if (method == StringView("textDocument/completion")) {
        handle_completion(params, request_id);
    } else {
        // Unknown method
        if (has_id) {
            m_transport.write_error_response(request_id, -32601, "Method not found");
        }
        // Notifications for unknown methods are silently ignored
    }
}

void LspServer::handle_initialize(const JsonValue& params, i64 id) {
    m_initialized = true;

    // Extract workspace root from rootUri or rootPath
    const JsonValue* root_uri = params.find("rootUri");
    if (root_uri && root_uri->is_string()) {
        m_workspace_root = uri_to_file_path(root_uri->as_string());
    } else {
        const JsonValue* root_path = params.find("rootPath");
        if (root_path && root_path->is_string()) {
            m_workspace_root = String(root_path->as_string());
        }
    }

    // Build capabilities response using JsonWriter
    String result;
    JsonWriter writer(result);
    writer.write_start_object();
    {
        writer.write_key("capabilities");
        writer.write_start_object();
        {
            // Full document sync
            writer.write_key("textDocumentSync");
            writer.write_start_object();
            {
                writer.write_key_bool("openClose", true);
                writer.write_key_int("change", 1); // Full sync
            }
            writer.write_end_object();

            writer.write_key_bool("documentSymbolProvider", true);
            writer.write_key_bool("definitionProvider", true);

            writer.write_key("completionProvider");
            writer.write_start_object();
            {
                writer.write_key("triggerCharacters");
                writer.write_start_array();
                writer.write_string(".");
                writer.write_string(":");
                writer.write_end_array();
            }
            writer.write_end_object();
        }
        writer.write_end_object();

        writer.write_key("serverInfo");
        writer.write_start_object();
        {
            writer.write_key_string("name", "roxy-lsp");
            writer.write_key_string("version", "0.2.0");
        }
        writer.write_end_object();
    }
    writer.write_end_object();

    m_transport.write_response(id, StringView(result.data(), result.size()));

    // Scan workspace after responding
    if (!m_workspace_root.empty()) {
        scan_workspace();
    }
}

void LspServer::handle_initialized() {
    // No-op: client confirms initialization
}

void LspServer::handle_shutdown(i64 id) {
    m_shutdown_requested = true;
    m_transport.write_response(id, "null");
}

void LspServer::handle_exit() {
    // Exit with 0 if shutdown was requested, 1 otherwise
    _Exit(m_shutdown_requested ? 0 : 1);
}

void LspServer::handle_did_open(const JsonValue& params) {
    const JsonValue* text_document = params.find("textDocument");
    if (!text_document || !text_document->is_object()) return;

    const JsonValue* uri_val = text_document->find("uri");
    const JsonValue* text_val = text_document->find("text");
    const JsonValue* version_val = text_document->find("version");

    if (!uri_val || !uri_val->is_string()) return;
    if (!text_val || !text_val->is_string()) return;

    OpenDocument doc;
    doc.uri = String(uri_val->as_string());
    doc.content = String(text_val->as_string());
    doc.version = version_val && version_val->is_int() ? static_cast<i32>(version_val->as_int()) : 0;

    m_open_documents.push_back(std::move(doc));

    parse_and_publish_diagnostics(m_open_documents.back());
    m_global_index.update_file(m_open_documents.back().uri, m_open_documents.back().stubs);
}

void LspServer::handle_did_change(const JsonValue& params) {
    const JsonValue* text_document = params.find("textDocument");
    if (!text_document || !text_document->is_object()) return;

    const JsonValue* uri_val = text_document->find("uri");
    if (!uri_val || !uri_val->is_string()) return;

    OpenDocument* doc = find_document(uri_val->as_string());
    if (!doc) return;

    const JsonValue* version_val = text_document->find("version");
    if (version_val && version_val->is_int()) {
        doc->version = static_cast<i32>(version_val->as_int());
    }

    // Full document sync: take the first content change
    const JsonValue* content_changes = params.find("contentChanges");
    if (!content_changes || !content_changes->is_array()) return;

    Span<JsonValue> changes = content_changes->as_array();
    if (changes.size() == 0) return;

    const JsonValue* text_val = changes[0].find("text");
    if (!text_val || !text_val->is_string()) return;

    doc->content = String(text_val->as_string());

    parse_and_publish_diagnostics(*doc);
    m_global_index.update_file(doc->uri, doc->stubs);
}

void LspServer::handle_did_close(const JsonValue& params) {
    const JsonValue* text_document = params.find("textDocument");
    if (!text_document || !text_document->is_object()) return;

    const JsonValue* uri_val = text_document->find("uri");
    if (!uri_val || !uri_val->is_string()) return;

    StringView uri = uri_val->as_string();

    // Remove from open documents
    for (u32 i = 0; i < m_open_documents.size(); i++) {
        if (StringView(m_open_documents[i].uri.data(), m_open_documents[i].uri.size()) == uri) {
            // Swap with last and pop
            if (i + 1 < m_open_documents.size()) {
                m_open_documents[i] = std::move(m_open_documents.back());
            }
            m_open_documents.pop_back();
            break;
        }
    }

    // Clear diagnostics for closed document
    Vector<LspDiagnostic> empty;
    publish_diagnostics(uri, empty);

    // If file is in workspace, re-read from disk and re-index
    String file_path = uri_to_file_path(uri);
    String uri_str(uri);
    auto workspace_it = m_workspace_files.find(uri_str);
    if (workspace_it != m_workspace_files.end()) {
        // Re-read from disk
        Vector<u8> file_buf;
        if (read_file_to_buf(file_path.c_str(), file_buf)) {
            workspace_it.value().content = String(reinterpret_cast<const char*>(file_buf.data()),
                                                  file_buf.size() > 0 ? static_cast<u32>(file_buf.size() - 1) : 0);

            // Re-parse and re-index
            BumpAllocator allocator(8192);
            Lexer lexer(workspace_it.value().content.data(), workspace_it.value().content.size());
            LspParser parser(lexer, allocator);
            SyntaxTree tree = parser.parse();

            FileIndexer indexer;
            workspace_it.value().stubs = indexer.index(tree.root);
            m_global_index.update_file(uri_str, workspace_it.value().stubs);
        }
    }
}

void LspServer::parse_and_publish_diagnostics(OpenDocument& doc) {
    // Create a fresh allocator for this parse
    BumpAllocator allocator(8192);

    Lexer lexer(doc.content.data(), doc.content.size());
    LspParser parser(lexer, allocator);
    SyntaxTree tree = parser.parse();

    // Index the CST for document symbols
    FileIndexer indexer;
    doc.stubs = indexer.index(tree.root);

    // Convert ParseDiagnostics to LspDiagnostics
    Vector<LspDiagnostic> lsp_diagnostics;
    for (u32 i = 0; i < tree.diagnostics.size(); i++) {
        const ParseDiagnostic& parse_diag = tree.diagnostics[i];

        LspDiagnostic lsp_diag;
        lsp_diag.range = text_range_to_lsp_range(
            tree.source, tree.source_length, parse_diag.range);
        lsp_diag.severity = DiagnosticSeverity::Error;
        lsp_diag.message = parse_diag.message;

        lsp_diagnostics.push_back(std::move(lsp_diag));
    }

    publish_diagnostics(
        StringView(doc.uri.data(), doc.uri.size()),
        lsp_diagnostics);
}

void LspServer::publish_diagnostics(StringView uri, const Vector<LspDiagnostic>& diagnostics) {
    String params_json;
    JsonWriter writer(params_json);

    writer.write_start_object();
    {
        writer.write_key_string("uri", uri);

        writer.write_key("diagnostics");
        writer.write_start_array();
        for (u32 i = 0; i < diagnostics.size(); i++) {
            const LspDiagnostic& diag = diagnostics[i];
            writer.write_start_object();
            {
                writer.write_key("range");
                writer.write_start_object();
                {
                    writer.write_key("start");
                    writer.write_start_object();
                    writer.write_key_int("line", diag.range.start.line);
                    writer.write_key_int("character", diag.range.start.character);
                    writer.write_end_object();

                    writer.write_key("end");
                    writer.write_start_object();
                    writer.write_key_int("line", diag.range.end.line);
                    writer.write_key_int("character", diag.range.end.character);
                    writer.write_end_object();
                }
                writer.write_end_object();

                writer.write_key_int("severity", static_cast<i64>(diag.severity));
                writer.write_key_string("source", "roxy");
                writer.write_key_string("message",
                    StringView(diag.message.data(), diag.message.size()));
            }
            writer.write_end_object();
        }
        writer.write_end_array();
    }
    writer.write_end_object();

    m_transport.write_notification(
        "textDocument/publishDiagnostics",
        StringView(params_json.data(), params_json.size()));
}

// --- LSP SymbolKind constants ---
static constexpr i64 SymbolKindModule = 2;
static constexpr i64 SymbolKindMethod = 6;
static constexpr i64 SymbolKindField = 8;
static constexpr i64 SymbolKindConstructor = 9;
static constexpr i64 SymbolKindEnum = 10;
static constexpr i64 SymbolKindInterface = 11;
static constexpr i64 SymbolKindFunction = 12;
static constexpr i64 SymbolKindVariable = 13;
static constexpr i64 SymbolKindEnumMember = 22;
static constexpr i64 SymbolKindStruct = 23;

static void write_lsp_range(JsonWriter& writer, LspRange range) {
    writer.write_start_object();
    {
        writer.write_key("start");
        writer.write_start_object();
        writer.write_key_int("line", range.start.line);
        writer.write_key_int("character", range.start.character);
        writer.write_end_object();

        writer.write_key("end");
        writer.write_start_object();
        writer.write_key_int("line", range.end.line);
        writer.write_key_int("character", range.end.character);
        writer.write_end_object();
    }
    writer.write_end_object();
}

static void write_symbol(JsonWriter& writer, StringView name, i64 kind,
                          LspRange range, LspRange selection_range) {
    writer.write_start_object();
    writer.write_key_string("name", name);
    writer.write_key_int("kind", kind);

    writer.write_key("range");
    write_lsp_range(writer, range);

    writer.write_key("selectionRange");
    write_lsp_range(writer, selection_range);
}

void LspServer::handle_document_symbol(const JsonValue& params, i64 id) {
    const JsonValue* text_document = params.find("textDocument");
    if (!text_document || !text_document->is_object()) {
        m_transport.write_error_response(id, -32602, "Invalid params");
        return;
    }

    const JsonValue* uri_val = text_document->find("uri");
    if (!uri_val || !uri_val->is_string()) {
        m_transport.write_error_response(id, -32602, "Missing URI");
        return;
    }

    OpenDocument* doc = find_document(uri_val->as_string());
    if (!doc) {
        m_transport.write_response(id, "[]");
        return;
    }

    const char* source = doc->content.data();
    u32 source_length = doc->content.size();
    const FileStubs& stubs = doc->stubs;

    String result;
    JsonWriter writer(result);
    writer.write_start_array();

    // Functions
    for (u32 i = 0; i < stubs.functions.size(); i++) {
        const FunctionStub& func = stubs.functions[i];
        write_symbol(writer, func.name, SymbolKindFunction,
            text_range_to_lsp_range(source, source_length, func.range),
            text_range_to_lsp_range(source, source_length, func.name_range));
        writer.write_end_object();
    }

    // Methods
    for (u32 i = 0; i < stubs.methods.size(); i++) {
        const MethodStub& method = stubs.methods[i];
        write_symbol(writer, method.method_name, SymbolKindMethod,
            text_range_to_lsp_range(source, source_length, method.range),
            text_range_to_lsp_range(source, source_length, method.name_range));
        writer.write_end_object();
    }

    // Constructors
    for (u32 i = 0; i < stubs.constructors.size(); i++) {
        const ConstructorStub& ctor = stubs.constructors[i];
        StringView display_name = ctor.constructor_name.empty() ? ctor.struct_name : ctor.constructor_name;
        write_symbol(writer, display_name, SymbolKindConstructor,
            text_range_to_lsp_range(source, source_length, ctor.range),
            text_range_to_lsp_range(source, source_length, ctor.name_range));
        writer.write_end_object();
    }

    // Destructors
    for (u32 i = 0; i < stubs.destructors.size(); i++) {
        const DestructorStub& dtor = stubs.destructors[i];
        write_symbol(writer, dtor.struct_name, SymbolKindMethod,
            text_range_to_lsp_range(source, source_length, dtor.range),
            text_range_to_lsp_range(source, source_length, dtor.name_range));
        writer.write_end_object();
    }

    // Structs (with field children)
    for (u32 i = 0; i < stubs.structs.size(); i++) {
        const StructStub& s = stubs.structs[i];
        write_symbol(writer, s.name, SymbolKindStruct,
            text_range_to_lsp_range(source, source_length, s.range),
            text_range_to_lsp_range(source, source_length, s.name_range));

        // Children: fields
        writer.write_key("children");
        writer.write_start_array();
        for (u32 j = 0; j < s.fields.size(); j++) {
            const FieldStub& field = s.fields[j];
            write_symbol(writer, field.name, SymbolKindField,
                text_range_to_lsp_range(source, source_length, field.range),
                text_range_to_lsp_range(source, source_length, field.name_range));
            writer.write_end_object();
        }
        writer.write_end_array();

        writer.write_end_object();
    }

    // Enums (with variant children)
    for (u32 i = 0; i < stubs.enums.size(); i++) {
        const EnumStub& e = stubs.enums[i];
        write_symbol(writer, e.name, SymbolKindEnum,
            text_range_to_lsp_range(source, source_length, e.range),
            text_range_to_lsp_range(source, source_length, e.name_range));

        // Children: variants
        writer.write_key("children");
        writer.write_start_array();
        for (u32 j = 0; j < e.variants.size(); j++) {
            const EnumVariantStub& variant = e.variants[j];
            write_symbol(writer, variant.name, SymbolKindEnumMember,
                text_range_to_lsp_range(source, source_length, variant.range),
                text_range_to_lsp_range(source, source_length, variant.name_range));
            writer.write_end_object();
        }
        writer.write_end_array();

        writer.write_end_object();
    }

    // Traits
    for (u32 i = 0; i < stubs.traits.size(); i++) {
        const TraitStub& trait = stubs.traits[i];
        write_symbol(writer, trait.name, SymbolKindInterface,
            text_range_to_lsp_range(source, source_length, trait.range),
            text_range_to_lsp_range(source, source_length, trait.name_range));
        writer.write_end_object();
    }

    // Global variables
    for (u32 i = 0; i < stubs.globals.size(); i++) {
        const GlobalVarStub& global = stubs.globals[i];
        write_symbol(writer, global.name, SymbolKindVariable,
            text_range_to_lsp_range(source, source_length, global.range),
            text_range_to_lsp_range(source, source_length, global.name_range));
        writer.write_end_object();
    }

    // Imports
    for (u32 i = 0; i < stubs.imports.size(); i++) {
        const ImportStub& imp = stubs.imports[i];
        write_symbol(writer, imp.module_path, SymbolKindModule,
            text_range_to_lsp_range(source, source_length, imp.range),
            text_range_to_lsp_range(source, source_length, imp.range));
        writer.write_end_object();
    }

    writer.write_end_array();

    m_transport.write_response(id, StringView(result.data(), result.size()));
}

// Walk up parent chain to find nearest function/method/constructor/destructor
static SyntaxNode* find_enclosing_function(SyntaxNode* node) {
    SyntaxNode* current = node ? node->parent : nullptr;
    while (current) {
        if (current->kind == SyntaxKind::NodeFunDecl ||
            current->kind == SyntaxKind::NodeMethodDecl ||
            current->kind == SyntaxKind::NodeConstructorDecl ||
            current->kind == SyntaxKind::NodeDestructorDecl) {
            return current;
        }
        current = current->parent;
    }
    return nullptr;
}

// Helper to write a single Location response from a SymbolLocation
static bool write_location_response(LspTransport& transport, i64 id,
                                      const SymbolLocation& loc,
                                      const char* target_source, u32 target_length) {
    LspRange target_range = text_range_to_lsp_range(
        target_source, target_length, loc.name_range);

    String result;
    JsonWriter writer(result);
    writer.write_start_object();
    writer.write_key_string("uri", StringView(loc.uri.data(), loc.uri.size()));
    writer.write_key("range");
    write_lsp_range(writer, target_range);
    writer.write_end_object();

    transport.write_response(id, StringView(result.data(), result.size()));
    return true;
}

void LspServer::handle_definition(const JsonValue& params, i64 id) {
    const JsonValue* text_document = params.find("textDocument");
    if (!text_document || !text_document->is_object()) {
        m_transport.write_response(id, "null");
        return;
    }

    const JsonValue* uri_val = text_document->find("uri");
    if (!uri_val || !uri_val->is_string()) {
        m_transport.write_response(id, "null");
        return;
    }

    const JsonValue* position_val = params.find("position");
    if (!position_val || !position_val->is_object()) {
        m_transport.write_response(id, "null");
        return;
    }

    StringView uri = uri_val->as_string();

    // Find the document
    OpenDocument* doc = find_document(uri);
    if (!doc) {
        m_transport.write_response(id, "null");
        return;
    }

    // Extract position
    const JsonValue* line_val = position_val->find("line");
    const JsonValue* char_val = position_val->find("character");
    if (!line_val || !line_val->is_int() || !char_val || !char_val->is_int()) {
        m_transport.write_response(id, "null");
        return;
    }

    LspPosition cursor_pos;
    cursor_pos.line = static_cast<u32>(line_val->as_int());
    cursor_pos.character = static_cast<u32>(char_val->as_int());

    // Convert to byte offset
    u32 byte_offset = lsp_position_to_offset(
        doc->content.data(), doc->content.size(), cursor_pos);

    // Re-parse to get fresh CST
    BumpAllocator allocator(8192);
    Lexer lexer(doc->content.data(), doc->content.size());
    LspParser parser(lexer, allocator);
    SyntaxTree tree = parser.parse();

    // Find deepest node at cursor
    SyntaxNode* node = find_node_at_offset(tree.root, byte_offset);
    if (!node) {
        m_transport.write_response(id, "null");
        return;
    }

    // Handle TokenKwSelf — resolve to struct definition
    if (node->kind == SyntaxKind::TokenKwSelf) {
        SyntaxNode* enclosing_fn = find_enclosing_function(node);
        if (enclosing_fn) {
            // Get struct name from enclosing method/constructor/destructor
            SyntaxNode* struct_ident = nullptr;
            for (u32 i = 0; i < enclosing_fn->children.size(); i++) {
                if (enclosing_fn->children[i]->kind == SyntaxKind::TokenIdentifier) {
                    struct_ident = enclosing_fn->children[i];
                    break;
                }
            }
            if (struct_ident) {
                const SymbolLocation* struct_loc = m_global_index.find_struct(struct_ident->token.text());
                if (struct_loc) {
                    u32 target_length = 0;
                    const char* target_source = get_file_content(
                        StringView(struct_loc->uri.data(), struct_loc->uri.size()), target_length);
                    if (target_source) {
                        write_location_response(m_transport, id, *struct_loc, target_source, target_length);
                        return;
                    }
                }
            }
        }
        m_transport.write_response(id, "null");
        return;
    }

    if (node->kind != SyntaxKind::TokenIdentifier) {
        m_transport.write_response(id, "null");
        return;
    }

    // Extract the identifier text
    StringView identifier = node->token.text();

    // Determine context by walking up parent chain
    const SymbolLocation* match = nullptr;
    Vector<SymbolLocation> matches;

    SyntaxNode* parent = node->parent;
    if (parent) {
        if (parent->kind == SyntaxKind::NodeGetExpr) {
            // Field/method access: check if this node is the member-name child
            // NodeGetExpr children: [object, '.', member_name]
            bool is_member_name = false;
            if (parent->children.size() >= 3) {
                is_member_name = (parent->children[parent->children.size() - 1] == node);
            }

            if (is_member_name) {
                // Try to resolve the object expression type
                SyntaxNode* object_expr = parent->children[0];
                String receiver_type;

                SyntaxNode* enclosing_fn = find_enclosing_function(parent);
                if (enclosing_fn) {
                    BumpAllocator ast_allocator(8192);
                    CstLowering lowering(ast_allocator);
                    Decl* ast_decl = lowering.lower_decl(enclosing_fn);

                    LspTypeResolver resolver(m_global_index);
                    resolver.analyze_function(ast_decl);

                    receiver_type = resolver.resolve_cst_expr_type(object_expr);
                }

                if (!receiver_type.empty()) {
                    // Walk inheritance chain to find field or method
                    StringView current_type(receiver_type.data(), receiver_type.size());
                    u32 depth = 0;
                    while (!current_type.empty() && depth < 16) {
                        match = m_global_index.find_field(current_type, identifier);
                        if (match) break;
                        match = m_global_index.find_method(current_type, identifier);
                        if (match) break;
                        current_type = m_global_index.find_struct_parent(current_type);
                        depth++;
                    }
                }

                // Fallback to find_any if type resolution failed
                if (!match) {
                    matches = m_global_index.find_any(identifier);
                }
            } else {
                // Cursor is on the receiver expression (not the member name)
                matches = m_global_index.find_any(identifier);
            }
        } else if (parent->kind == SyntaxKind::NodeTypeExpr) {
            // Type reference — search structs, enums, traits
            match = m_global_index.find_struct(identifier);
            if (!match) match = m_global_index.find_enum(identifier);
            if (!match) match = m_global_index.find_trait(identifier);
        } else if (parent->kind == SyntaxKind::NodeCallExpr) {
            // Function call — search functions first, then constructors
            match = m_global_index.find_function(identifier);
            if (!match) {
                // Could be a constructor call (e.g., Point(...))
                match = m_global_index.find_struct(identifier);
            }
        } else if (parent->kind == SyntaxKind::NodeStaticGetExpr) {
            // Enum::Variant or static access
            matches = m_global_index.find_any(identifier);
        } else {
            // Default — search all categories
            matches = m_global_index.find_any(identifier);
        }
    } else {
        matches = m_global_index.find_any(identifier);
    }

    // Build response
    if (match) {
        u32 target_length = 0;
        const char* target_source = get_file_content(
            StringView(match->uri.data(), match->uri.size()), target_length);

        if (!target_source) {
            m_transport.write_response(id, "null");
            return;
        }

        write_location_response(m_transport, id, *match, target_source, target_length);
    } else if (!matches.empty()) {
        if (matches.size() == 1) {
            const SymbolLocation& loc = matches[0];
            u32 target_length = 0;
            const char* target_source = get_file_content(
                StringView(loc.uri.data(), loc.uri.size()), target_length);

            if (!target_source) {
                m_transport.write_response(id, "null");
                return;
            }

            write_location_response(m_transport, id, loc, target_source, target_length);
        } else {
            // Multiple matches — return Location[]
            String result;
            JsonWriter writer(result);
            writer.write_start_array();

            for (u32 i = 0; i < matches.size(); i++) {
                const SymbolLocation& loc = matches[i];
                u32 target_length = 0;
                const char* target_source = get_file_content(
                    StringView(loc.uri.data(), loc.uri.size()), target_length);

                if (!target_source) continue;

                LspRange target_range = text_range_to_lsp_range(
                    target_source, target_length, loc.name_range);

                writer.write_start_object();
                writer.write_key_string("uri", StringView(loc.uri.data(), loc.uri.size()));
                writer.write_key("range");
                write_lsp_range(writer, target_range);
                writer.write_end_object();
            }

            writer.write_end_array();
            m_transport.write_response(id, StringView(result.data(), result.size()));
        }
    } else {
        m_transport.write_response(id, "null");
    }
}

// --- Completion support ---

enum class CompletionContext { DotAccess, StaticAccess, TypeAnnotation, BareIdentifier, None };

// Detect completion context from source text and cursor position
static CompletionContext detect_completion_context(const char* source, u32 source_length,
                                                    u32 byte_offset, SyntaxNode* root) {
    if (byte_offset == 0) return CompletionContext::BareIdentifier;

    // Check character(s) immediately before cursor
    char prev_char = source[byte_offset - 1];

    if (prev_char == '.') {
        return CompletionContext::DotAccess;
    }

    if (prev_char == ':' && byte_offset >= 2 && source[byte_offset - 2] == ':') {
        return CompletionContext::StaticAccess;
    }

    // Check if we're in a type annotation position:
    // Walk backwards skipping whitespace to find ':'
    u32 scan = byte_offset;
    while (scan > 0 && (source[scan - 1] == ' ' || source[scan - 1] == '\t')) {
        scan--;
    }

    // Check if preceding non-whitespace context suggests type annotation
    // Case 1: cursor right after ':' (e.g., "var p: |")
    if (scan > 0 && source[scan - 1] == ':') {
        // Make sure it's not '::'
        if (scan < 2 || source[scan - 2] != ':') {
            return CompletionContext::TypeAnnotation;
        }
    }

    // Case 2: partially typed name after ':' (e.g., "var p: Po|")
    // Walk backwards over identifier chars, then check for ':'
    u32 ident_end = scan;
    while (scan > 0 && (
        (source[scan - 1] >= 'a' && source[scan - 1] <= 'z') ||
        (source[scan - 1] >= 'A' && source[scan - 1] <= 'Z') ||
        (source[scan - 1] >= '0' && source[scan - 1] <= '9') ||
        source[scan - 1] == '_')) {
        scan--;
    }
    if (scan < ident_end && scan > 0) {
        // Skip whitespace before the identifier
        u32 pre_ident = scan;
        while (pre_ident > 0 && (source[pre_ident - 1] == ' ' || source[pre_ident - 1] == '\t')) {
            pre_ident--;
        }
        if (pre_ident > 0 && source[pre_ident - 1] == ':') {
            if (pre_ident < 2 || source[pre_ident - 2] != ':') {
                return CompletionContext::TypeAnnotation;
            }
        }
    }

    // Also check if cursor is right after '.' + partial identifier (e.g., "p.le|")
    // Walk back over identifier chars, then check for '.'
    scan = byte_offset;
    while (scan > 0 && (
        (source[scan - 1] >= 'a' && source[scan - 1] <= 'z') ||
        (source[scan - 1] >= 'A' && source[scan - 1] <= 'Z') ||
        (source[scan - 1] >= '0' && source[scan - 1] <= '9') ||
        source[scan - 1] == '_')) {
        scan--;
    }
    if (scan > 0 && source[scan - 1] == '.') {
        return CompletionContext::DotAccess;
    }
    // Check for '::' + partial identifier (e.g., "Color::Re|")
    if (scan >= 2 && source[scan - 1] == ':' && source[scan - 2] == ':') {
        return CompletionContext::StaticAccess;
    }

    return CompletionContext::BareIdentifier;
}

// Find the receiver text for dot-access completion
// Walks backwards from the '.' to extract the receiver identifier
static StringView find_dot_receiver(const char* source, u32 source_length, u32 byte_offset) {
    // byte_offset should be at or after the '.'
    // Walk backwards to find the '.'
    u32 pos = byte_offset;
    // Skip any partial identifier after '.'
    while (pos > 0 && source[pos - 1] != '.') {
        pos--;
    }
    if (pos == 0) return StringView();
    // Now pos-1 is '.', walk backwards over whitespace
    u32 dot_pos = pos - 1;
    u32 end = dot_pos;
    // Walk backwards over identifier characters to get receiver name
    while (end > 0 && (
        (source[end - 1] >= 'a' && source[end - 1] <= 'z') ||
        (source[end - 1] >= 'A' && source[end - 1] <= 'Z') ||
        (source[end - 1] >= '0' && source[end - 1] <= '9') ||
        source[end - 1] == '_')) {
        end--;
    }
    if (end == dot_pos) return StringView();
    return StringView(source + end, dot_pos - end);
}

// Find the type name before '::' for static-access completion
static StringView find_static_receiver(const char* source, u32 source_length, u32 byte_offset) {
    u32 pos = byte_offset;
    // Skip any partial identifier after '::'
    while (pos > 0 && source[pos - 1] != ':') {
        pos--;
    }
    // pos-1 should be second ':', check for first ':'
    if (pos < 2 || source[pos - 2] != ':') return StringView();
    u32 colon_pos = pos - 2;
    // Walk backwards over identifier characters to get type name
    u32 end = colon_pos;
    while (end > 0 && (
        (source[end - 1] >= 'a' && source[end - 1] <= 'z') ||
        (source[end - 1] >= 'A' && source[end - 1] <= 'Z') ||
        (source[end - 1] >= '0' && source[end - 1] <= '9') ||
        source[end - 1] == '_')) {
        end--;
    }
    if (end == colon_pos) return StringView();
    return StringView(source + end, colon_pos - end);
}

// Write a single CompletionItem as JSON
static void write_completion_item(JsonWriter& writer, StringView label, i64 kind,
                                   StringView detail = StringView()) {
    writer.write_start_object();
    writer.write_key_string("label", label);
    writer.write_key_int("kind", kind);
    if (!detail.empty()) {
        writer.write_key_string("detail", detail);
    }
    writer.write_end_object();
}

void LspServer::handle_completion(const JsonValue& params, i64 id) {
    const JsonValue* text_document = params.find("textDocument");
    if (!text_document || !text_document->is_object()) {
        m_transport.write_response(id, "{\"isIncomplete\":false,\"items\":[]}");
        return;
    }

    const JsonValue* uri_val = text_document->find("uri");
    if (!uri_val || !uri_val->is_string()) {
        m_transport.write_response(id, "{\"isIncomplete\":false,\"items\":[]}");
        return;
    }

    const JsonValue* position_val = params.find("position");
    if (!position_val || !position_val->is_object()) {
        m_transport.write_response(id, "{\"isIncomplete\":false,\"items\":[]}");
        return;
    }

    StringView uri = uri_val->as_string();
    OpenDocument* doc = find_document(uri);
    if (!doc) {
        m_transport.write_response(id, "{\"isIncomplete\":false,\"items\":[]}");
        return;
    }

    const JsonValue* line_val = position_val->find("line");
    const JsonValue* char_val = position_val->find("character");
    if (!line_val || !line_val->is_int() || !char_val || !char_val->is_int()) {
        m_transport.write_response(id, "{\"isIncomplete\":false,\"items\":[]}");
        return;
    }

    LspPosition cursor_pos;
    cursor_pos.line = static_cast<u32>(line_val->as_int());
    cursor_pos.character = static_cast<u32>(char_val->as_int());

    u32 byte_offset = lsp_position_to_offset(
        doc->content.data(), doc->content.size(), cursor_pos);

    // Re-parse to get fresh CST
    BumpAllocator allocator(8192);
    Lexer lexer(doc->content.data(), doc->content.size());
    LspParser parser(lexer, allocator);
    SyntaxTree tree = parser.parse();

    CompletionContext context = detect_completion_context(
        doc->content.data(), doc->content.size(), byte_offset, tree.root);

    String result;
    JsonWriter writer(result);
    writer.write_start_object();
    writer.write_key_bool("isIncomplete", false);
    writer.write_key("items");
    writer.write_start_array();

    if (context == CompletionContext::DotAccess) {
        // Resolve receiver type
        StringView receiver_ident = find_dot_receiver(
            doc->content.data(), doc->content.size(), byte_offset);

        String receiver_type;
        if (!receiver_ident.empty()) {
            // Try to resolve receiver type via CstLowering + LspTypeResolver
            SyntaxNode* cursor_node = find_node_at_offset(tree.root, byte_offset > 0 ? byte_offset - 1 : 0);
            SyntaxNode* enclosing_fn = cursor_node ? find_enclosing_function(cursor_node) : nullptr;

            if (enclosing_fn) {
                BumpAllocator ast_allocator(8192);
                CstLowering lowering(ast_allocator);
                Decl* ast_decl = lowering.lower_decl(enclosing_fn);

                LspTypeResolver resolver(m_global_index);
                resolver.analyze_function(ast_decl);

                // Check if receiver is "self"
                if (receiver_ident == StringView("self")) {
                    receiver_type = resolver.self_type();
                } else {
                    // Look up variable type
                    auto var_it = resolver.var_types().find(String(receiver_ident));
                    if (var_it != resolver.var_types().end()) {
                        receiver_type = var_it->second;
                    }
                }
            }
        }

        if (!receiver_type.empty()) {
            // Walk inheritance chain collecting fields and methods
            StringView current_type(receiver_type.data(), receiver_type.size());
            u32 depth = 0;
            while (!current_type.empty() && depth < 16) {
                // Fields
                const Vector<String>* fields = m_global_index.get_struct_fields(current_type);
                if (fields) {
                    for (u32 i = 0; i < fields->size(); i++) {
                        StringView field_name((*fields)[i].data(), (*fields)[i].size());
                        StringView field_type = m_global_index.find_field_type(current_type, field_name);
                        write_completion_item(writer, field_name, CompletionItemKind::Field, field_type);
                    }
                }

                // Methods
                const Vector<String>* methods = m_global_index.get_struct_methods(current_type);
                if (methods) {
                    for (u32 i = 0; i < methods->size(); i++) {
                        StringView method_name((*methods)[i].data(), (*methods)[i].size());
                        StringView signature = m_global_index.find_method_signature(current_type, method_name);
                        write_completion_item(writer, method_name, CompletionItemKind::Method, signature);
                    }
                }

                current_type = m_global_index.find_struct_parent(current_type);
                depth++;
            }
        }
    } else if (context == CompletionContext::StaticAccess) {
        // Enum variant completion
        StringView type_name = find_static_receiver(
            doc->content.data(), doc->content.size(), byte_offset);

        if (!type_name.empty()) {
            const Vector<String>* variants = m_global_index.get_enum_variants(type_name);
            if (variants) {
                for (u32 i = 0; i < variants->size(); i++) {
                    StringView variant_name((*variants)[i].data(), (*variants)[i].size());
                    write_completion_item(writer, variant_name, CompletionItemKind::EnumMember);
                }
            }
        }
    } else if (context == CompletionContext::TypeAnnotation) {
        // Primitive types
        static const char* primitive_types[] = {
            "i32", "i64", "f32", "f64", "bool", "string", "void",
            "u8", "u16", "u32", "u64", "i8", "i16"
        };
        for (u32 i = 0; i < sizeof(primitive_types) / sizeof(primitive_types[0]); i++) {
            write_completion_item(writer, StringView(primitive_types[i]),
                                  CompletionItemKind::Keyword);
        }

        // All struct names
        m_global_index.for_each_struct([&](const String& name) {
            write_completion_item(writer, StringView(name.data(), name.size()),
                                  CompletionItemKind::Struct);
        });

        // All enum names
        m_global_index.for_each_enum([&](const String& name) {
            write_completion_item(writer, StringView(name.data(), name.size()),
                                  CompletionItemKind::Enum);
        });

        // All trait names
        m_global_index.for_each_trait([&](const String& name) {
            write_completion_item(writer, StringView(name.data(), name.size()),
                                  CompletionItemKind::Interface);
        });

        // Reference keywords
        write_completion_item(writer, StringView("uniq"), CompletionItemKind::Keyword);
        write_completion_item(writer, StringView("ref"), CompletionItemKind::Keyword);
        write_completion_item(writer, StringView("weak"), CompletionItemKind::Keyword);
        write_completion_item(writer, StringView("List"), CompletionItemKind::Struct);
        write_completion_item(writer, StringView("Map"), CompletionItemKind::Struct);
    } else if (context == CompletionContext::BareIdentifier) {
        // Local variables from resolver
        SyntaxNode* cursor_node = find_node_at_offset(tree.root, byte_offset > 0 ? byte_offset - 1 : 0);
        SyntaxNode* enclosing_fn = cursor_node ? find_enclosing_function(cursor_node) : nullptr;

        if (enclosing_fn) {
            BumpAllocator ast_allocator(8192);
            CstLowering lowering(ast_allocator);
            Decl* ast_decl = lowering.lower_decl(enclosing_fn);

            LspTypeResolver resolver(m_global_index);
            resolver.analyze_function(ast_decl);

            // Emit locals
            for (auto it = resolver.var_types().begin(); it != resolver.var_types().end(); ++it) {
                StringView var_name(it->first.data(), it->first.size());
                StringView var_type(it->second.data(), it->second.size());
                write_completion_item(writer, var_name, CompletionItemKind::Variable, var_type);
            }

            // Emit self if in method
            if (!resolver.self_type().empty()) {
                StringView self_type(resolver.self_type().data(), resolver.self_type().size());
                write_completion_item(writer, StringView("self"), CompletionItemKind::Variable, self_type);
            }
        }

        // All functions
        m_global_index.for_each_function([&](const String& name) {
            StringView func_name(name.data(), name.size());
            StringView signature = m_global_index.find_function_signature(func_name);
            write_completion_item(writer, func_name, CompletionItemKind::Function, signature);
        });

        // All globals
        m_global_index.for_each_global([&](const String& name) {
            write_completion_item(writer, StringView(name.data(), name.size()),
                                  CompletionItemKind::Variable);
        });

        // Struct and enum names (usable as constructors / type names)
        m_global_index.for_each_struct([&](const String& name) {
            write_completion_item(writer, StringView(name.data(), name.size()),
                                  CompletionItemKind::Struct);
        });
        m_global_index.for_each_enum([&](const String& name) {
            write_completion_item(writer, StringView(name.data(), name.size()),
                                  CompletionItemKind::Enum);
        });

        // Control flow and declaration keywords
        static const char* keywords[] = {
            "var", "fun", "struct", "enum", "trait", "if", "else",
            "for", "while", "break", "continue", "return",
            "when", "case", "true", "false", "nil",
            "pub", "native", "import", "from",
            "try", "catch", "throw", "finally",
            "new", "delete", "self", "super",
            "uniq", "ref", "weak"
        };
        for (u32 i = 0; i < sizeof(keywords) / sizeof(keywords[0]); i++) {
            write_completion_item(writer, StringView(keywords[i]),
                                  CompletionItemKind::Keyword);
        }
    }

    writer.write_end_array();
    writer.write_end_object();

    m_transport.write_response(id, StringView(result.data(), result.size()));
}

// --- Workspace helpers ---

void LspServer::scan_workspace() {
    if (m_workspace_root.empty()) return;

    // Recursive directory walk collecting .roxy files
    Vector<String> directories;
    directories.push_back(m_workspace_root);

    while (!directories.empty()) {
        String dir_path = std::move(directories.back());
        directories.pop_back();

#ifdef _WIN32
        String search_path = dir_path;
        search_path.append("\\*", 2);

        WIN32_FIND_DATAA find_data;
        HANDLE find_handle = FindFirstFileA(search_path.c_str(), &find_data);
        if (find_handle == INVALID_HANDLE_VALUE) continue;

        do {
            if (find_data.cFileName[0] == '.') continue;

            String full_path = dir_path;
            full_path.push_back('\\');
            full_path.append(find_data.cFileName, static_cast<u32>(strlen(find_data.cFileName)));

            if (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                directories.push_back(std::move(full_path));
            } else {
                u32 name_len = static_cast<u32>(strlen(find_data.cFileName));
                if (name_len > 5 && memcmp(find_data.cFileName + name_len - 5, ".roxy", 5) == 0) {
                    index_workspace_file(full_path);
                }
            }
        } while (FindNextFileA(find_handle, &find_data));

        FindClose(find_handle);
#else
        DIR* dir = opendir(dir_path.c_str());
        if (!dir) continue;

        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            if (entry->d_name[0] == '.') continue;

            String full_path = dir_path;
            full_path.push_back('/');
            full_path.append(entry->d_name, static_cast<u32>(strlen(entry->d_name)));

            struct stat st;
            if (stat(full_path.c_str(), &st) != 0) continue;

            if (S_ISDIR(st.st_mode)) {
                directories.push_back(std::move(full_path));
            } else if (S_ISREG(st.st_mode)) {
                u32 name_len = static_cast<u32>(strlen(entry->d_name));
                if (name_len > 5 && memcmp(entry->d_name + name_len - 5, ".roxy", 5) == 0) {
                    index_workspace_file(full_path);
                }
            }
        }

        closedir(dir);
#endif
    }
}

void LspServer::index_workspace_file(const String& file_path) {
    Vector<u8> file_buf;
    if (!read_file_to_buf(file_path.c_str(), file_buf)) return;

    String uri = file_path_to_uri(StringView(file_path.data(), file_path.size()));
    u32 content_length = file_buf.size() > 0 ? static_cast<u32>(file_buf.size() - 1) : 0;

    WorkspaceFile workspace_file;
    workspace_file.uri = uri;
    workspace_file.file_path = file_path;
    workspace_file.content = String(reinterpret_cast<const char*>(file_buf.data()), content_length);

    // Parse and index
    BumpAllocator allocator(8192);
    Lexer lexer(workspace_file.content.data(), workspace_file.content.size());
    LspParser parser(lexer, allocator);
    SyntaxTree tree = parser.parse();

    FileIndexer indexer;
    workspace_file.stubs = indexer.index(tree.root);

    m_global_index.update_file(workspace_file.uri, workspace_file.stubs);
    m_workspace_files[uri] = std::move(workspace_file);
}

String LspServer::file_path_to_uri(StringView path) {
    String result("file://");
    result.append(path.data(), path.size());
    return result;
}

String LspServer::uri_to_file_path(StringView uri) {
    // Strip "file://" prefix
    if (uri.size() > 7 && memcmp(uri.data(), "file://", 7) == 0) {
        return String(StringView(uri.data() + 7, uri.size() - 7));
    }
    return String(uri);
}

const char* LspServer::get_file_content(StringView uri, u32& out_length) {
    // Check open documents first
    for (u32 i = 0; i < m_open_documents.size(); i++) {
        if (StringView(m_open_documents[i].uri.data(), m_open_documents[i].uri.size()) == uri) {
            out_length = m_open_documents[i].content.size();
            return m_open_documents[i].content.data();
        }
    }

    // Check workspace files
    String uri_str(uri);
    auto it = m_workspace_files.find(uri_str);
    if (it != m_workspace_files.end()) {
        out_length = it->second.content.size();
        return it->second.content.data();
    }

    out_length = 0;
    return nullptr;
}

OpenDocument* LspServer::find_document(StringView uri) {
    for (u32 i = 0; i < m_open_documents.size(); i++) {
        if (StringView(m_open_documents[i].uri.data(), m_open_documents[i].uri.size()) == uri) {
            return &m_open_documents[i];
        }
    }
    return nullptr;
}

} // namespace rx
