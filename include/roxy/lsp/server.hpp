#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/string.hpp"
#include "roxy/core/vector.hpp"
#include "roxy/core/json.hpp"
#include "roxy/core/tsl/robin_map.h"
#include "roxy/lsp/transport.hpp"
#include "roxy/lsp/protocol.hpp"
#include "roxy/lsp/indexer.hpp"
#include "roxy/lsp/global_index.hpp"

namespace rx {

struct OpenDocument {
    String uri;
    String content;
    i32 version;
    FileStubs stubs;
};

struct WorkspaceFile {
    String uri;
    String file_path;
    String content;
    FileStubs stubs;
};

class LspServer {
public:
    void run();

private:
    LspTransport m_transport;
    Vector<OpenDocument> m_open_documents;
    GlobalIndex m_global_index;
    String m_workspace_root;
    tsl::robin_map<String, WorkspaceFile> m_workspace_files;
    bool m_initialized = false;
    bool m_shutdown_requested = false;

    void dispatch(char* message_buf, u32 message_length);

    // Request handlers
    void handle_initialize(const JsonValue& params, i64 id);
    void handle_initialized();
    void handle_shutdown(i64 id);
    void handle_exit();
    void handle_did_open(const JsonValue& params);
    void handle_did_change(const JsonValue& params);
    void handle_did_close(const JsonValue& params);
    void handle_document_symbol(const JsonValue& params, i64 id);
    void handle_definition(const JsonValue& params, i64 id);
    void handle_completion(const JsonValue& params, i64 id);
    void handle_hover(const JsonValue& params, i64 id);

    // Core logic
    void parse_and_publish_diagnostics(OpenDocument& doc);
    void publish_diagnostics(StringView uri, const Vector<LspDiagnostic>& diagnostics);
    void collect_semantic_diagnostics(SyntaxNode* root, BumpAllocator& allocator,
                                      const char* source, u32 source_length,
                                      Vector<LspDiagnostic>& out_diagnostics);

    // Workspace helpers
    void scan_workspace();
    void index_workspace_file(const String& file_path);
    String file_path_to_uri(StringView file_path);
    String uri_to_file_path(StringView uri);

    // Get source text for any file (open doc or workspace file)
    const char* get_file_content(StringView uri, u32& out_length);

    // Document management
    OpenDocument* find_document(StringView uri);
};

} // namespace rx
