#include "roxy/core/doctest/doctest.h"
#include "roxy/core/json.hpp"

using namespace rx;

// Helper: parse JSON source into a DOM value.
// source_buf must outlive the returned JsonValue (in-situ parsing).
static bool parse_json(const char* json, BumpAllocator& allocator, JsonValue& out,
                        String& source_buf, JsonParseError* error = nullptr) {
    u32 length = (u32)strlen(json);
    source_buf.reserve(length + 1);
    source_buf.resize(length + 1);
    memcpy(source_buf.data(), json, length);
    source_buf[length] = '\0';
    return json_parse(source_buf.data(), length, allocator, out, error);
}

// --- Scalar parsing ---

TEST_CASE("JSON parse - null") {
    BumpAllocator allocator(256);
    String buf;
    JsonValue root;
    REQUIRE(parse_json("null", allocator, root, buf));
    CHECK(root.is_null());
}

TEST_CASE("JSON parse - true") {
    BumpAllocator allocator(256);
    String buf;
    JsonValue root;
    REQUIRE(parse_json("true", allocator, root, buf));
    CHECK(root.is_bool());
    CHECK(root.as_bool() == true);
}

TEST_CASE("JSON parse - false") {
    BumpAllocator allocator(256);
    String buf;
    JsonValue root;
    REQUIRE(parse_json("false", allocator, root, buf));
    CHECK(root.is_bool());
    CHECK(root.as_bool() == false);
}

TEST_CASE("JSON parse - integer zero") {
    BumpAllocator allocator(256);
    String buf;
    JsonValue root;
    REQUIRE(parse_json("0", allocator, root, buf));
    CHECK(root.is_int());
    CHECK(root.as_int() == 0);
}

TEST_CASE("JSON parse - positive integer") {
    BumpAllocator allocator(256);
    String buf;
    JsonValue root;
    REQUIRE(parse_json("42", allocator, root, buf));
    CHECK(root.is_int());
    CHECK(root.as_int() == 42);
}

TEST_CASE("JSON parse - negative integer") {
    BumpAllocator allocator(256);
    String buf;
    JsonValue root;
    REQUIRE(parse_json("-123", allocator, root, buf));
    CHECK(root.is_int());
    CHECK(root.as_int() == -123);
}

TEST_CASE("JSON parse - float with decimal") {
    BumpAllocator allocator(256);
    String buf;
    JsonValue root;
    REQUIRE(parse_json("3.14", allocator, root, buf));
    CHECK(root.is_double());
    CHECK(root.as_double() == doctest::Approx(3.14));
}

TEST_CASE("JSON parse - float with exponent") {
    BumpAllocator allocator(256);
    String buf;
    JsonValue root;
    REQUIRE(parse_json("1.5e10", allocator, root, buf));
    CHECK(root.is_double());
    CHECK(root.as_double() == doctest::Approx(1.5e10));
}

TEST_CASE("JSON parse - float with negative exponent") {
    BumpAllocator allocator(256);
    String buf;
    JsonValue root;
    REQUIRE(parse_json("2.5E-3", allocator, root, buf));
    CHECK(root.is_double());
    CHECK(root.as_double() == doctest::Approx(2.5e-3));
}

TEST_CASE("JSON parse - simple string") {
    BumpAllocator allocator(256);
    String buf;
    JsonValue root;
    REQUIRE(parse_json("\"hello\"", allocator, root, buf));
    CHECK(root.is_string());
    CHECK(root.as_string() == "hello");
}

TEST_CASE("JSON parse - empty string") {
    BumpAllocator allocator(256);
    String buf;
    JsonValue root;
    REQUIRE(parse_json("\"\"", allocator, root, buf));
    CHECK(root.is_string());
    CHECK(root.as_string().size() == 0);
}

TEST_CASE("JSON parse - string with escape sequences") {
    BumpAllocator allocator(256);
    String buf;
    JsonValue root;
    REQUIRE(parse_json("\"line1\\nline2\\ttab\"", allocator, root, buf));
    CHECK(root.is_string());
    CHECK(root.as_string() == "line1\nline2\ttab");
}

