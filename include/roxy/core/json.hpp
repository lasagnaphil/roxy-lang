#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/string.hpp"
#include "roxy/core/string_view.hpp"
#include "roxy/core/span.hpp"
#include "roxy/core/vector.hpp"
#include "roxy/core/bump_allocator.hpp"

#include <cassert>
#include <cstdlib>
#include <cstring>

namespace rx {

// --- Data Structures ---

enum class JsonType : u8 { Null, Bool, Int, Double, String, Array, Object };

struct JsonMember;

struct JsonValue {
    JsonType type;
    union {
        bool bool_value;
        i64 int_value;
        f64 double_value;
        StringView string_value;
        Span<JsonValue> array_value;
        Span<JsonMember> object_value;
    };

    JsonValue() : type(JsonType::Null), int_value(0) {}

    static JsonValue make_null() { JsonValue v; return v; }

    static JsonValue make_bool(bool b) {
        JsonValue v;
        v.type = JsonType::Bool;
        v.bool_value = b;
        return v;
    }

    static JsonValue make_int(i64 i) {
        JsonValue v;
        v.type = JsonType::Int;
        v.int_value = i;
        return v;
    }

    static JsonValue make_double(f64 d) {
        JsonValue v;
        v.type = JsonType::Double;
        v.double_value = d;
        return v;
    }

    static JsonValue make_string(StringView s) {
        JsonValue v;
        v.type = JsonType::String;
        v.string_value = s;
        return v;
    }

    static JsonValue make_array(Span<JsonValue> elems) {
        JsonValue v;
        v.type = JsonType::Array;
        v.array_value = elems;
        return v;
    }

    static JsonValue make_object(Span<JsonMember> members) {
        JsonValue v;
        v.type = JsonType::Object;
        v.object_value = members;
        return v;
    }

    bool is_null() const { return type == JsonType::Null; }
    bool is_bool() const { return type == JsonType::Bool; }
    bool is_int() const { return type == JsonType::Int; }
    bool is_double() const { return type == JsonType::Double; }
    bool is_string() const { return type == JsonType::String; }
    bool is_array() const { return type == JsonType::Array; }
    bool is_object() const { return type == JsonType::Object; }

    bool as_bool() const { assert(is_bool()); return bool_value; }
    i64 as_int() const { assert(is_int()); return int_value; }
    f64 as_double() const { assert(is_double()); return double_value; }
    StringView as_string() const { assert(is_string()); return string_value; }
    Span<JsonValue> as_array() const { assert(is_array()); return array_value; }
    Span<JsonMember> as_object() const { assert(is_object()); return object_value; }

    // Object key lookup (linear scan)
    const JsonValue* find(StringView key) const;
};

struct JsonMember {
    StringView key;
    JsonValue value;
};

struct JsonParseError {
    u32 offset;
    u32 line;
    u32 column;
    const char* message; // static string, no allocation
};

// --- SAX Handler (static dispatch via templates) ---
//
// Any struct with the following methods can be used as a handler:
//   bool on_null();
//   bool on_bool(bool value);
//   bool on_int(i64 value);
//   bool on_double(f64 value);
//   bool on_string(StringView value);
//   bool on_key(StringView key);
//   bool on_start_object();
//   bool on_end_object(u32 member_count);
//   bool on_start_array();
//   bool on_end_array(u32 element_count);
//
// Returning false aborts parsing.

// Default no-op handler. Inherit and override only the methods you care about.
struct JsonDefaultHandler {
    bool on_null() { return true; }
    bool on_bool(bool) { return true; }
    bool on_int(i64) { return true; }
    bool on_double(f64) { return true; }
    bool on_string(StringView) { return true; }
    bool on_key(StringView) { return true; }
    bool on_start_object() { return true; }
    bool on_end_object(u32) { return true; }
    bool on_start_array() { return true; }
    bool on_end_array(u32) { return true; }
};

// --- Parser ---

class JsonParser {
public:
    // source must be mutable (in-situ decoding). Must outlive any returned StringViews.
    template<typename Handler>
    bool parse(char* source, u32 length, Handler& handler);

