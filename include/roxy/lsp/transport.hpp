#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/string.hpp"
#include "roxy/core/string_view.hpp"

namespace rx {

class LspTransport {
public:
    // Read a JSON-RPC message from stdin (Content-Length header + body).
    // Returns true if a message was read, false on EOF or error.
    bool read_message(String& out_message);

    // Write a raw JSON body to stdout with Content-Length header.
    void write_message(StringView body);

    // Write a JSON-RPC notification.
    void write_notification(StringView method, StringView params_json);

    // Write a JSON-RPC response.
    void write_response(i64 request_id, StringView result_json);

    // Write a JSON-RPC error response.
    void write_error_response(i64 request_id, i32 error_code, StringView error_message);
};

} // namespace rx