TEST_CASE("JSON parse - string with escaped quotes") {
    BumpAllocator allocator(256);
    String buf;
    JsonValue root;
    REQUIRE(parse_json("\"say \\\"hi\\\"\"", allocator, root, buf));
    CHECK(root.is_string());
    CHECK(root.as_string() == "say \"hi\"");
}

TEST_CASE("JSON parse - string with unicode escape") {
    BumpAllocator allocator(256);
    String buf;
    JsonValue root;
    // \u0041 = 'A'
    REQUIRE(parse_json("\"\\u0041\"", allocator, root, buf));
    CHECK(root.is_string());
    CHECK(root.as_string() == "A");
}

TEST_CASE("JSON parse - string with surrogate pair") {
    BumpAllocator allocator(256);
    String buf;
    JsonValue root;
    // U+1F600 (grinning face) = \uD83D\uDE00
    REQUIRE(parse_json("\"\\uD83D\\uDE00\"", allocator, root, buf));
    CHECK(root.is_string());
    // UTF-8 encoding of U+1F600 is F0 9F 98 80
    CHECK(root.as_string().size() == 4);
    StringView s = root.as_string();
    CHECK((u8)s[0] == 0xF0);
    CHECK((u8)s[1] == 0x9F);
    CHECK((u8)s[2] == 0x98);
    CHECK((u8)s[3] == 0x80);
}

// --- Container parsing ---

TEST_CASE("JSON parse - empty object") {
    BumpAllocator allocator(256);
    String buf;
    JsonValue root;
    REQUIRE(parse_json("{}", allocator, root, buf));
    CHECK(root.is_object());
    CHECK(root.as_object().size() == 0);
}

TEST_CASE("JSON parse - empty array") {
    BumpAllocator allocator(256);
    String buf;
    JsonValue root;
    REQUIRE(parse_json("[]", allocator, root, buf));
    CHECK(root.is_array());
    CHECK(root.as_array().size() == 0);
}

TEST_CASE("JSON parse - simple object") {
    BumpAllocator allocator(1024);
    String buf;
    JsonValue root;
    REQUIRE(parse_json("{\"name\":\"alice\",\"age\":30}", allocator, root, buf));
    CHECK(root.is_object());
    CHECK(root.as_object().size() == 2);

    const JsonValue* name = root.find("name");
    REQUIRE(name != nullptr);
    CHECK(name->is_string());
    CHECK(name->as_string() == "alice");

    const JsonValue* age = root.find("age");
    REQUIRE(age != nullptr);
    CHECK(age->is_int());
    CHECK(age->as_int() == 30);
}

TEST_CASE("JSON parse - simple array") {
    BumpAllocator allocator(1024);
    String buf;
    JsonValue root;
    REQUIRE(parse_json("[1,2,3]", allocator, root, buf));
    CHECK(root.is_array());
    CHECK(root.as_array().size() == 3);
    CHECK(root.as_array()[0].as_int() == 1);
    CHECK(root.as_array()[1].as_int() == 2);
    CHECK(root.as_array()[2].as_int() == 3);
}

TEST_CASE("JSON parse - nested object") {
    BumpAllocator allocator(1024);
    String buf;
    JsonValue root;
    REQUIRE(parse_json("{\"a\":{\"b\":42}}", allocator, root, buf));
    CHECK(root.is_object());
    const JsonValue* a = root.find("a");
    REQUIRE(a != nullptr);
    CHECK(a->is_object());
    const JsonValue* b = a->find("b");
    REQUIRE(b != nullptr);
    CHECK(b->as_int() == 42);
}

TEST_CASE("JSON parse - nested array") {
    BumpAllocator allocator(1024);
    String buf;
    JsonValue root;
    REQUIRE(parse_json("[[1,2],[3,4]]", allocator, root, buf));
    CHECK(root.is_array());
    CHECK(root.as_array().size() == 2);
    CHECK(root.as_array()[0].as_array()[0].as_int() == 1);
    CHECK(root.as_array()[0].as_array()[1].as_int() == 2);
    CHECK(root.as_array()[1].as_array()[0].as_int() == 3);
    CHECK(root.as_array()[1].as_array()[1].as_int() == 4);
}

