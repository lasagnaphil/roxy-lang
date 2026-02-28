#include "roxy/core/json.hpp"
#include "roxy/core/format.hpp"

#include <cmath>
#include <cstdio>
#include <cerrno>

namespace rx {

// --- JsonValue ---

const JsonValue* JsonValue::find(StringView key) const {
    assert(is_object());
    for (u32 i = 0; i < object_value.size(); i++) {
        if (object_value[i].key == key) {
            return &object_value[i].value;
        }
    }
    return nullptr;
}

// --- JsonParser (non-template methods) ---

char JsonParser::peek() const {
    if (m_current >= m_length) return '\0';
    return m_source[m_current];
}

char JsonParser::advance() {
    char c = m_source[m_current++];
    if (c == '\n') {
        m_line++;
        m_line_start = m_current;
    }
    return c;
}

bool JsonParser::match(char expected) {
    if (m_current >= m_length || m_source[m_current] != expected) return false;
    advance();
    return true;
}

void JsonParser::skip_whitespace() {
    while (m_current < m_length) {
        char c = m_source[m_current];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance();
        } else {
            break;
        }
    }
}

bool JsonParser::expect(char c, const char* context) {
    if (!match(c)) {
        set_error("expected character in context");
        return false;
    }
    return true;
}

void JsonParser::set_error(const char* message) {
    m_has_error = true;
    m_error.offset = m_current;
    m_error.line = m_line;
    m_error.column = m_current - m_line_start + 1;
    m_error.message = message;
}

bool JsonParser::parse_string(StringView& out) {
    if (m_current >= m_length || m_source[m_current] != '"') {
        set_error("expected '\"'");
        return false;
    }
    m_current++; // skip opening quote

    char* write_pos = m_source + m_current;
    char* start = write_pos;
    bool has_escape = false;

    while (m_current < m_length) {
        char c = m_source[m_current];

        if (c == '"') {
            m_current++; // skip closing quote
            if (has_escape) {
                out = StringView(start, (u32)(write_pos - start));
            } else {
                out = StringView(start, (u32)(write_pos - start));
            }
            return true;
        }

        if (c == '\\') {
            has_escape = true;
            m_current++;
            if (m_current >= m_length) {
                set_error("unterminated string escape");
                return false;
            }

            char esc = m_source[m_current];
            m_current++;

            switch (esc) {
            case '"':  *write_pos++ = '"'; break;
            case '\\': *write_pos++ = '\\'; break;
            case '/':  *write_pos++ = '/'; break;
            case 'b':  *write_pos++ = '\b'; break;
            case 'f':  *write_pos++ = '\f'; break;
            case 'n':  *write_pos++ = '\n'; break;
            case 'r':  *write_pos++ = '\r'; break;
            case 't':  *write_pos++ = '\t'; break;
            case 'u': {
                i32 codepoint = decode_hex4();
                if (codepoint < 0) return false;

                // Handle UTF-16 surrogate pairs
                if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
                    // High surrogate — expect \uXXXX low surrogate
                    if (m_current + 1 >= m_length ||
                        m_source[m_current] != '\\' ||
                        m_source[m_current + 1] != 'u') {
                        set_error("expected low surrogate");
                        return false;
                    }
                    m_current += 2; // skip \u
                    i32 low = decode_hex4();
                    if (low < 0) return false;
                    if (low < 0xDC00 || low > 0xDFFF) {
                        set_error("invalid low surrogate");
                        return false;
                    }
                    codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
                } else if (codepoint >= 0xDC00 && codepoint <= 0xDFFF) {
                    set_error("unexpected low surrogate");
                    return false;
                }

                u32 bytes = encode_utf8(write_pos, (u32)codepoint);
                write_pos += bytes;
                break;
            }
            default:
                set_error("invalid escape sequence");
                return false;
            }
        } else if ((u8)c < 0x20) {
            set_error("control character in string");
            return false;
        } else {
            *write_pos++ = c;
            m_current++;
        }
    }

    set_error("unterminated string");
    return false;
}

i32 JsonParser::decode_hex4() {
    if (m_current + 4 > m_length) {
        set_error("incomplete unicode escape");
        return -1;
    }

    i32 result = 0;
    for (int i = 0; i < 4; i++) {
        char c = m_source[m_current++];
        result <<= 4;
        if (c >= '0' && c <= '9') result += c - '0';
        else if (c >= 'a' && c <= 'f') result += c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') result += c - 'A' + 10;
        else {
            set_error("invalid hex digit in unicode escape");
            return -1;
        }
    }
    return result;
}

