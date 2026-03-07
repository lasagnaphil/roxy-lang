#include "roxy/lsp/server.hpp"
#include "roxy/lsp/lsp_parser.hpp"
#include "roxy/lsp/indexer.hpp"
#include "roxy/lsp/cst_lowering.hpp"
#include "roxy/core/bump_allocator.hpp"
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
    } else if (method == StringView("textDocument/hover")) {
        handle_hover(params, request_id);
    } else if (method == StringView("textDocument/references")) {
        handle_references(params, request_id);
    } else if (method == StringView("textDocument/rename")) {
        handle_rename(params, request_id);
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
            writer.write_key_bool("hoverProvider", true);
            writer.write_key_bool("referencesProvider", true);
            writer.write_key_bool("renameProvider", true);

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

    // Build initial analysis context from workspace files
    rebuild_analysis_context();
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

// Check if declaration-level stubs changed (structs, enums, functions, traits)
static bool declarations_changed(const FileStubs& old_stubs, const FileStubs& new_stubs) {
    // Quick size checks
    if (old_stubs.structs.size() != new_stubs.structs.size()) return true;
    if (old_stubs.enums.size() != new_stubs.enums.size()) return true;
    if (old_stubs.functions.size() != new_stubs.functions.size()) return true;
    if (old_stubs.traits.size() != new_stubs.traits.size()) return true;
    if (old_stubs.methods.size() != new_stubs.methods.size()) return true;

    // Check struct names and field counts
    for (u32 i = 0; i < old_stubs.structs.size(); i++) {
        if (old_stubs.structs[i].name != new_stubs.structs[i].name) return true;
        if (old_stubs.structs[i].fields.size() != new_stubs.structs[i].fields.size()) return true;
    }

    // Check enum names
    for (u32 i = 0; i < old_stubs.enums.size(); i++) {
        if (old_stubs.enums[i].name != new_stubs.enums[i].name) return true;
    }

    // Check function signatures
    for (u32 i = 0; i < old_stubs.functions.size(); i++) {
        if (old_stubs.functions[i].name != new_stubs.functions[i].name) return true;
        if (old_stubs.functions[i].params.size() != new_stubs.functions[i].params.size()) return true;
    }

    return false;
}

void LspServer::parse_and_publish_diagnostics(OpenDocument& doc) {
    // Create a fresh allocator for this parse
    BumpAllocator allocator(8192);

    Lexer lexer(doc.content.data(), doc.content.size());
    LspParser parser(lexer, allocator);
    SyntaxTree tree = parser.parse();

    // Index the CST for document symbols
    FileIndexer indexer;
    FileStubs new_stubs = indexer.index(tree.root);

    // Check if declarations changed and rebuild analysis context if so
    bool decls_changed = declarations_changed(doc.stubs, new_stubs);
    doc.stubs = std::move(new_stubs);

    if (decls_changed) {
        // Update global index first
        m_global_index.update_file(
            StringView(doc.uri.data(), doc.uri.size()), doc.stubs);
        rebuild_analysis_context();
    }

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

    // Collect semantic diagnostics if no parse errors
    if (tree.diagnostics.empty()) {
        collect_semantic_diagnostics(tree.root, allocator,
                                     tree.source, tree.source_length,
                                     lsp_diagnostics);
    }

    publish_diagnostics(
        StringView(doc.uri.data(), doc.uri.size()),
        lsp_diagnostics);
}

void LspServer::collect_semantic_diagnostics(SyntaxNode* root, BumpAllocator& allocator,
                                              const char* source, u32 source_length,
                                              Vector<LspDiagnostic>& out_diagnostics) {
    if (!root) return;

    for (u32 i = 0; i < root->children.size(); i++) {
        SyntaxKind kind = root->children[i]->kind;
        if (kind != SyntaxKind::NodeFunDecl && kind != SyntaxKind::NodeMethodDecl &&
            kind != SyntaxKind::NodeConstructorDecl && kind != SyntaxKind::NodeDestructorDecl) {
            continue;
        }

        if (!m_analysis_context.is_initialized()) continue;

        BumpAllocator ast_allocator(4096);
        BodyAnalysisResult result = m_analysis_context.analyze_function_body(
            root->children[i], ast_allocator);

        if (result.decl) {
            // Convert SemanticErrors to LspDiagnostics
            for (u32 j = 0; j < result.errors.size(); j++) {
                const SemanticError& err = result.errors[j];

                TextRange text_range;
                text_range.start = err.loc.offset;
                text_range.end = err.loc.end_offset > err.loc.offset
                    ? err.loc.end_offset : err.loc.offset + 1;

                LspDiagnostic lsp_diag;
                lsp_diag.range = text_range_to_lsp_range(source, source_length, text_range);
                lsp_diag.severity = DiagnosticSeverity::Error;
                lsp_diag.message = String(err.message);
                out_diagnostics.push_back(std::move(lsp_diag));
            }
        }
        delete result.symbols;
    }
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
                SyntaxNode* enclosing_fn = find_enclosing_function(parent);

                if (enclosing_fn && m_analysis_context.is_initialized()) {
                    BumpAllocator ast_allocator(8192);
                    BodyAnalysisResult result = m_analysis_context.analyze_function_body(
                        enclosing_fn, ast_allocator);

                    if (result.decl) {
                        tsl::robin_map<String, Type*> local_vars;
                        m_analysis_context.collect_local_variables(result.decl, local_vars);

                        SyntaxNode* object_expr = parent->children[0];
                        Type* resolved = m_analysis_context.resolve_cst_expr_type(
                            object_expr, local_vars);

                        // Unwrap reference types
                        if (resolved && resolved->is_reference()) {
                            resolved = resolved->ref_info.inner_type;
                        }

                        if (resolved && resolved->kind == TypeKind::Struct) {
                            // Check fields first
                            for (const auto& field : resolved->struct_info.fields) {
                                if (field.name == identifier) {
                                    String type_str = LspAnalysisContext::type_to_string(resolved);
                                    match = m_global_index.find_field(
                                        StringView(type_str.data(), type_str.size()), identifier);
                                    break;
                                }
                            }

                            // Check methods via hierarchy
                            if (!match) {
                                Type* found_in = nullptr;
                                const MethodInfo* method_info =
                                    m_analysis_context.types().lookup_method(
                                        resolved, identifier, &found_in);
                                if (method_info && found_in && found_in->kind == TypeKind::Struct) {
                                    String found_str = LspAnalysisContext::type_to_string(found_in);
                                    match = m_global_index.find_method(
                                        StringView(found_str.data(), found_str.size()), identifier);
                                }
                            }
                        }
                    }
                    delete result.symbols;
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

        // Resolve receiver type via semantic analysis
        if (!receiver_ident.empty() && m_analysis_context.is_initialized()) {
            SyntaxNode* cursor_node = find_node_at_offset(tree.root, byte_offset > 0 ? byte_offset - 1 : 0);
            SyntaxNode* enclosing_fn = cursor_node ? find_enclosing_function(cursor_node) : nullptr;

            if (enclosing_fn) {
                BumpAllocator ast_allocator(8192);
                BodyAnalysisResult body_result = m_analysis_context.analyze_function_body(
                    enclosing_fn, ast_allocator);

                if (body_result.decl) {
                    tsl::robin_map<String, Type*> local_vars;
                    m_analysis_context.collect_local_variables(body_result.decl, local_vars);

                    auto var_it = local_vars.find(String(receiver_ident));
                    Type* receiver_type = (var_it != local_vars.end()) ? var_it->second : nullptr;

                    if (receiver_type && !receiver_type->is_error()) {
                        Type* base_type = receiver_type->base_type();

                        if (base_type->kind == TypeKind::Struct) {
                            // Emit fields
                            for (const auto& field : base_type->struct_info.fields) {
                                String field_type_str = LspAnalysisContext::type_to_string(field.type);
                                write_completion_item(writer, field.name,
                                    CompletionItemKind::Field,
                                    StringView(field_type_str.data(), field_type_str.size()));
                            }
                            // Emit methods
                            for (const auto& method : base_type->struct_info.methods) {
                                String sig;
                                sig.push_back('(');
                                for (u32 pi = 0; pi < method.param_types.size(); pi++) {
                                    if (pi > 0) sig.append(", ", 2);
                                    String param_str = LspAnalysisContext::type_to_string(method.param_types[pi]);
                                    sig.append(param_str.data(), param_str.size());
                                }
                                sig.append("): ", 3);
                                String ret_str = LspAnalysisContext::type_to_string(method.return_type);
                                sig.append(ret_str.data(), ret_str.size());
                                write_completion_item(writer, method.name,
                                    CompletionItemKind::Method,
                                    StringView(sig.data(), sig.size()));
                            }
                        }
                    }
                }
                delete body_result.symbols;
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
        // Local variables from semantic analysis
        SyntaxNode* cursor_node = find_node_at_offset(tree.root, byte_offset > 0 ? byte_offset - 1 : 0);
        SyntaxNode* enclosing_fn = cursor_node ? find_enclosing_function(cursor_node) : nullptr;

        if (enclosing_fn && m_analysis_context.is_initialized()) {
            BumpAllocator ast_allocator(8192);
            BodyAnalysisResult body_result = m_analysis_context.analyze_function_body(
                enclosing_fn, ast_allocator);

            if (body_result.decl) {
                tsl::robin_map<String, Type*> local_vars;
                m_analysis_context.collect_local_variables(body_result.decl, local_vars);

                for (auto it = local_vars.begin(); it != local_vars.end(); ++it) {
                    StringView var_name(it->first.data(), it->first.size());
                    String type_str = LspAnalysisContext::type_to_string(it->second);
                    write_completion_item(writer, var_name, CompletionItemKind::Variable,
                        StringView(type_str.data(), type_str.size()));
                }
            }
            delete body_result.symbols;
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

// --- Hover support ---

static String build_hover_markdown(StringView content) {
    String result;
    result.append("```roxy\n", 8);
    result.append(content.data(), content.size());
    result.append("\n```", 4);
    return result;
}

static void write_hover_response(LspTransport& transport, i64 id,
                                  StringView hover_text, const char* source, u32 source_length,
                                  TextRange range) {
    String markdown = build_hover_markdown(hover_text);
    LspRange lsp_range = text_range_to_lsp_range(source, source_length, range);

    String result;
    JsonWriter writer(result);
    writer.write_start_object();
    {
        writer.write_key("contents");
        writer.write_start_object();
        writer.write_key_string("kind", "markdown");
        writer.write_key_string("value", StringView(markdown.data(), markdown.size()));
        writer.write_end_object();

        writer.write_key("range");
        write_lsp_range(writer, lsp_range);
    }
    writer.write_end_object();

    transport.write_response(id, StringView(result.data(), result.size()));
}

void LspServer::handle_hover(const JsonValue& params, i64 id) {
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
    OpenDocument* doc = find_document(uri);
    if (!doc) {
        m_transport.write_response(id, "null");
        return;
    }

    const JsonValue* line_val = position_val->find("line");
    const JsonValue* char_val = position_val->find("character");
    if (!line_val || !line_val->is_int() || !char_val || !char_val->is_int()) {
        m_transport.write_response(id, "null");
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

    SyntaxNode* node = find_node_at_offset(tree.root, byte_offset);
    if (!node) {
        m_transport.write_response(id, "null");
        return;
    }

    const char* source = doc->content.data();
    u32 source_length = doc->content.size();

    // --- Keyword literals ---
    if (node->kind == SyntaxKind::TokenKwTrue || node->kind == SyntaxKind::TokenKwFalse) {
        write_hover_response(m_transport, id, "bool", source, source_length, node->range);
        return;
    }

    if (node->kind == SyntaxKind::TokenKwNil) {
        write_hover_response(m_transport, id, "nil", source, source_length, node->range);
        return;
    }

    // --- Self keyword ---
    if (node->kind == SyntaxKind::TokenKwSelf) {
        SyntaxNode* enclosing_fn = find_enclosing_function(node);
        if (enclosing_fn) {
            SyntaxNode* struct_ident = nullptr;
            for (u32 i = 0; i < enclosing_fn->children.size(); i++) {
                if (enclosing_fn->children[i]->kind == SyntaxKind::TokenIdentifier) {
                    struct_ident = enclosing_fn->children[i];
                    break;
                }
            }
            if (struct_ident) {
                String hover_text("self: ");
                hover_text.append(struct_ident->token.text().data(), struct_ident->token.text().size());
                write_hover_response(m_transport, id,
                    StringView(hover_text.data(), hover_text.size()),
                    source, source_length, node->range);
                return;
            }
        }
        m_transport.write_response(id, "null");
        return;
    }

    // Only handle identifiers from here on
    if (node->kind != SyntaxKind::TokenIdentifier) {
        m_transport.write_response(id, "null");
        return;
    }

    StringView identifier = node->token.text();
    SyntaxNode* parent = node->parent;

    // --- Field/method access (NodeGetExpr) ---
    if (parent && parent->kind == SyntaxKind::NodeGetExpr) {
        bool is_member_name = parent->children.size() >= 3 &&
            parent->children[parent->children.size() - 1] == node;

        if (is_member_name) {
            SyntaxNode* enclosing_fn = find_enclosing_function(parent);
            if (enclosing_fn && m_analysis_context.is_initialized()) {
                BumpAllocator ast_allocator(8192);
                BodyAnalysisResult result = m_analysis_context.analyze_function_body(
                    enclosing_fn, ast_allocator);

                if (result.decl) {
                    tsl::robin_map<String, Type*> local_vars;
                    m_analysis_context.collect_local_variables(result.decl, local_vars);

                    SyntaxNode* object_expr = parent->children[0];
                    Type* resolved_type = m_analysis_context.resolve_cst_expr_type(
                        object_expr, local_vars);

                    // Unwrap reference types
                    if (resolved_type && resolved_type->is_reference()) {
                        resolved_type = resolved_type->ref_info.inner_type;
                    }

                    if (resolved_type && resolved_type->kind == TypeKind::Struct) {
                        // Check fields
                        for (const auto& field : resolved_type->struct_info.fields) {
                            if (field.name == identifier) {
                                String hover_text("(field) ");
                                String type_str = LspAnalysisContext::type_to_string(resolved_type);
                                hover_text.append(type_str.data(), type_str.size());
                                hover_text.push_back('.');
                                hover_text.append(identifier.data(), identifier.size());
                                hover_text.append(": ", 2);
                                String field_type_str = LspAnalysisContext::type_to_string(field.type);
                                hover_text.append(field_type_str.data(), field_type_str.size());
                                write_hover_response(m_transport, id,
                                    StringView(hover_text.data(), hover_text.size()),
                                    source, source_length, node->range);
                                delete result.symbols;
                                return;
                            }
                        }

                        // Check methods via TypeCache
                        Type* found_in = nullptr;
                        const MethodInfo* method_info = m_analysis_context.types().lookup_method(
                            resolved_type, identifier, &found_in);
                        if (method_info) {
                            String hover_text("fun ");
                            String type_str = LspAnalysisContext::type_to_string(
                                found_in ? found_in : resolved_type);
                            hover_text.append(type_str.data(), type_str.size());
                            hover_text.push_back('.');
                            hover_text.append(identifier.data(), identifier.size());
                            hover_text.push_back('(');
                            for (u32 pi = 0; pi < method_info->param_types.size(); pi++) {
                                if (pi > 0) hover_text.append(", ", 2);
                                String param_str = LspAnalysisContext::type_to_string(
                                    method_info->param_types[pi]);
                                hover_text.append(param_str.data(), param_str.size());
                            }
                            hover_text.append("): ", 3);
                            String ret_str = LspAnalysisContext::type_to_string(method_info->return_type);
                            hover_text.append(ret_str.data(), ret_str.size());
                            write_hover_response(m_transport, id,
                                StringView(hover_text.data(), hover_text.size()),
                                source, source_length, node->range);
                            delete result.symbols;
                            return;
                        }
                    }
                }
                delete result.symbols;
            }

            m_transport.write_response(id, "null");
            return;
        }
    }

    // --- Static access (NodeStaticGetExpr): Enum::Variant or Type ---
    if (parent && parent->kind == SyntaxKind::NodeStaticGetExpr) {
        // Determine if this is the type child (first identifier) or member child (after ::)
        bool is_member_child = false;
        if (parent->children.size() >= 3) {
            is_member_child = (parent->children[parent->children.size() - 1] == node);
        }

        if (is_member_child) {
            // This is the variant name (e.g., Red in Color::Red)
            // Find the type name (first identifier child)
            StringView type_name;
            for (u32 i = 0; i < parent->children.size(); i++) {
                if (parent->children[i]->kind == SyntaxKind::TokenIdentifier &&
                    parent->children[i] != node) {
                    type_name = parent->children[i]->token.text();
                    break;
                }
            }

            if (!type_name.empty()) {
                String hover_text("(variant) ");
                hover_text.append(type_name.data(), type_name.size());
                hover_text.append("::", 2);
                hover_text.append(identifier.data(), identifier.size());
                write_hover_response(m_transport, id,
                    StringView(hover_text.data(), hover_text.size()),
                    source, source_length, node->range);
                return;
            }
        } else {
            // This is the type name (e.g., Color in Color::Red)
            if (m_global_index.find_enum(identifier)) {
                String hover_text("enum ");
                hover_text.append(identifier.data(), identifier.size());
                write_hover_response(m_transport, id,
                    StringView(hover_text.data(), hover_text.size()),
                    source, source_length, node->range);
                return;
            }
            if (m_global_index.find_struct(identifier)) {
                String hover_text("struct ");
                hover_text.append(identifier.data(), identifier.size());
                write_hover_response(m_transport, id,
                    StringView(hover_text.data(), hover_text.size()),
                    source, source_length, node->range);
                return;
            }
        }

        m_transport.write_response(id, "null");
        return;
    }

    // --- Type annotation (NodeTypeExpr) ---
    if (parent && parent->kind == SyntaxKind::NodeTypeExpr) {
        if (m_global_index.find_struct(identifier)) {
            String hover_text("struct ");
            hover_text.append(identifier.data(), identifier.size());
            write_hover_response(m_transport, id,
                StringView(hover_text.data(), hover_text.size()),
                source, source_length, node->range);
            return;
        }
        if (m_global_index.find_enum(identifier)) {
            String hover_text("enum ");
            hover_text.append(identifier.data(), identifier.size());
            write_hover_response(m_transport, id,
                StringView(hover_text.data(), hover_text.size()),
                source, source_length, node->range);
            return;
        }
        if (m_global_index.find_trait(identifier)) {
            String hover_text("trait ");
            hover_text.append(identifier.data(), identifier.size());
            write_hover_response(m_transport, id,
                StringView(hover_text.data(), hover_text.size()),
                source, source_length, node->range);
            return;
        }
        m_transport.write_response(id, "null");
        return;
    }

    // --- Function call (NodeCallExpr) ---
    if (parent && parent->kind == SyntaxKind::NodeCallExpr) {
        // Check if this is the callee (first child)
        bool is_callee = parent->children.size() > 0 && parent->children[0] == node;
        if (is_callee) {
            StringView func_sig = m_global_index.find_function_signature(identifier);
            if (!func_sig.empty()) {
                String hover_text("fun ");
                hover_text.append(identifier.data(), identifier.size());
                hover_text.append(func_sig.data(), func_sig.size());
                write_hover_response(m_transport, id,
                    StringView(hover_text.data(), hover_text.size()),
                    source, source_length, node->range);
                return;
            }
        }
    }

    // --- Struct literal (NodeStructLiteralExpr) ---
    if (parent && parent->kind == SyntaxKind::NodeStructLiteralExpr) {
        // Check if this is the struct name (first child)
        bool is_struct_name = parent->children.size() > 0 && parent->children[0] == node;
        if (is_struct_name && m_global_index.find_struct(identifier)) {
            String hover_text("struct ");
            hover_text.append(identifier.data(), identifier.size());
            write_hover_response(m_transport, id,
                StringView(hover_text.data(), hover_text.size()),
                source, source_length, node->range);
            return;
        }
    }

    // --- Field initializer (NodeFieldInit) ---
    if (parent && parent->kind == SyntaxKind::NodeFieldInit) {
        // Check if this is the field name (first child)
        bool is_field_name = parent->children.size() > 0 && parent->children[0] == node;
        if (is_field_name) {
            // Walk up to find the enclosing struct literal
            SyntaxNode* struct_literal = parent->parent;
            if (struct_literal && struct_literal->kind == SyntaxKind::NodeStructLiteralExpr) {
                // Get struct name from first identifier child
                for (u32 i = 0; i < struct_literal->children.size(); i++) {
                    if (struct_literal->children[i]->kind == SyntaxKind::TokenIdentifier) {
                        StringView struct_name = struct_literal->children[i]->token.text();

                        // Walk inheritance chain for field type
                        StringView current_type = struct_name;
                        u32 depth = 0;
                        while (!current_type.empty() && depth < 16) {
                            StringView field_type = m_global_index.find_field_type(current_type, identifier);
                            if (!field_type.empty()) {
                                String hover_text("(field) ");
                                hover_text.append(current_type.data(), current_type.size());
                                hover_text.push_back('.');
                                hover_text.append(identifier.data(), identifier.size());
                                hover_text.append(": ", 2);
                                hover_text.append(field_type.data(), field_type.size());
                                write_hover_response(m_transport, id,
                                    StringView(hover_text.data(), hover_text.size()),
                                    source, source_length, node->range);
                                return;
                            }
                            current_type = m_global_index.find_struct_parent(current_type);
                            depth++;
                        }
                        break;
                    }
                }
            }
        }
    }

    // --- Local variable resolution (expression context) ---
    {
        SyntaxNode* enclosing_fn = find_enclosing_function(node);
        if (enclosing_fn && m_analysis_context.is_initialized()) {
            BumpAllocator ast_allocator(8192);
            BodyAnalysisResult result = m_analysis_context.analyze_function_body(
                enclosing_fn, ast_allocator);

            if (result.decl) {
                tsl::robin_map<String, Type*> local_vars;
                m_analysis_context.collect_local_variables(result.decl, local_vars);

                auto var_it = local_vars.find(String(identifier));
                if (var_it != local_vars.end() && var_it->second && !var_it->second->is_error()) {
                    String hover_text("(variable) ");
                    hover_text.append(identifier.data(), identifier.size());
                    hover_text.append(": ", 2);
                    String type_str = LspAnalysisContext::type_to_string(var_it->second);
                    hover_text.append(type_str.data(), type_str.size());
                    write_hover_response(m_transport, id,
                        StringView(hover_text.data(), hover_text.size()),
                        source, source_length, node->range);
                    delete result.symbols;
                    return;
                }
            }
            delete result.symbols;
        }
    }

    // --- Fallback: GlobalIndex lookup ---
    if (m_global_index.find_function(identifier)) {
        StringView func_sig = m_global_index.find_function_signature(identifier);
        if (!func_sig.empty()) {
            String hover_text("fun ");
            hover_text.append(identifier.data(), identifier.size());
            hover_text.append(func_sig.data(), func_sig.size());
            write_hover_response(m_transport, id,
                StringView(hover_text.data(), hover_text.size()),
                source, source_length, node->range);
            return;
        }
    }
    if (m_global_index.find_struct(identifier)) {
        String hover_text("struct ");
        hover_text.append(identifier.data(), identifier.size());
        write_hover_response(m_transport, id,
            StringView(hover_text.data(), hover_text.size()),
            source, source_length, node->range);
        return;
    }
    if (m_global_index.find_enum(identifier)) {
        String hover_text("enum ");
        hover_text.append(identifier.data(), identifier.size());
        write_hover_response(m_transport, id,
            StringView(hover_text.data(), hover_text.size()),
            source, source_length, node->range);
        return;
    }
    if (m_global_index.find_trait(identifier)) {
        String hover_text("trait ");
        hover_text.append(identifier.data(), identifier.size());
        write_hover_response(m_transport, id,
            StringView(hover_text.data(), hover_text.size()),
            source, source_length, node->range);
        return;
    }
    if (m_global_index.find_global(identifier)) {
        StringView global_type = m_global_index.find_global_type(identifier);
        String hover_text("(global) ");
        hover_text.append(identifier.data(), identifier.size());
        if (!global_type.empty()) {
            hover_text.append(": ", 2);
            hover_text.append(global_type.data(), global_type.size());
        }
        write_hover_response(m_transport, id,
            StringView(hover_text.data(), hover_text.size()),
            source, source_length, node->range);
        return;
    }

    m_transport.write_response(id, "null");
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

void LspServer::rebuild_analysis_context() {
    // Collect source files from open documents and workspace files
    Vector<LspAnalysisContext::SourceFile> source_files;

    // We need to parse each file's CST for the analysis context
    // Use a temporary allocator for parsing
    BumpAllocator parse_allocator(16384);

    // Add open documents
    for (u32 i = 0; i < m_open_documents.size(); i++) {
        const OpenDocument& doc = m_open_documents[i];
        Lexer lexer(doc.content.data(), doc.content.size());
        LspParser parser(lexer, parse_allocator);
        SyntaxTree tree = parser.parse();

        LspAnalysisContext::SourceFile source_file;
        source_file.uri = StringView(doc.uri.data(), doc.uri.size());
        source_file.source = doc.content.data();
        source_file.source_length = doc.content.size();
        source_file.cst_root = tree.root;
        source_files.push_back(source_file);
    }

    // Add workspace files not already covered by open documents
    for (auto& [uri, workspace_file] : m_workspace_files) {
        // Skip if already in open documents
        bool already_open = false;
        for (u32 i = 0; i < m_open_documents.size(); i++) {
            if (StringView(m_open_documents[i].uri.data(), m_open_documents[i].uri.size()) ==
                StringView(uri.data(), uri.size())) {
                already_open = true;
                break;
            }
        }
        if (already_open) continue;

        Lexer lexer(workspace_file.content.data(), workspace_file.content.size());
        LspParser parser(lexer, parse_allocator);
        SyntaxTree tree = parser.parse();

        LspAnalysisContext::SourceFile source_file;
        source_file.uri = StringView(workspace_file.uri.data(), workspace_file.uri.size());
        source_file.source = workspace_file.content.data();
        source_file.source_length = workspace_file.content.size();
        source_file.cst_root = tree.root;
        source_files.push_back(source_file);
    }

    Span<LspAnalysisContext::SourceFile> source_span(
        source_files.data(), static_cast<u32>(source_files.size()));
    m_analysis_context.rebuild_declarations(source_span);
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

// --- Find References + Rename support ---

// Symbol category for reference searching
enum class SymbolCategory : u8 {
    Function,
    Struct,
    Enum,
    Trait,
    Global,
    Method,
    Field,
    Constructor,
    Local,
    Parameter,
};

// Describes the canonical identity of a symbol for reference matching
struct SymbolIdentity {
    SymbolCategory category;
    String name;                // Primary name (function, struct, enum, trait, global, local, param, or member name)
    String qualifier;           // For methods/fields/constructors: the struct name
    TextRange enclosing_range;  // For locals/params: the byte range of the enclosing function
    String enclosing_uri;       // For locals/params: the URI of the file containing them
};

// Collect all identifier tokens (and optionally self tokens) matching a name via DFS
static void collect_identifiers(SyntaxNode* root, StringView name, Vector<SyntaxNode*>& out) {
    if (!root) return;

    if (root->kind == SyntaxKind::TokenIdentifier && root->token.text() == name) {
        out.push_back(root);
    } else if (root->kind == SyntaxKind::TokenKwSelf && name == StringView("self")) {
        out.push_back(root);
    }

    for (u32 i = 0; i < root->children.size(); i++) {
        collect_identifiers(root->children[i], name, out);
    }
}

// Identify what symbol the cursor is on, producing a SymbolIdentity
static bool identify_symbol_at_cursor(SyntaxNode* node, const GlobalIndex& index,
                                       StringView uri, SymbolIdentity& out_identity,
                                       LspAnalysisContext* analysis_context = nullptr) {
    if (!node) return false;

    // Handle self keyword
    if (node->kind == SyntaxKind::TokenKwSelf) {
        SyntaxNode* enclosing_fn = find_enclosing_function(node);
        if (!enclosing_fn) return false;

        // Find struct name from enclosing method/constructor/destructor
        for (u32 i = 0; i < enclosing_fn->children.size(); i++) {
            if (enclosing_fn->children[i]->kind == SyntaxKind::TokenIdentifier) {
                out_identity.category = SymbolCategory::Struct;
                out_identity.name = String(enclosing_fn->children[i]->token.text());
                return true;
            }
        }
        return false;
    }

    if (node->kind != SyntaxKind::TokenIdentifier) return false;

    StringView identifier = node->token.text();
    SyntaxNode* parent = node->parent;

    if (parent) {
        // Field/method access: NodeGetExpr
        if (parent->kind == SyntaxKind::NodeGetExpr) {
            bool is_member_name = parent->children.size() >= 3 &&
                parent->children[parent->children.size() - 1] == node;

            if (is_member_name) {
                // Resolve receiver type to determine qualifier
                SyntaxNode* object_expr = parent->children[0];
                String receiver_type;

                SyntaxNode* enclosing_fn = find_enclosing_function(parent);
                if (enclosing_fn && analysis_context && analysis_context->is_initialized()) {
                    BumpAllocator ast_allocator(8192);
                    BodyAnalysisResult body_result = analysis_context->analyze_function_body(
                        enclosing_fn, ast_allocator);

                    if (body_result.decl) {
                        tsl::robin_map<String, Type*> local_vars;
                        analysis_context->collect_local_variables(body_result.decl, local_vars);

                        Type* resolved = analysis_context->resolve_cst_expr_type(
                            object_expr, local_vars);
                        if (resolved) {
                            receiver_type = LspAnalysisContext::type_to_string(resolved);
                        }
                    }
                    delete body_result.symbols;
                }

                if (!receiver_type.empty()) {
                    // Walk inheritance chain to find which struct owns this field/method
                    StringView current_type(receiver_type.data(), receiver_type.size());
                    u32 depth = 0;
                    while (!current_type.empty() && depth < 16) {
                        if (index.find_field(current_type, identifier)) {
                            out_identity.category = SymbolCategory::Field;
                            out_identity.name = String(identifier);
                            out_identity.qualifier = String(current_type);
                            return true;
                        }
                        if (index.find_method(current_type, identifier)) {
                            out_identity.category = SymbolCategory::Method;
                            out_identity.name = String(identifier);
                            out_identity.qualifier = String(current_type);
                            return true;
                        }
                        current_type = index.find_struct_parent(current_type);
                        depth++;
                    }
                }
                // Fallback: if we can't resolve receiver type, don't return references
                return false;
            }
            // Cursor on receiver expression — fall through to general identifier handling
        }

        // Type annotation: NodeTypeExpr
        if (parent->kind == SyntaxKind::NodeTypeExpr) {
            if (index.find_struct(identifier)) {
                out_identity.category = SymbolCategory::Struct;
                out_identity.name = String(identifier);
                return true;
            }
            if (index.find_enum(identifier)) {
                out_identity.category = SymbolCategory::Enum;
                out_identity.name = String(identifier);
                return true;
            }
            if (index.find_trait(identifier)) {
                out_identity.category = SymbolCategory::Trait;
                out_identity.name = String(identifier);
                return true;
            }
            return false;
        }

        // Function call: NodeCallExpr
        if (parent->kind == SyntaxKind::NodeCallExpr) {
            bool is_callee = parent->children.size() > 0 && parent->children[0] == node;
            if (is_callee) {
                if (index.find_function(identifier)) {
                    out_identity.category = SymbolCategory::Function;
                    out_identity.name = String(identifier);
                    return true;
                }
                // Could be constructor call
                if (index.find_struct(identifier)) {
                    out_identity.category = SymbolCategory::Struct;
                    out_identity.name = String(identifier);
                    return true;
                }
            }
        }

        // Static access: NodeStaticGetExpr (Enum::Variant or Type::Constructor)
        if (parent->kind == SyntaxKind::NodeStaticGetExpr) {
            bool is_member_child = parent->children.size() >= 3 &&
                parent->children[parent->children.size() - 1] == node;

            if (!is_member_child) {
                // This is the type name (first identifier)
                if (index.find_enum(identifier)) {
                    out_identity.category = SymbolCategory::Enum;
                    out_identity.name = String(identifier);
                    return true;
                }
                if (index.find_struct(identifier)) {
                    out_identity.category = SymbolCategory::Struct;
                    out_identity.name = String(identifier);
                    return true;
                }
            }
            // Member child of static access — could be enum variant or constructor name
            // For now, don't handle these as referable symbols
            return false;
        }

        // Struct literal: NodeStructLiteralExpr
        if (parent->kind == SyntaxKind::NodeStructLiteralExpr) {
            bool is_struct_name = parent->children.size() > 0 && parent->children[0] == node;
            if (is_struct_name && index.find_struct(identifier)) {
                out_identity.category = SymbolCategory::Struct;
                out_identity.name = String(identifier);
                return true;
            }
        }

        // Field initializer name in struct literal: NodeFieldInit
        if (parent->kind == SyntaxKind::NodeFieldInit) {
            bool is_field_name = parent->children.size() > 0 && parent->children[0] == node;
            if (is_field_name) {
                // Walk up to find struct literal
                SyntaxNode* struct_literal = parent->parent;
                if (struct_literal && struct_literal->kind == SyntaxKind::NodeStructLiteralExpr) {
                    for (u32 i = 0; i < struct_literal->children.size(); i++) {
                        if (struct_literal->children[i]->kind == SyntaxKind::TokenIdentifier) {
                            StringView struct_name = struct_literal->children[i]->token.text();
                            // Walk inheritance chain
                            StringView current_type = struct_name;
                            u32 depth = 0;
                            while (!current_type.empty() && depth < 16) {
                                if (index.find_field(current_type, identifier)) {
                                    out_identity.category = SymbolCategory::Field;
                                    out_identity.name = String(identifier);
                                    out_identity.qualifier = String(current_type);
                                    return true;
                                }
                                current_type = index.find_struct_parent(current_type);
                                depth++;
                            }
                            break;
                        }
                    }
                }
            }
        }

        // Var declaration name: NodeVarDecl
        if (parent->kind == SyntaxKind::NodeVarDecl) {
            // Check if this is the name token of the var decl
            // NodeVarDecl children: [var, name, ...]
            for (u32 i = 0; i < parent->children.size(); i++) {
                if (parent->children[i]->kind == SyntaxKind::TokenIdentifier &&
                    parent->children[i] == node) {
                    // Check if it's a global variable
                    if (index.find_global(identifier)) {
                        out_identity.category = SymbolCategory::Global;
                        out_identity.name = String(identifier);
                        return true;
                    }
                    // It's a local variable
                    SyntaxNode* enclosing_fn = find_enclosing_function(node);
                    if (enclosing_fn) {
                        out_identity.category = SymbolCategory::Local;
                        out_identity.name = String(identifier);
                        out_identity.enclosing_range = enclosing_fn->range;
                        out_identity.enclosing_uri = String(uri);
                        return true;
                    }
                    break;
                }
            }
        }

        // Parameter in param list
        if (parent->kind == SyntaxKind::NodeParam) {
            SyntaxNode* enclosing_fn = find_enclosing_function(node);
            if (enclosing_fn) {
                out_identity.category = SymbolCategory::Parameter;
                out_identity.name = String(identifier);
                out_identity.enclosing_range = enclosing_fn->range;
                out_identity.enclosing_uri = String(uri);
                return true;
            }
        }

        // Function declaration name
        if (parent->kind == SyntaxKind::NodeFunDecl) {
            out_identity.category = SymbolCategory::Function;
            out_identity.name = String(identifier);
            return true;
        }

        // Method declaration name — find the struct qualifier
        if (parent->kind == SyntaxKind::NodeMethodDecl) {
            // NodeMethodDecl children: [StructName, '.', MethodName, ...]
            // Find which child is this node — if it's the struct name or method name
            bool found_dot = false;
            StringView struct_name;
            for (u32 i = 0; i < parent->children.size(); i++) {
                if (parent->children[i]->kind == SyntaxKind::TokenDot) {
                    found_dot = true;
                }
                if (parent->children[i] == node) {
                    if (found_dot) {
                        // This is the method name
                        // Find struct name (first identifier before dot)
                        for (u32 j = 0; j < i; j++) {
                            if (parent->children[j]->kind == SyntaxKind::TokenIdentifier) {
                                struct_name = parent->children[j]->token.text();
                                break;
                            }
                        }
                        if (!struct_name.empty()) {
                            out_identity.category = SymbolCategory::Method;
                            out_identity.name = String(identifier);
                            out_identity.qualifier = String(struct_name);
                            return true;
                        }
                    } else {
                        // This is the struct name in the method declaration
                        out_identity.category = SymbolCategory::Struct;
                        out_identity.name = String(identifier);
                        return true;
                    }
                    break;
                }
            }
        }

        // Constructor declaration name
        if (parent->kind == SyntaxKind::NodeConstructorDecl) {
            // First identifier is struct name
            for (u32 i = 0; i < parent->children.size(); i++) {
                if (parent->children[i] == node) {
                    // Determine if this is struct name or constructor name
                    // First identifier = struct name
                    bool is_first_ident = true;
                    for (u32 j = 0; j < i; j++) {
                        if (parent->children[j]->kind == SyntaxKind::TokenIdentifier) {
                            is_first_ident = false;
                            break;
                        }
                    }
                    if (is_first_ident) {
                        out_identity.category = SymbolCategory::Struct;
                        out_identity.name = String(identifier);
                        return true;
                    }
                    break;
                }
            }
        }

        // Destructor declaration — struct name
        if (parent->kind == SyntaxKind::NodeDestructorDecl) {
            out_identity.category = SymbolCategory::Struct;
            out_identity.name = String(identifier);
            return true;
        }

        // Struct declaration name
        if (parent->kind == SyntaxKind::NodeStructDecl) {
            out_identity.category = SymbolCategory::Struct;
            out_identity.name = String(identifier);
            return true;
        }

        // Enum declaration name
        if (parent->kind == SyntaxKind::NodeEnumDecl) {
            out_identity.category = SymbolCategory::Enum;
            out_identity.name = String(identifier);
            return true;
        }

        // Trait declaration name
        if (parent->kind == SyntaxKind::NodeTraitDecl) {
            out_identity.category = SymbolCategory::Trait;
            out_identity.name = String(identifier);
            return true;
        }

        // Field declaration in struct body
        if (parent->kind == SyntaxKind::NodeFieldDecl) {
            // Find enclosing struct
            SyntaxNode* struct_node = parent->parent;
            if (struct_node && struct_node->kind == SyntaxKind::NodeStructDecl) {
                for (u32 i = 0; i < struct_node->children.size(); i++) {
                    if (struct_node->children[i]->kind == SyntaxKind::TokenIdentifier) {
                        out_identity.category = SymbolCategory::Field;
                        out_identity.name = String(identifier);
                        out_identity.qualifier = String(struct_node->children[i]->token.text());
                        return true;
                    }
                }
            }
        }
    }

    // General identifier — try to resolve what it refers to
    // Check if it's a local variable in the enclosing function
    SyntaxNode* enclosing_fn = find_enclosing_function(node);
    if (enclosing_fn) {
        BumpAllocator ast_allocator(8192);
        CstLowering lowering(ast_allocator);
        Decl* ast_decl = lowering.lower_decl(enclosing_fn);

        tsl::robin_set<String> local_names;
        LspAnalysisContext::collect_local_var_names(ast_decl, local_names);

        if (local_names.find(String(identifier)) != local_names.end()) {
            out_identity.category = SymbolCategory::Local;
            out_identity.name = String(identifier);
            out_identity.enclosing_range = enclosing_fn->range;
            out_identity.enclosing_uri = String(uri);
            return true;
        }
    }

    // Try global lookups
    if (index.find_function(identifier)) {
        out_identity.category = SymbolCategory::Function;
        out_identity.name = String(identifier);
        return true;
    }
    if (index.find_struct(identifier)) {
        out_identity.category = SymbolCategory::Struct;
        out_identity.name = String(identifier);
        return true;
    }
    if (index.find_enum(identifier)) {
        out_identity.category = SymbolCategory::Enum;
        out_identity.name = String(identifier);
        return true;
    }
    if (index.find_trait(identifier)) {
        out_identity.category = SymbolCategory::Trait;
        out_identity.name = String(identifier);
        return true;
    }
    if (index.find_global(identifier)) {
        out_identity.category = SymbolCategory::Global;
        out_identity.name = String(identifier);
        return true;
    }

    return false;
}

// Check if a candidate identifier node refers to the same symbol described by the identity
static bool is_reference_to_symbol(SyntaxNode* candidate, const SymbolIdentity& target,
                                    const GlobalIndex& index, SyntaxNode* file_root,
                                    StringView file_uri,
                                    LspAnalysisContext* analysis_context = nullptr) {
    if (!candidate) return false;

    StringView candidate_text;
    if (candidate->kind == SyntaxKind::TokenIdentifier) {
        candidate_text = candidate->token.text();
    } else if (candidate->kind == SyntaxKind::TokenKwSelf) {
        candidate_text = StringView("self");
    } else {
        return false;
    }

    SyntaxNode* parent = candidate->parent;

    switch (target.category) {
    case SymbolCategory::Function: {
        // Function reference: the candidate must be in a call context or a bare reference
        // and must NOT be a local variable that shadows the function name
        if (!parent) return false;

        // Skip if candidate is a field/method name after dot
        if (parent->kind == SyntaxKind::NodeGetExpr) {
            bool is_member_name = parent->children.size() >= 3 &&
                parent->children[parent->children.size() - 1] == candidate;
            if (is_member_name) return false;
        }

        // Skip if candidate is in a type annotation
        if (parent->kind == SyntaxKind::NodeTypeExpr) return false;

        // Skip if it's a field name in a field init
        if (parent->kind == SyntaxKind::NodeFieldInit &&
            parent->children.size() > 0 && parent->children[0] == candidate) return false;

        // Skip struct/enum/trait decl names
        if (parent->kind == SyntaxKind::NodeStructDecl ||
            parent->kind == SyntaxKind::NodeEnumDecl ||
            parent->kind == SyntaxKind::NodeTraitDecl) return false;

        // Skip field declarations
        if (parent->kind == SyntaxKind::NodeFieldDecl) return false;

        // Check if shadowed by a local variable
        SyntaxNode* enclosing_fn = find_enclosing_function(candidate);
        if (enclosing_fn) {
            BumpAllocator ast_allocator(4096);
            CstLowering lowering(ast_allocator);
            Decl* ast_decl = lowering.lower_decl(enclosing_fn);

            tsl::robin_set<String> local_names;
            LspAnalysisContext::collect_local_var_names(ast_decl, local_names);
            if (local_names.find(String(candidate_text)) != local_names.end()) return false;
        }

        // It's a fun decl name if parent is NodeFunDecl
        if (parent->kind == SyntaxKind::NodeFunDecl) return true;

        // Or it's a reference to the function
        return index.find_function(candidate_text) != nullptr;
    }

    case SymbolCategory::Struct: {
        if (!parent) return false;

        // Skip field/method names after dot
        if (parent->kind == SyntaxKind::NodeGetExpr) {
            bool is_member_name = parent->children.size() >= 3 &&
                parent->children[parent->children.size() - 1] == candidate;
            if (is_member_name) return false;
        }

        // Skip field init names
        if (parent->kind == SyntaxKind::NodeFieldInit &&
            parent->children.size() > 0 && parent->children[0] == candidate) return false;

        // Skip field declarations
        if (parent->kind == SyntaxKind::NodeFieldDecl) return false;

        // Skip parameter names
        if (parent->kind == SyntaxKind::NodeParam) {
            // NodeParam children: [name, ':', type]
            // If this is the first identifier (before ':'), it's the param name
            for (u32 i = 0; i < parent->children.size(); i++) {
                if (parent->children[i] == candidate) {
                    // Check if this precedes a colon
                    if (i + 1 < parent->children.size() &&
                        parent->children[i + 1]->kind == SyntaxKind::TokenColon) {
                        return false;  // This is a parameter name, not a type ref
                    }
                    break;
                }
            }
        }

        // Skip if shadowed by a local variable
        SyntaxNode* enclosing_fn = find_enclosing_function(candidate);
        if (enclosing_fn) {
            BumpAllocator ast_allocator(4096);
            CstLowering lowering(ast_allocator);
            Decl* ast_decl = lowering.lower_decl(enclosing_fn);

            tsl::robin_set<String> local_names;
            LspAnalysisContext::collect_local_var_names(ast_decl, local_names);
            if (local_names.find(String(candidate_text)) != local_names.end()) return false;
        }

        // Valid contexts: type annotations, struct literal, call expr, struct decl,
        // method/constructor/destructor decl, static get expr
        return true;
    }

    case SymbolCategory::Enum: {
        if (!parent) return false;

        // Skip field/method names after dot
        if (parent->kind == SyntaxKind::NodeGetExpr) {
            bool is_member_name = parent->children.size() >= 3 &&
                parent->children[parent->children.size() - 1] == candidate;
            if (is_member_name) return false;
        }

        // Skip field init names
        if (parent->kind == SyntaxKind::NodeFieldInit &&
            parent->children.size() > 0 && parent->children[0] == candidate) return false;

        // Skip field declarations
        if (parent->kind == SyntaxKind::NodeFieldDecl) return false;

        // Skip if shadowed by local
        SyntaxNode* enclosing_fn = find_enclosing_function(candidate);
        if (enclosing_fn) {
            BumpAllocator ast_allocator(4096);
            CstLowering lowering(ast_allocator);
            Decl* ast_decl = lowering.lower_decl(enclosing_fn);

            tsl::robin_set<String> local_names;
            LspAnalysisContext::collect_local_var_names(ast_decl, local_names);
            if (local_names.find(String(candidate_text)) != local_names.end()) return false;
        }

        return true;
    }

    case SymbolCategory::Trait: {
        if (!parent) return false;

        // Skip field/method names after dot
        if (parent->kind == SyntaxKind::NodeGetExpr) {
            bool is_member_name = parent->children.size() >= 3 &&
                parent->children[parent->children.size() - 1] == candidate;
            if (is_member_name) return false;
        }

        return true;
    }

    case SymbolCategory::Global: {
        if (!parent) return false;

        // Skip field/method names after dot
        if (parent->kind == SyntaxKind::NodeGetExpr) {
            bool is_member_name = parent->children.size() >= 3 &&
                parent->children[parent->children.size() - 1] == candidate;
            if (is_member_name) return false;
        }

        // Skip type annotations
        if (parent->kind == SyntaxKind::NodeTypeExpr) return false;

        // Skip field init names
        if (parent->kind == SyntaxKind::NodeFieldInit &&
            parent->children.size() > 0 && parent->children[0] == candidate) return false;

        // Skip field declarations
        if (parent->kind == SyntaxKind::NodeFieldDecl) return false;

        // Skip struct/enum/trait/fun decl names
        if (parent->kind == SyntaxKind::NodeStructDecl ||
            parent->kind == SyntaxKind::NodeEnumDecl ||
            parent->kind == SyntaxKind::NodeTraitDecl ||
            parent->kind == SyntaxKind::NodeFunDecl) return false;

        // Check if shadowed by a local variable
        SyntaxNode* enclosing_fn = find_enclosing_function(candidate);
        if (enclosing_fn) {
            BumpAllocator ast_allocator(4096);
            CstLowering lowering(ast_allocator);
            Decl* ast_decl = lowering.lower_decl(enclosing_fn);

            tsl::robin_set<String> local_names;
            LspAnalysisContext::collect_local_var_names(ast_decl, local_names);
            if (local_names.find(String(candidate_text)) != local_names.end()) return false;
        }

        return true;
    }

    case SymbolCategory::Method: {
        if (!parent) return false;

        // For method references, check that the candidate is the member-name
        // child of a GetExpr whose receiver resolves to the target struct
        if (parent->kind == SyntaxKind::NodeGetExpr) {
            bool is_member_name = parent->children.size() >= 3 &&
                parent->children[parent->children.size() - 1] == candidate;
            if (!is_member_name) return false;

            SyntaxNode* object_expr = parent->children[0];
            SyntaxNode* enclosing_fn = find_enclosing_function(parent);
            if (!enclosing_fn || !analysis_context || !analysis_context->is_initialized()) return false;

            BumpAllocator ast_allocator(8192);
            BodyAnalysisResult body_result = analysis_context->analyze_function_body(
                enclosing_fn, ast_allocator);

            if (!body_result.decl) {
                delete body_result.symbols;
                return false;
            }

            tsl::robin_map<String, Type*> local_vars;
            analysis_context->collect_local_variables(body_result.decl, local_vars);

            Type* receiver = analysis_context->resolve_cst_expr_type(object_expr, local_vars);
            delete body_result.symbols;

            if (!receiver) return false;
            String receiver_type = LspAnalysisContext::type_to_string(receiver);

            // Walk inheritance chain to check if the method is on the target struct
            StringView current_type(receiver_type.data(), receiver_type.size());
            u32 depth = 0;
            while (!current_type.empty() && depth < 16) {
                if (current_type == StringView(target.qualifier.data(), target.qualifier.size()) &&
                    index.find_method(current_type, candidate_text)) {
                    return true;
                }
                current_type = index.find_struct_parent(current_type);
                depth++;
            }
            return false;
        }

        // Method declaration: NodeMethodDecl — check struct name matches
        if (parent->kind == SyntaxKind::NodeMethodDecl) {
            // Check if this identifier is the method name (after the dot)
            bool found_dot = false;
            StringView struct_name;
            for (u32 i = 0; i < parent->children.size(); i++) {
                if (parent->children[i]->kind == SyntaxKind::TokenDot) {
                    found_dot = true;
                }
                if (parent->children[i] == candidate) {
                    if (found_dot) {
                        // This is the method name — find struct name
                        for (u32 j = 0; j < i; j++) {
                            if (parent->children[j]->kind == SyntaxKind::TokenIdentifier) {
                                struct_name = parent->children[j]->token.text();
                                break;
                            }
                        }
                        return struct_name == StringView(target.qualifier.data(), target.qualifier.size());
                    }
                    break;
                }
            }
        }

        return false;
    }

    case SymbolCategory::Field: {
        if (!parent) return false;

        // Field access: NodeGetExpr
        if (parent->kind == SyntaxKind::NodeGetExpr) {
            bool is_member_name = parent->children.size() >= 3 &&
                parent->children[parent->children.size() - 1] == candidate;
            if (!is_member_name) return false;

            SyntaxNode* object_expr = parent->children[0];
            SyntaxNode* enclosing_fn = find_enclosing_function(parent);
            if (!enclosing_fn || !analysis_context || !analysis_context->is_initialized()) return false;

            BumpAllocator ast_allocator(8192);
            BodyAnalysisResult body_result = analysis_context->analyze_function_body(
                enclosing_fn, ast_allocator);

            if (!body_result.decl) {
                delete body_result.symbols;
                return false;
            }

            tsl::robin_map<String, Type*> local_vars;
            analysis_context->collect_local_variables(body_result.decl, local_vars);

            Type* receiver = analysis_context->resolve_cst_expr_type(object_expr, local_vars);
            delete body_result.symbols;

            if (!receiver) return false;
            String receiver_type = LspAnalysisContext::type_to_string(receiver);

            // Walk inheritance chain
            StringView current_type(receiver_type.data(), receiver_type.size());
            u32 depth = 0;
            while (!current_type.empty() && depth < 16) {
                if (current_type == StringView(target.qualifier.data(), target.qualifier.size()) &&
                    index.find_field(current_type, candidate_text)) {
                    return true;
                }
                current_type = index.find_struct_parent(current_type);
                depth++;
            }
            return false;
        }

        // Field declaration in struct body: NodeFieldDecl
        if (parent->kind == SyntaxKind::NodeFieldDecl) {
            // Check if this is the field name (before ':')
            bool is_name = false;
            for (u32 i = 0; i < parent->children.size(); i++) {
                if (parent->children[i] == candidate) {
                    is_name = true;
                    break;
                }
                if (parent->children[i]->kind == SyntaxKind::TokenColon) break;
            }
            if (!is_name) return false;

            // Find enclosing struct
            SyntaxNode* struct_node = parent->parent;
            if (struct_node && struct_node->kind == SyntaxKind::NodeStructDecl) {
                for (u32 i = 0; i < struct_node->children.size(); i++) {
                    if (struct_node->children[i]->kind == SyntaxKind::TokenIdentifier) {
                        StringView struct_name = struct_node->children[i]->token.text();
                        return struct_name == StringView(target.qualifier.data(), target.qualifier.size());
                    }
                }
            }
            return false;
        }

        // Field initializer in struct literal: NodeFieldInit
        if (parent->kind == SyntaxKind::NodeFieldInit) {
            bool is_field_name = parent->children.size() > 0 && parent->children[0] == candidate;
            if (!is_field_name) return false;

            // Walk up to struct literal
            SyntaxNode* struct_literal = parent->parent;
            if (struct_literal && struct_literal->kind == SyntaxKind::NodeStructLiteralExpr) {
                for (u32 i = 0; i < struct_literal->children.size(); i++) {
                    if (struct_literal->children[i]->kind == SyntaxKind::TokenIdentifier) {
                        StringView struct_name = struct_literal->children[i]->token.text();
                        // Walk inheritance chain
                        StringView current_type = struct_name;
                        u32 depth = 0;
                        while (!current_type.empty() && depth < 16) {
                            if (current_type == StringView(target.qualifier.data(), target.qualifier.size()) &&
                                index.find_field(current_type, candidate_text)) {
                                return true;
                            }
                            current_type = index.find_struct_parent(current_type);
                            depth++;
                        }
                        break;
                    }
                }
            }
            return false;
        }

        return false;
    }

    case SymbolCategory::Local:
    case SymbolCategory::Parameter: {
        // Locals/params: must be in the same enclosing function in the same file
        if (file_uri != StringView(target.enclosing_uri.data(), target.enclosing_uri.size())) {
            return false;
        }

        SyntaxNode* enclosing_fn = find_enclosing_function(candidate);
        if (!enclosing_fn) return false;

        // Compare enclosing function range
        if (enclosing_fn->range.start != target.enclosing_range.start ||
            enclosing_fn->range.end != target.enclosing_range.end) {
            return false;
        }

        // Make sure candidate is not in a type annotation or field init name context
        if (parent && parent->kind == SyntaxKind::NodeTypeExpr) return false;
        if (parent && parent->kind == SyntaxKind::NodeFieldInit &&
            parent->children.size() > 0 && parent->children[0] == candidate) return false;
        if (parent && parent->kind == SyntaxKind::NodeFieldDecl) return false;

        // Skip struct/enum/trait/fun decl names
        if (parent && (parent->kind == SyntaxKind::NodeStructDecl ||
                       parent->kind == SyntaxKind::NodeEnumDecl ||
                       parent->kind == SyntaxKind::NodeTraitDecl ||
                       parent->kind == SyntaxKind::NodeFunDecl)) return false;

        return true;
    }

    case SymbolCategory::Constructor:
        // Not yet handled
        return false;
    }

    return false;
}

// A found reference location
struct ReferenceLocation {
    String uri;
    TextRange name_range;
};

// Find all references to a symbol across all files
static void find_all_references(const SymbolIdentity& target, const GlobalIndex& index,
                                 bool include_declaration,
                                 const Vector<OpenDocument>& open_documents,
                                 const tsl::robin_map<String, WorkspaceFile>& workspace_files,
                                 Vector<ReferenceLocation>& out_locations,
                                 LspAnalysisContext* analysis_context = nullptr) {
    // Collect all file URIs and their content
    // Use a map to avoid duplicates (open docs override workspace files)
    struct FileInfo {
        StringView uri;
        const char* content;
        u32 length;
    };
    tsl::robin_map<String, FileInfo> all_files;

    for (auto it = workspace_files.begin(); it != workspace_files.end(); ++it) {
        FileInfo info;
        info.uri = StringView(it->second.uri.data(), it->second.uri.size());
        info.content = it->second.content.data();
        info.length = it->second.content.size();
        all_files[it->first] = info;
    }
    for (u32 i = 0; i < open_documents.size(); i++) {
        String key(open_documents[i].uri.data(), open_documents[i].uri.size());
        FileInfo info;
        info.uri = StringView(open_documents[i].uri.data(), open_documents[i].uri.size());
        info.content = open_documents[i].content.data();
        info.length = open_documents[i].content.size();
        all_files[key] = info;
    }

    // For locals/params, only search the file containing the definition
    bool locals_only = (target.category == SymbolCategory::Local ||
                        target.category == SymbolCategory::Parameter);

    StringView target_name(target.name.data(), target.name.size());

    for (auto it = all_files.begin(); it != all_files.end(); ++it) {
        const FileInfo& file_info = it->second;

        // For locals, skip files that don't match
        if (locals_only &&
            file_info.uri != StringView(target.enclosing_uri.data(), target.enclosing_uri.size())) {
            continue;
        }

        // Parse the file
        BumpAllocator allocator(8192);
        Lexer lexer(file_info.content, file_info.length);
        LspParser parser(lexer, allocator);
        SyntaxTree tree = parser.parse();

        // Collect all identifier nodes matching the name
        Vector<SyntaxNode*> candidates;
        collect_identifiers(tree.root, target_name, candidates);

        // Filter candidates
        for (u32 j = 0; j < candidates.size(); j++) {
            if (is_reference_to_symbol(candidates[j], target, index, tree.root, file_info.uri, analysis_context)) {
                // Check if this is the declaration and if we should skip it
                if (!include_declaration) {
                    // Check if candidate is at a declaration site
                    SyntaxNode* parent = candidates[j]->parent;
                    bool is_decl = false;
                    if (parent) {
                        switch (target.category) {
                        case SymbolCategory::Function:
                            is_decl = (parent->kind == SyntaxKind::NodeFunDecl);
                            break;
                        case SymbolCategory::Struct:
                            is_decl = (parent->kind == SyntaxKind::NodeStructDecl);
                            break;
                        case SymbolCategory::Enum:
                            is_decl = (parent->kind == SyntaxKind::NodeEnumDecl);
                            break;
                        case SymbolCategory::Trait:
                            is_decl = (parent->kind == SyntaxKind::NodeTraitDecl);
                            break;
                        case SymbolCategory::Global:
                            is_decl = (parent->kind == SyntaxKind::NodeVarDecl &&
                                       find_enclosing_function(candidates[j]) == nullptr);
                            break;
                        case SymbolCategory::Method:
                            is_decl = (parent->kind == SyntaxKind::NodeMethodDecl);
                            break;
                        case SymbolCategory::Field:
                            is_decl = (parent->kind == SyntaxKind::NodeFieldDecl);
                            break;
                        case SymbolCategory::Local:
                            is_decl = (parent->kind == SyntaxKind::NodeVarDecl);
                            break;
                        case SymbolCategory::Parameter:
                            is_decl = (parent->kind == SyntaxKind::NodeParam);
                            break;
                        case SymbolCategory::Constructor:
                            is_decl = (parent->kind == SyntaxKind::NodeConstructorDecl);
                            break;
                        }
                    }
                    if (is_decl) continue;
                }

                ReferenceLocation loc;
                loc.uri = String(file_info.uri);
                loc.name_range = candidates[j]->range;
                out_locations.push_back(std::move(loc));
            }
        }
    }
}

void LspServer::handle_references(const JsonValue& params, i64 id) {
    const JsonValue* text_document = params.find("textDocument");
    if (!text_document || !text_document->is_object()) {
        m_transport.write_response(id, "[]");
        return;
    }

    const JsonValue* uri_val = text_document->find("uri");
    if (!uri_val || !uri_val->is_string()) {
        m_transport.write_response(id, "[]");
        return;
    }

    const JsonValue* position_val = params.find("position");
    if (!position_val || !position_val->is_object()) {
        m_transport.write_response(id, "[]");
        return;
    }

    StringView uri = uri_val->as_string();

    // Get the source content for this file (open doc or workspace file)
    u32 source_length = 0;
    const char* source = get_file_content(uri, source_length);
    if (!source) {
        m_transport.write_response(id, "[]");
        return;
    }

    const JsonValue* line_val = position_val->find("line");
    const JsonValue* char_val = position_val->find("character");
    if (!line_val || !line_val->is_int() || !char_val || !char_val->is_int()) {
        m_transport.write_response(id, "[]");
        return;
    }

    LspPosition cursor_pos;
    cursor_pos.line = static_cast<u32>(line_val->as_int());
    cursor_pos.character = static_cast<u32>(char_val->as_int());

    u32 byte_offset = lsp_position_to_offset(source, source_length, cursor_pos);

    // Re-parse to get fresh CST
    BumpAllocator allocator(8192);
    Lexer lexer(source, source_length);
    LspParser parser(lexer, allocator);
    SyntaxTree tree = parser.parse();

    SyntaxNode* node = find_node_at_offset(tree.root, byte_offset);
    if (!node) {
        m_transport.write_response(id, "[]");
        return;
    }

    // Identify the symbol at cursor
    SymbolIdentity identity;
    if (!identify_symbol_at_cursor(node, m_global_index, uri, identity, &m_analysis_context)) {
        m_transport.write_response(id, "[]");
        return;
    }

    // Check includeDeclaration from context
    bool include_declaration = true;
    const JsonValue* context_val = params.find("context");
    if (context_val && context_val->is_object()) {
        const JsonValue* include_decl_val = context_val->find("includeDeclaration");
        if (include_decl_val && include_decl_val->is_bool()) {
            include_declaration = include_decl_val->as_bool();
        }
    }

    // Find all references
    Vector<ReferenceLocation> locations;
    find_all_references(identity, m_global_index, include_declaration,
                         m_open_documents, m_workspace_files, locations,
                         &m_analysis_context);

    // Build Location[] response
    String result;
    JsonWriter writer(result);
    writer.write_start_array();

    for (u32 i = 0; i < locations.size(); i++) {
        const ReferenceLocation& loc = locations[i];

        // Get source for this file to convert byte offsets to LSP positions
        u32 file_length = 0;
        const char* file_source = get_file_content(
            StringView(loc.uri.data(), loc.uri.size()), file_length);
        if (!file_source) continue;

        LspRange lsp_range = text_range_to_lsp_range(file_source, file_length, loc.name_range);

        writer.write_start_object();
        writer.write_key_string("uri", StringView(loc.uri.data(), loc.uri.size()));
        writer.write_key("range");
        write_lsp_range(writer, lsp_range);
        writer.write_end_object();
    }

    writer.write_end_array();
    m_transport.write_response(id, StringView(result.data(), result.size()));
}

void LspServer::handle_rename(const JsonValue& params, i64 id) {
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

    const JsonValue* position_val = params.find("position");
    if (!position_val || !position_val->is_object()) {
        m_transport.write_error_response(id, -32602, "Missing position");
        return;
    }

    const JsonValue* new_name_val = params.find("newName");
    if (!new_name_val || !new_name_val->is_string()) {
        m_transport.write_error_response(id, -32602, "Missing newName");
        return;
    }

    StringView uri = uri_val->as_string();
    StringView new_name = new_name_val->as_string();

    u32 source_length = 0;
    const char* source = get_file_content(uri, source_length);
    if (!source) {
        m_transport.write_error_response(id, -32602, "File not found");
        return;
    }

    const JsonValue* line_val = position_val->find("line");
    const JsonValue* char_val = position_val->find("character");
    if (!line_val || !line_val->is_int() || !char_val || !char_val->is_int()) {
        m_transport.write_error_response(id, -32602, "Invalid position");
        return;
    }

    LspPosition cursor_pos;
    cursor_pos.line = static_cast<u32>(line_val->as_int());
    cursor_pos.character = static_cast<u32>(char_val->as_int());

    u32 byte_offset = lsp_position_to_offset(source, source_length, cursor_pos);

    BumpAllocator allocator(8192);
    Lexer lexer(source, source_length);
    LspParser parser(lexer, allocator);
    SyntaxTree tree = parser.parse();

    SyntaxNode* node = find_node_at_offset(tree.root, byte_offset);
    if (!node) {
        m_transport.write_response(id, "null");
        return;
    }

    SymbolIdentity identity;
    if (!identify_symbol_at_cursor(node, m_global_index, uri, identity, &m_analysis_context)) {
        m_transport.write_response(id, "null");
        return;
    }

    // Find all references (always include declaration for rename)
    Vector<ReferenceLocation> locations;
    find_all_references(identity, m_global_index, true,
                         m_open_documents, m_workspace_files, locations,
                         &m_analysis_context);

    if (locations.empty()) {
        m_transport.write_response(id, "null");
        return;
    }

    // Group locations by URI
    tsl::robin_map<String, Vector<ReferenceLocation*>> by_uri;
    for (u32 i = 0; i < locations.size(); i++) {
        by_uri[locations[i].uri].push_back(&locations[i]);
    }

    // Build WorkspaceEdit response
    String result;
    JsonWriter writer(result);
    writer.write_start_object();
    writer.write_key("changes");
    writer.write_start_object();

    for (auto it = by_uri.begin(); it != by_uri.end(); ++it) {
        // Get source for this file
        u32 file_length = 0;
        const char* file_source = get_file_content(
            StringView(it->first.data(), it->first.size()), file_length);
        if (!file_source) continue;

        writer.write_key(StringView(it->first.data(), it->first.size()));
        writer.write_start_array();

        for (u32 j = 0; j < it->second.size(); j++) {
            LspRange lsp_range = text_range_to_lsp_range(
                file_source, file_length, it->second[j]->name_range);

            writer.write_start_object();
            writer.write_key("range");
            write_lsp_range(writer, lsp_range);
            writer.write_key_string("newText", new_name);
            writer.write_end_object();
        }

        writer.write_end_array();
    }

    writer.write_end_object();
    writer.write_end_object();

    m_transport.write_response(id, StringView(result.data(), result.size()));
}

} // namespace rx