TEST_CASE("JSON parse - mixed nesting") {
    BumpAllocator allocator(1024);
    String buf;
    JsonValue root;
    REQUIRE(parse_json("{\"items\":[{\"id\":1},{\"id\":2}],\"count\":2}", allocator, root, buf));
    CHECK(root.is_object());

    const JsonValue* items = root.find("items");
    REQUIRE(items != nullptr);
    CHECK(items->is_array());
    CHECK(items->as_array().size() == 2);
    CHECK(items->as_array()[0].find("id")->as_int() == 1);
    CHECK(items->as_array()[1].find("id")->as_int() == 2);

    const JsonValue* count = root.find("count");
    REQUIRE(count != nullptr);
    CHECK(count->as_int() == 2);
}

TEST_CASE("JSON parse - whitespace handling") {
    BumpAllocator allocator(1024);
    String buf;
    JsonValue root;
    REQUIRE(parse_json("  {  \"key\"  :  \"value\"  }  ", allocator, root, buf));
    CHECK(root.is_object());
    CHECK(root.find("key")->as_string() == "value");
}

// --- In-situ verification ---

TEST_CASE("JSON parse - in-situ string points into source buffer") {
    char source[] = "{\"hello\":\"world\"}";
    u32 length = (u32)strlen(source);
    BumpAllocator allocator(256);
    JsonValue root;
    REQUIRE(json_parse(source, length, allocator, root, nullptr));

    // StringView should point into the source buffer
    const JsonValue* val = root.find("hello");
    REQUIRE(val != nullptr);
    CHECK(val->as_string().data() >= source);
    CHECK(val->as_string().data() < source + length);
}

// --- DOM access ---

TEST_CASE("JSON parse - find returns nullptr for missing key") {
    BumpAllocator allocator(256);
    String buf;
    JsonValue root;
    REQUIRE(parse_json("{\"a\":1}", allocator, root, buf));
    CHECK(root.find("b") == nullptr);
}

TEST_CASE("JSON parse - object with null and bool values") {
    BumpAllocator allocator(1024);
    String buf;
    JsonValue root;
    REQUIRE(parse_json("{\"x\":null,\"y\":true,\"z\":false}", allocator, root, buf));
    CHECK(root.find("x")->is_null());
    CHECK(root.find("y")->as_bool() == true);
    CHECK(root.find("z")->as_bool() == false);
}

// --- SAX handler ---

TEST_CASE("JSON SAX - custom counting handler") {
    struct CountingHandler {
        u32 null_count = 0;
        u32 bool_count = 0;
        u32 int_count = 0;
        u32 double_count = 0;
        u32 string_count = 0;
        u32 key_count = 0;
        u32 object_count = 0;
        u32 array_count = 0;

        bool on_null() { null_count++; return true; }
        bool on_bool(bool) { bool_count++; return true; }
        bool on_int(i64) { int_count++; return true; }
        bool on_double(f64) { double_count++; return true; }
        bool on_string(StringView) { string_count++; return true; }
        bool on_key(StringView) { key_count++; return true; }
        bool on_start_object() { return true; }
        bool on_end_object(u32) { object_count++; return true; }
        bool on_start_array() { return true; }
        bool on_end_array(u32) { array_count++; return true; }
    };

    char source[] = "{\"a\":1,\"b\":\"hi\",\"c\":[null,true,3.14]}";
    u32 length = (u32)strlen(source);

    JsonParser parser;
    CountingHandler handler;
    REQUIRE(parser.parse(source, length, handler));

    CHECK(handler.null_count == 1);
    CHECK(handler.bool_count == 1);
    CHECK(handler.int_count == 1);
    CHECK(handler.double_count == 1);
    CHECK(handler.string_count == 1);
    CHECK(handler.key_count == 3);
    CHECK(handler.object_count == 1);
    CHECK(handler.array_count == 1);
}

TEST_CASE("JSON SAX - early abort") {
    // Handler that aborts on first int
    struct AbortHandler {
        i64 found_int = 0;
        bool on_null() { return true; }
        bool on_bool(bool) { return true; }
        bool on_int(i64 v) { found_int = v; return false; }
        bool on_double(f64) { return true; }
        bool on_string(StringView) { return true; }
        bool on_key(StringView) { return true; }
        bool on_start_object() { return true; }
        bool on_end_object(u32) { return true; }
        bool on_start_array() { return true; }
        bool on_end_array(u32) { return true; }
    };

    char source[] = "{\"a\":\"skip\",\"b\":42,\"c\":99}";
    u32 length = (u32)strlen(source);

    JsonParser parser;
    AbortHandler handler;
    CHECK(parser.parse(source, length, handler) == false);
    CHECK(handler.found_int == 42);
}