    bool has_error() const { return m_has_error; }
    const JsonParseError& error() const { return m_error; }

private:
    char* m_source = nullptr;
    u32 m_length = 0;
    u32 m_current = 0;
    u32 m_line = 1;
    u32 m_line_start = 0;
    bool m_has_error = false;
    JsonParseError m_error = {};

    template<typename Handler>
    bool parse_value(Handler& handler);
    template<typename Handler>
    bool parse_object(Handler& handler);
    template<typename Handler>
    bool parse_array(Handler& handler);
    template<typename Handler>
    bool parse_number(Handler& handler);

    bool parse_string(StringView& out);

    char peek() const;
    char advance();
    bool match(char expected);
    void skip_whitespace();
    bool expect(char c, const char* context);
    void set_error(const char* message);

    // Decode a 4-digit hex escape (\uXXXX), returns codepoint or -1 on error
    i32 decode_hex4();
    // Encode a Unicode codepoint to UTF-8 in-place, returns bytes written
    u32 encode_utf8(char* dest, u32 codepoint);
};

// --- Template implementations (must be in header) ---

template<typename Handler>
bool JsonParser::parse(char* source, u32 length, Handler& handler) {
    m_source = source;
    m_length = length;
    m_current = 0;
    m_line = 1;
    m_line_start = 0;
    m_has_error = false;

    skip_whitespace();

    if (m_current >= m_length) {
        set_error("empty input");
        return false;
    }

    if (!parse_value(handler)) return false;

    skip_whitespace();
    if (m_current < m_length) {
        set_error("unexpected content after value");
        return false;
    }

    return true;
}

template<typename Handler>
bool JsonParser::parse_value(Handler& handler) {
    skip_whitespace();

    if (m_current >= m_length) {
        set_error("unexpected end of input");
        return false;
    }

    char c = peek();
    switch (c) {
    case 'n':
        if (m_current + 4 <= m_length &&
            m_source[m_current + 1] == 'u' &&
            m_source[m_current + 2] == 'l' &&
            m_source[m_current + 3] == 'l') {
            m_current += 4;
            return handler.on_null();
        }
        set_error("invalid token");
        return false;

    case 't':
        if (m_current + 4 <= m_length &&
            m_source[m_current + 1] == 'r' &&
            m_source[m_current + 2] == 'u' &&
            m_source[m_current + 3] == 'e') {
            m_current += 4;
            return handler.on_bool(true);
        }
        set_error("invalid token");
        return false;

    case 'f':
        if (m_current + 5 <= m_length &&
            m_source[m_current + 1] == 'a' &&
            m_source[m_current + 2] == 'l' &&
            m_source[m_current + 3] == 's' &&
            m_source[m_current + 4] == 'e') {
            m_current += 5;
            return handler.on_bool(false);
        }
        set_error("invalid token");
        return false;

    case '"': {
        StringView str;
        if (!parse_string(str)) return false;
        return handler.on_string(str);
    }

    case '{':
        return parse_object(handler);

    case '[':
        return parse_array(handler);

    default:
        if (c == '-' || (c >= '0' && c <= '9')) {
            return parse_number(handler);
        }
        set_error("unexpected character");
        return false;
    }
}

template<typename Handler>
bool JsonParser::parse_object(Handler& handler) {
    assert(peek() == '{');
    advance();

    if (!handler.on_start_object()) return false;

    skip_whitespace();
    if (m_current < m_length && peek() == '}') {
        advance();
        return handler.on_end_object(0);
    }

    u32 count = 0;
    for (;;) {
        skip_whitespace();
        if (m_current >= m_length || peek() != '"') {
            set_error("expected string key");
            return false;
        }

        StringView key;
        if (!parse_string(key)) return false;
        if (!handler.on_key(key)) return false;

        skip_whitespace();
        if (!expect(':', "object")) return false;

        if (!parse_value(handler)) return false;

        count++;

        skip_whitespace();
        if (m_current >= m_length) {
            set_error("unterminated object");
            return false;
        }

        if (peek() == '}') {
            advance();
            return handler.on_end_object(count);
        }

        if (!expect(',', "object")) return false;
    }
}

template<typename Handler>
bool JsonParser::parse_array(Handler& handler) {
    assert(peek() == '[');
    advance();

    if (!handler.on_start_array()) return false;

    skip_whitespace();
    if (m_current < m_length && peek() == ']') {
        advance();
        return handler.on_end_array(0);
    }

    u32 count = 0;
    for (;;) {
        if (!parse_value(handler)) return false;

        count++;

        skip_whitespace();
        if (m_current >= m_length) {
            set_error("unterminated array");
            return false;
        }

        if (peek() == ']') {
            advance();
            return handler.on_end_array(count);
        }

        if (!expect(',', "array")) return false;
    }
}

template<typename Handler>
bool JsonParser::parse_number(Handler& handler) {
    u32 start = m_current;
    bool is_float = false;

    // Optional minus
    if (m_current < m_length && m_source[m_current] == '-') m_current++;

    // Integer part
    if (m_current >= m_length) {
        set_error("unexpected end of number");
        return false;
    }

    if (m_source[m_current] == '0') {
        m_current++;
    } else if (m_source[m_current] >= '1' && m_source[m_current] <= '9') {
        while (m_current < m_length && m_source[m_current] >= '0' && m_source[m_current] <= '9') {
            m_current++;
        }
    } else {
        set_error("invalid number");
        return false;
    }

    // Fractional part
    if (m_current < m_length && m_source[m_current] == '.') {
        is_float = true;
        m_current++;
        if (m_current >= m_length || m_source[m_current] < '0' || m_source[m_current] > '9') {
            set_error("expected digit after decimal point");
            return false;
        }
        while (m_current < m_length && m_source[m_current] >= '0' && m_source[m_current] <= '9') {
            m_current++;
        }
    }

    // Exponent part
    if (m_current < m_length && (m_source[m_current] == 'e' || m_source[m_current] == 'E')) {
        is_float = true;
        m_current++;
        if (m_current < m_length && (m_source[m_current] == '+' || m_source[m_current] == '-')) {
            m_current++;
        }
        if (m_current >= m_length || m_source[m_current] < '0' || m_source[m_current] > '9') {
            set_error("expected digit in exponent");
            return false;
        }
        while (m_current < m_length && m_source[m_current] >= '0' && m_source[m_current] <= '9') {
            m_current++;
        }
    }

    // Temporarily null-terminate for strtoll/strtod
    u32 end = m_current;
    char saved = (end < m_length) ? m_source[end] : '\0';
    if (end < m_length) m_source[end] = '\0';

    if (is_float) {
        char* endptr;
        f64 value = strtod(m_source + start, &endptr);
        if (end < m_length) m_source[end] = saved;
        return handler.on_double(value);
    } else {
        char* endptr;
        i64 value = strtoll(m_source + start, &endptr, 10);
        if (end < m_length) m_source[end] = saved;
        return handler.on_int(value);
    }
}

// --- DOM Handler ---

class JsonDomHandler {
public:
    explicit JsonDomHandler(BumpAllocator& allocator)
        : m_allocator(allocator), m_has_root(false) {}

