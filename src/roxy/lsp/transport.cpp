#include "roxy/lsp/transport.hpp"
#include "roxy/core/format.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace rx {

bool LspTransport::read_message(String& out_message) {
    // Parse headers: expect "Content-Length: N\r\n" followed by "\r\n"
    i32 content_length = -1;

    // Read headers line by line
    while (true) {
        // Read a header line (up to \r\n)
        String line;
        while (true) {
            int ch = fgetc(stdin);
            if (ch == EOF) return false;
            if (ch == '\r') {
                int next = fgetc(stdin);
                if (next == '\n') break;
                // Not \r\n, put it back
                if (next != EOF) ungetc(next, stdin);
            }
            line.push_back(static_cast<char>(ch));
        }

        // Empty line signals end of headers
        if (line.empty()) break;

        // Parse Content-Length
        const char* prefix = "Content-Length: ";
        u32 prefix_len = 16;
        if (line.size() > prefix_len && memcmp(line.data(), prefix, prefix_len) == 0) {
            content_length = atoi(line.data() + prefix_len);
        }
        // Other headers (Content-Type, etc.) are ignored
    }

    if (content_length < 0) {
        return false;
    }

    // Read exactly content_length bytes
    out_message.clear();
    out_message.resize(static_cast<u32>(content_length));
    u32 bytes_read = static_cast<u32>(fread(out_message.data(), 1, content_length, stdin));
    if (bytes_read != static_cast<u32>(content_length)) {
        return false;
    }

    return true;
}

void LspTransport::write_message(StringView body) {
    String header = format("Content-Length: {}\r\n\r\n", body.size());
    fwrite(header.data(), 1, header.size(), stdout);
    fwrite(body.data(), 1, body.size(), stdout);
    fflush(stdout);
}

void LspTransport::write_notification(StringView method, StringView params_json) {
    String body;
    body.append("{\"jsonrpc\":\"2.0\",\"method\":\"");
    body.append(method);
    body.append("\",\"params\":");
    body.append(params_json);
    body.push_back('}');
    write_message(StringView(body.data(), body.size()));
}

void LspTransport::write_response(i64 request_id, StringView result_json) {
    String id_str = format("{}", request_id);
    String body;
    body.append("{\"jsonrpc\":\"2.0\",\"id\":");
    body.append(StringView(id_str.data(), id_str.size()));
    body.append(",\"result\":");
    body.append(result_json);
    body.push_back('}');
    write_message(StringView(body.data(), body.size()));
}

void LspTransport::write_error_response(i64 request_id, i32 error_code, StringView error_message) {
    String id_str = format("{}", request_id);
    String code_str = format("{}", error_code);
    String body;
    body.append("{\"jsonrpc\":\"2.0\",\"id\":");
    body.append(StringView(id_str.data(), id_str.size()));
    body.append(",\"error\":{\"code\":");
    body.append(StringView(code_str.data(), code_str.size()));
    body.append(",\"message\":\"");
    // Simple escape for the error message (just escape quotes and backslashes)
    for (u32 i = 0; i < error_message.size(); i++) {
        char ch = error_message[i];
        if (ch == '"' || ch == '\\') body.push_back('\\');
        body.push_back(ch);
    }
    body.append("\"}}");
    write_message(StringView(body.data(), body.size()));
}

} // namespace rx