// --- Error cases ---

TEST_CASE("JSON parse error - unterminated string") {
    BumpAllocator allocator(256);
    String buf;
    JsonValue root;
    JsonParseError error;
    CHECK(parse_json("\"hello", allocator, root, buf, &error) == false);
    CHECK(error.message != nullptr);
}

TEST_CASE("JSON parse error - invalid escape") {
    BumpAllocator allocator(256);
    String buf;
    JsonValue root;
    JsonParseError error;
    CHECK(parse_json("\"\\q\"", allocator, root, buf, &error) == false);
}

TEST_CASE("JSON parse error - unexpected token") {
    BumpAllocator allocator(256);
    String buf;
    JsonValue root;
    JsonParseError error;
    CHECK(parse_json("@", allocator, root, buf, &error) == false);
}

TEST_CASE("JSON parse error - empty input") {
    BumpAllocator allocator(256);
    String buf;
    JsonValue root;
    JsonParseError error;
    CHECK(parse_json("", allocator, root, buf, &error) == false);
    CHECK(error.message != nullptr);
}

TEST_CASE("JSON parse error - trailing garbage") {
    BumpAllocator allocator(256);
    String buf;
    JsonValue root;
    JsonParseError error;
    CHECK(parse_json("42 garbage", allocator, root, buf, &error) == false);
}

TEST_CASE("JSON parse error - truncated true") {
    BumpAllocator allocator(256);
    String buf;
    JsonValue root;
    JsonParseError error;
    CHECK(parse_json("tru", allocator, root, buf, &error) == false);
}

TEST_CASE("JSON parse error - unterminated object") {
    BumpAllocator allocator(256);
    String buf;
    JsonValue root;
    JsonParseError error;
    CHECK(parse_json("{\"a\":1", allocator, root, buf, &error) == false);
}

TEST_CASE("JSON parse error - unterminated array") {
    BumpAllocator allocator(256);
    String buf;
    JsonValue root;
    JsonParseError error;
    CHECK(parse_json("[1,2", allocator, root, buf, &error) == false);
}

TEST_CASE("JSON parse error - unexpected low surrogate") {
    BumpAllocator allocator(256);
    String buf;
    JsonValue root;
    JsonParseError error;
    CHECK(parse_json("\"\\uDC00\"", allocator, root, buf, &error) == false);
}

// --- Writer ---

TEST_CASE("JSON write - null") {
    String output;
    JsonWriter writer(output);
    writer.write_null();
    CHECK(output == "null");
}

TEST_CASE("JSON write - bool") {
    String output;
    JsonWriter writer(output);
    writer.write_bool(true);
    CHECK(output == "true");

    output.clear();
    JsonWriter writer2(output);
    writer2.write_bool(false);
    CHECK(output == "false");
}

TEST_CASE("JSON write - int") {
    String output;
    JsonWriter writer(output);
    writer.write_int(42);
    CHECK(output == "42");

    output.clear();
    JsonWriter writer2(output);
    writer2.write_int(-123);
    CHECK(output == "-123");
}

TEST_CASE("JSON write - double") {
    String output;
    JsonWriter writer(output);
    writer.write_double(3.14);
    // %.17g format
    CHECK(output.size() > 0);
}

TEST_CASE("JSON write - NaN and Inf become null") {
    String output;
    JsonWriter writer(output);
    writer.write_double(NAN);
    CHECK(output == "null");

    output.clear();
    JsonWriter writer2(output);
    writer2.write_double(INFINITY);
    CHECK(output == "null");
}

TEST_CASE("JSON write - string") {
    String output;
    JsonWriter writer(output);
    writer.write_string("hello");
    CHECK(output == "\"hello\"");
}

TEST_CASE("JSON write - string with escapes") {
    String output;
    JsonWriter writer(output);
    writer.write_string("line1\nline2\ttab");
    CHECK(output == "\"line1\\nline2\\ttab\"");
}

