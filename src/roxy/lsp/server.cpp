#include "roxy/lsp/server.hpp"
#include "roxy/lsp/lsp_parser.hpp"
#include "roxy/lsp/indexer.hpp"
#include "roxy/core/bump_allocator.hpp"
#include "roxy/core/format.hpp"

#include <cstring>

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
        }
        writer.write_end_object();

        writer.write_key("serverInfo");
        writer.write_start_object();
        {
            writer.write_key_string("name", "roxy-lsp");
            writer.write_key_string("version", "0.1.0");
        }
        writer.write_end_object();
    }
    writer.write_end_object();

    m_transport.write_response(id, StringView(result.data(), result.size()));
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

OpenDocument* LspServer::find_document(StringView uri) {
    for (u32 i = 0; i < m_open_documents.size(); i++) {
        if (StringView(m_open_documents[i].uri.data(), m_open_documents[i].uri.size()) == uri) {
            return &m_open_documents[i];
        }
    }
    return nullptr;
}

} // namespace rx
