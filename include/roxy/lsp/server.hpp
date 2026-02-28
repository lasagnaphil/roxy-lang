#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/string.hpp"
#include "roxy/core/vector.hpp"
#include "roxy/core/json.hpp"
#include "roxy/lsp/transport.hpp"
#include "roxy/lsp/protocol.hpp"
#include "roxy/lsp/indexer.hpp"

namespace rx {

struct OpenDocument {
    String uri;
    String content;
    i32 version;
    FileStubs stubs;
};

class LspServer {
public:
    void run();

private:
    LspTransport m_transport;
    Vector<OpenDocument> m_open_documents;
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

    // Core logic
    void parse_and_publish_diagnostics(OpenDocument& doc);
    void publish_diagnostics(StringView uri, const Vector<LspDiagnostic>& diagnostics);

    // Document management
    OpenDocument* find_document(StringView uri);
};

} // namespace rx