TEST_CASE("JSON write - string with quotes") {
    String output;
    JsonWriter writer(output);
    writer.write_string("say \"hi\"");
    CHECK(output == "\"say \\\"hi\\\"\"");
}

TEST_CASE("JSON write - empty object") {
    String output;
    JsonWriter writer(output);
    writer.write_start_object();
    writer.write_end_object();
    CHECK(output == "{}");
}

TEST_CASE("JSON write - object with values") {
    String output;
    JsonWriter writer(output);
    writer.write_start_object();
    writer.write_key_string("name", "alice");
    writer.write_key_int("age", 30);
    writer.write_end_object();
    CHECK(output == "{\"name\":\"alice\",\"age\":30}");
}

TEST_CASE("JSON write - empty array") {
    String output;
    JsonWriter writer(output);
    writer.write_start_array();
    writer.write_end_array();
    CHECK(output == "[]");
}

TEST_CASE("JSON write - array with values") {
    String output;
    JsonWriter writer(output);
    writer.write_start_array();
    writer.write_int(1);
    writer.write_int(2);
    writer.write_int(3);
    writer.write_end_array();
    CHECK(output == "[1,2,3]");
}

TEST_CASE("JSON write - nested object") {
    String output;
    JsonWriter writer(output);
    writer.write_start_object();
    writer.write_key("inner");
    writer.write_start_object();
    writer.write_key_int("value", 42);
    writer.write_end_object();
    writer.write_end_object();
    CHECK(output == "{\"inner\":{\"value\":42}}");
}

TEST_CASE("JSON write - write_value for DOM tree") {
    BumpAllocator allocator(1024);
    String buf;
    JsonValue root;
    REQUIRE(parse_json("{\"a\":[1,2],\"b\":true}", allocator, root, buf));

    String output;
    JsonWriter writer(output);
    writer.write_value(root);
    CHECK(output == "{\"a\":[1,2],\"b\":true}");
}

// --- Round-trip ---

TEST_CASE("JSON round-trip") {
    const char* test_cases[] = {
        "null",
        "true",
        "false",
        "42",
        "-123",
        "\"hello\"",
        "\"\"",
        "[]",
        "{}",
        "[1,2,3]",
        "{\"a\":1,\"b\":\"hi\"}",
        "{\"x\":[1,[2,3]],\"y\":{\"z\":null}}",
        "[{\"id\":1},{\"id\":2}]",
    };

    for (const char* input : test_cases) {
        INFO("input: " << input);

        // Parse
        BumpAllocator allocator1(1024);
        String buf1;
        JsonValue root1;
        REQUIRE(parse_json(input, allocator1, root1, buf1));

        // Stringify
        String json1 = json_stringify(root1);

        // Parse again
        BumpAllocator allocator2(1024);
        JsonValue root2;
        String buf2;
        buf2.resize((u32)json1.size() + 1);
        memcpy(buf2.data(), json1.data(), json1.size());
        buf2[json1.size()] = '\0';
        REQUIRE(json_parse(buf2.data(), json1.size(), allocator2, root2, nullptr));

        // Stringify again
        String json2 = json_stringify(root2);

        // Should be identical
        CHECK(json1 == json2);
    }
}

// --- Edge cases ---

TEST_CASE("JSON parse - large integer near i64 limits") {
    BumpAllocator allocator(256);
    String buf;
    JsonValue root;
    REQUIRE(parse_json("9223372036854775807", allocator, root, buf));
    CHECK(root.is_int());
    CHECK(root.as_int() == 9223372036854775807LL);
}

TEST_CASE("JSON parse - large negative integer") {
    BumpAllocator allocator(256);
    String buf;
    JsonValue root;
    REQUIRE(parse_json("-9223372036854775807", allocator, root, buf));
    CHECK(root.is_int());
    CHECK(root.as_int() == -9223372036854775807LL);
}