    JsonValue root() const { assert(m_has_root); return m_root; }
    bool has_root() const { return m_has_root; }

    bool on_null() {
        push_value(JsonValue::make_null());
        return true;
    }

    bool on_bool(bool value) {
        push_value(JsonValue::make_bool(value));
        return true;
    }

    bool on_int(i64 value) {
        push_value(JsonValue::make_int(value));
        return true;
    }

    bool on_double(f64 value) {
        push_value(JsonValue::make_double(value));
        return true;
    }

    bool on_string(StringView value) {
        push_value(JsonValue::make_string(value));
        return true;
    }

    bool on_key(StringView key) {
        m_key_stack.push_back(key);
        return true;
    }

    bool on_start_object() {
        m_nesting_stack.push_back(m_value_stack.size());
        return true;
    }

    bool on_end_object(u32 member_count) {
        u32 base = m_nesting_stack.back();
        m_nesting_stack.pop_back();

        Vector<JsonMember> members;
        members.reserve(member_count);
        u32 key_base = m_key_stack.size() - member_count;
        for (u32 i = 0; i < member_count; i++) {
            JsonMember member;
            member.key = m_key_stack[key_base + i];
            member.value = m_value_stack[base + i];
            members.push_back(member);
        }

        // Pop values and keys
        for (u32 i = 0; i < member_count; i++) {
            m_value_stack.pop_back();
            m_key_stack.pop_back();
        }

        Span<JsonMember> span = m_allocator.alloc_span(members);
        push_value(JsonValue::make_object(span));
        return true;
    }