u32 JsonParser::encode_utf8(char* dest, u32 codepoint) {
    if (codepoint < 0x80) {
        dest[0] = (char)codepoint;
        return 1;
    } else if (codepoint < 0x800) {
        dest[0] = (char)(0xC0 | (codepoint >> 6));
        dest[1] = (char)(0x80 | (codepoint & 0x3F));
        return 2;
    } else if (codepoint < 0x10000) {
        dest[0] = (char)(0xE0 | (codepoint >> 12));
        dest[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        dest[2] = (char)(0x80 | (codepoint & 0x3F));
        return 3;
    } else {
        dest[0] = (char)(0xF0 | (codepoint >> 18));
        dest[1] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
        dest[2] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        dest[3] = (char)(0x80 | (codepoint & 0x3F));
        return 4;
    }
}

// --- JsonWriter ---

JsonWriter::JsonWriter(String& output) : m_output(output) {}

void JsonWriter::write_separator() {
    if (!m_stack.empty()) {
        Level& level = m_stack.back();
        if (level.expecting_value) {
            // After key:, no comma needed, just reset flag
            level.expecting_value = false;
            return;
        }
        if (level.needs_comma) {
            m_output.push_back(',');
        }
        level.needs_comma = true;
    }
}

void JsonWriter::write_escaped_string(StringView value) {
    m_output.push_back('"');
    for (u32 i = 0; i < value.size(); i++) {
        char c = value[i];
        switch (c) {
        case '"':  m_output.append("\\\"", 2); break;
        case '\\': m_output.append("\\\\", 2); break;
        case '\b': m_output.append("\\b", 2); break;
        case '\f': m_output.append("\\f", 2); break;
        case '\n': m_output.append("\\n", 2); break;
        case '\r': m_output.append("\\r", 2); break;
        case '\t': m_output.append("\\t", 2); break;
        default:
            if ((u8)c < 0x20) {
                // Control character: emit \u00XX
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)(u8)c);
                m_output.append(buf, 6);
            } else {
                m_output.push_back(c);
            }
            break;
        }
    }
    m_output.push_back('"');
}

void JsonWriter::write_null() {
    write_separator();
    m_output.append("null", 4);
}

void JsonWriter::write_bool(bool value) {
    write_separator();
    if (value) {
        m_output.append("true", 4);
    } else {
        m_output.append("false", 5);
    }
}

void JsonWriter::write_int(i64 value) {
    write_separator();
    char buf[32];
    i32 len = format_to(buf, sizeof(buf), "{}", value);
    m_output.append(buf, (u32)len);
}

void JsonWriter::write_double(f64 value) {
    write_separator();
    if (std::isnan(value) || std::isinf(value)) {
        m_output.append("null", 4);
        return;
    }
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%.17g", value);
    m_output.append(buf, (u32)len);
}

void JsonWriter::write_string(StringView value) {
    write_separator();
    write_escaped_string(value);
}

void JsonWriter::write_start_object() {
    write_separator();
    m_output.push_back('{');
    Level level;
    level.is_object = true;
    level.needs_comma = false;
    level.expecting_value = false;
    m_stack.push_back(level);
}

void JsonWriter::write_key(StringView key) {
    assert(!m_stack.empty() && m_stack.back().is_object);
    Level& level = m_stack.back();
    if (level.needs_comma) {
        m_output.push_back(',');
    }
    level.needs_comma = true;
    write_escaped_string(key);
    m_output.push_back(':');
    level.expecting_value = true;
}

void JsonWriter::write_end_object() {
    assert(!m_stack.empty() && m_stack.back().is_object);
    m_stack.pop_back();
    m_output.push_back('}');
}

void JsonWriter::write_start_array() {
    write_separator();
    m_output.push_back('[');
    Level level;
    level.is_object = false;
    level.needs_comma = false;
    level.expecting_value = false;
    m_stack.push_back(level);
}

void JsonWriter::write_end_array() {
    assert(!m_stack.empty() && !m_stack.back().is_object);
    m_stack.pop_back();
    m_output.push_back(']');
}

void JsonWriter::write_key_null(StringView key) {
    write_key(key);
    write_null();
}

void JsonWriter::write_key_bool(StringView key, bool value) {
    write_key(key);
    write_bool(value);
}

void JsonWriter::write_key_int(StringView key, i64 value) {
    write_key(key);
    write_int(value);
}

void JsonWriter::write_key_double(StringView key, f64 value) {
    write_key(key);
    write_double(value);
}

void JsonWriter::write_key_string(StringView key, StringView value) {
    write_key(key);
    write_string(value);
}

void JsonWriter::write_value(const JsonValue& value) {
    switch (value.type) {
    case JsonType::Null:
        write_null();
        break;
    case JsonType::Bool:
        write_bool(value.bool_value);
        break;
    case JsonType::Int:
        write_int(value.int_value);
        break;
    case JsonType::Double:
        write_double(value.double_value);
        break;
    case JsonType::String:
        write_string(value.string_value);
        break;
    case JsonType::Array:
        write_start_array();
        for (u32 i = 0; i < value.array_value.size(); i++) {
            write_value(value.array_value[i]);
        }
        write_end_array();
        break;
    case JsonType::Object:
        write_start_object();
        for (u32 i = 0; i < value.object_value.size(); i++) {
            write_key(value.object_value[i].key);
            write_value(value.object_value[i].value);
        }
        write_end_object();
        break;
    }
}

// --- Convenience Functions ---

bool json_parse(char* source, u32 length, BumpAllocator& allocator,
                JsonValue& out_root, JsonParseError* out_error) {
    JsonParser parser;
    JsonDomHandler handler(allocator);

    bool ok = parser.parse(source, length, handler);
    if (!ok) {
        if (out_error) {
            *out_error = parser.error();
        }
        return false;
    }

    if (!handler.has_root()) {
        if (out_error) {
            out_error->offset = 0;
            out_error->line = 1;
            out_error->column = 1;
            out_error->message = "no value parsed";
        }
        return false;
    }

    out_root = handler.root();
    return true;
}

String json_stringify(const JsonValue& value) {
    String output;
    json_stringify(value, output);
    return output;
}

void json_stringify(const JsonValue& value, String& output) {
    JsonWriter writer(output);
    writer.write_value(value);
}

} // namespace rx