TEST_CASE("JSON parse - deeply nested JSON") {
    // 50 levels of nesting
    String input;
    for (int i = 0; i < 50; i++) input.push_back('[');
    input.push_back('1');
    for (int i = 0; i < 50; i++) input.push_back(']');

    BumpAllocator allocator(4096);
    JsonValue root;
    String buf;
    buf.resize(input.size() + 1);
    memcpy(buf.data(), input.data(), input.size());
    buf[input.size()] = '\0';
    REQUIRE(json_parse(buf.data(), input.size(), allocator, root, nullptr));

    // Navigate down 50 levels
    const JsonValue* current = &root;
    for (int i = 0; i < 50; i++) {
        REQUIRE(current->is_array());
        REQUIRE(current->as_array().size() == 1);
        current = &current->as_array()[0];
    }
    CHECK(current->is_int());
    CHECK(current->as_int() == 1);
}

TEST_CASE("JSON parse - integer-only exponent is float") {
    BumpAllocator allocator(256);
    String buf;
    JsonValue root;
    REQUIRE(parse_json("1e2", allocator, root, buf));
    CHECK(root.is_double());
    CHECK(root.as_double() == doctest::Approx(100.0));
}

TEST_CASE("JSON parse - all value types in array") {
    BumpAllocator allocator(1024);
    String buf;
    JsonValue root;
    REQUIRE(parse_json("[null,true,false,42,3.14,\"hi\",[],{}]", allocator, root, buf));
    CHECK(root.is_array());
    Span<JsonValue> arr = root.as_array();
    CHECK(arr.size() == 8);
    CHECK(arr[0].is_null());
    CHECK(arr[1].as_bool() == true);
    CHECK(arr[2].as_bool() == false);
    CHECK(arr[3].as_int() == 42);
    CHECK(arr[4].is_double());
    CHECK(arr[5].as_string() == "hi");
    CHECK(arr[6].is_array());
    CHECK(arr[7].is_object());
}

TEST_CASE("JSON write - key convenience helpers") {
    String output;
    JsonWriter writer(output);
    writer.write_start_object();
    writer.write_key_null("n");
    writer.write_key_bool("b", true);
    writer.write_key_double("d", 1.5);
    writer.write_end_object();
    // Verify it parses back correctly
    BumpAllocator allocator(1024);
    JsonValue root;
    String buf;
    buf.resize(output.size() + 1);
    memcpy(buf.data(), output.data(), output.size());
    buf[output.size()] = '\0';
    REQUIRE(json_parse(buf.data(), output.size(), allocator, root, nullptr));
    CHECK(root.find("n")->is_null());
    CHECK(root.find("b")->as_bool() == true);
    CHECK(root.find("d")->is_double());
}

TEST_CASE("JSON parse - string with backslash") {
    BumpAllocator allocator(256);
    String buf;
    JsonValue root;
    REQUIRE(parse_json("\"a\\\\b\"", allocator, root, buf));
    CHECK(root.as_string() == "a\\b");
}

TEST_CASE("JSON parse - string with slash") {
    BumpAllocator allocator(256);
    String buf;
    JsonValue root;
    REQUIRE(parse_json("\"a\\/b\"", allocator, root, buf));
    CHECK(root.as_string() == "a/b");
}

TEST_CASE("JSON parse - unicode 2-byte sequence") {
    BumpAllocator allocator(256);
    String buf;
    JsonValue root;
    // \u00E9 = é (2-byte UTF-8: C3 A9)
    REQUIRE(parse_json("\"\\u00E9\"", allocator, root, buf));
    CHECK(root.as_string().size() == 2);
    StringView s = root.as_string();
    CHECK((u8)s[0] == 0xC3);
    CHECK((u8)s[1] == 0xA9);
}

TEST_CASE("JSON parse - unicode 3-byte sequence") {
    BumpAllocator allocator(256);
    String buf;
    JsonValue root;
    // \u4E16 = 世 (3-byte UTF-8: E4 B8 96)
    REQUIRE(parse_json("\"\\u4E16\"", allocator, root, buf));
    CHECK(root.as_string().size() == 3);
    StringView s = root.as_string();
    CHECK((u8)s[0] == 0xE4);
    CHECK((u8)s[1] == 0xB8);
    CHECK((u8)s[2] == 0x96);
}

TEST_CASE("JSON write - control character escaping") {
    String output;
    JsonWriter writer(output);
    StringView sv("\x01\x1f", 2);
    writer.write_string(sv);
    CHECK(output == "\"\\u0001\\u001f\"");
}