    bool on_start_array() {
        m_nesting_stack.push_back(m_value_stack.size());
        return true;
    }

    bool on_end_array(u32 element_count) {
        u32 base = m_nesting_stack.back();
        m_nesting_stack.pop_back();

        Vector<JsonValue> elems;
        elems.reserve(element_count);
        for (u32 i = 0; i < element_count; i++) {
            elems.push_back(m_value_stack[base + i]);
        }

        for (u32 i = 0; i < element_count; i++) {
            m_value_stack.pop_back();
        }

        Span<JsonValue> span = m_allocator.alloc_span(elems);
        push_value(JsonValue::make_array(span));
        return true;
    }

private:
    void push_value(JsonValue value) {
        if (m_nesting_stack.empty()) {
            m_root = value;
            m_has_root = true;
        } else {
            m_value_stack.push_back(value);
        }
    }

    BumpAllocator& m_allocator;
    Vector<JsonValue> m_value_stack;
    Vector<StringView> m_key_stack;
    Vector<u32> m_nesting_stack; // base index into m_value_stack for each nesting level
    JsonValue m_root;
    bool m_has_root;
};

// --- Writer ---

class JsonWriter {
public:
    explicit JsonWriter(String& output);

    void write_null();
    void write_bool(bool value);
    void write_int(i64 value);
    void write_double(f64 value);
    void write_string(StringView value);

    void write_start_object();
    void write_key(StringView key);
    void write_end_object();

    void write_start_array();
    void write_end_array();

    // Key-value convenience
    void write_key_null(StringView key);
    void write_key_bool(StringView key, bool value);
    void write_key_int(StringView key, i64 value);
    void write_key_double(StringView key, f64 value);
    void write_key_string(StringView key, StringView value);

    // Serialize a DOM tree
    void write_value(const JsonValue& value);

private:
    String& m_output;

    struct Level {
        bool is_object;
        bool needs_comma;
        bool expecting_value; // after key, before value
    };
    Vector<Level> m_stack;

    void write_separator();
    void write_escaped_string(StringView value);
};

// --- Convenience Functions ---

// Parse JSON source into a DOM tree. source is modified in-place.
// source must outlive the returned JsonValue (StringViews borrow from it).
// allocator must outlive the returned JsonValue (Spans are allocated from it).
bool json_parse(char* source, u32 length, BumpAllocator& allocator,
                JsonValue& out_root, JsonParseError* out_error = nullptr);

// Serialize a JsonValue to a JSON string.
String json_stringify(const JsonValue& value);

// Serialize a JsonValue, appending to an existing string.
void json_stringify(const JsonValue& value, String& output);

} // namespace rx
