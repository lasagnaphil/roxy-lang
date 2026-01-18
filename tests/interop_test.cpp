#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "roxy/core/doctest/doctest.h"

#include "roxy/core/bump_allocator.hpp"
#include "roxy/shared/lexer.hpp"
#include "roxy/compiler/parser.hpp"
#include "roxy/compiler/semantic.hpp"
#include "roxy/compiler/ssa_ir.hpp"
#include "roxy/compiler/ir_builder.hpp"
#include "roxy/compiler/lowering.hpp"
#include "roxy/vm/vm.hpp"
#include "roxy/vm/interpreter.hpp"
#include "roxy/vm/natives.hpp"
#include "roxy/vm/binding/interop.hpp"

#include <cmath>

using namespace rx;

// ============================================================================
// Simple C++ functions to bind
// ============================================================================

// Simple integer functions
i32 my_add(i32 a, i32 b) { return a + b; }
i32 my_multiply(i32 a, i32 b) { return a * b; }
i32 my_abs(i32 x) { return x < 0 ? -x : x; }
i32 my_negate(i32 x) { return -x; }
i32 my_square(i32 x) { return x * x; }

// Boolean functions
bool my_is_positive(i32 x) { return x > 0; }
bool my_is_even(i32 x) { return x % 2 == 0; }
bool my_and(bool a, bool b) { return a && b; }
bool my_not(bool b) { return !b; }

// Float functions
f64 my_sqrt(f64 x) { return std::sqrt(x); }
f64 my_add_f(f64 a, f64 b) { return a + b; }
f64 my_floor(f64 x) { return std::floor(x); }

// Void function
void my_void_func(i32 x) { (void)x; }

// ============================================================================
// Test Helpers
// ============================================================================

// Helper to compile and run with registry
// The registry should only contain automatically bound functions (no manual bindings
// that depend on types). Manual bindings will be skipped.
static Value compile_and_run_with_registry(const char* source, StringView func_name,
                                           NativeRegistry& registry,
                                           Span<Value> args = {}) {
    BumpAllocator allocator(8192);

    u32 len = 0;
    while (source[len]) len++;

    Lexer lexer(source, len);
    Parser parser(lexer, allocator);
    Program* program = parser.parse();

    if (!program || parser.has_error()) {
        return Value::make_null();
    }

    SemanticAnalyzer analyzer(allocator, &registry);
    if (!analyzer.analyze(program)) {
        return Value::make_null();
    }

    IRBuilder ir_builder(allocator, analyzer.types(), &registry);
    IRModule* ir_module = ir_builder.build(program);
    if (!ir_module) {
        return Value::make_null();
    }

    BytecodeBuilder bc_builder;
    BCModule* module = bc_builder.build(ir_module);
    if (!module) {
        return Value::make_null();
    }

    registry.apply_to_module(module);

    RoxyVM vm;
    vm_init(&vm);
    vm_load_module(&vm, module);

    if (!vm_call(&vm, func_name, args)) {
        vm_destroy(&vm);
        delete module;
        return Value::make_null();
    }

    Value result = vm_get_result(&vm);
    vm_destroy(&vm);
    delete module;
    return result;
}

// Helper for tests that need built-in natives (array, print)
// Creates everything internally with proper type cache management
static Value compile_and_run_with_builtins(const char* source, StringView func_name,
                                           Span<Value> args = {}) {
    BumpAllocator allocator(8192);
    TypeCache types(allocator);

    // Create registry and register built-in natives
    NativeRegistry registry(allocator, types);
    register_builtin_natives(registry);

    u32 len = 0;
    while (source[len]) len++;

    Lexer lexer(source, len);
    Parser parser(lexer, allocator);
    Program* program = parser.parse();

    if (!program || parser.has_error()) {
        return Value::make_null();
    }

    SemanticAnalyzer analyzer(allocator, &registry);
    if (!analyzer.analyze(program)) {
        return Value::make_null();
    }

    IRBuilder ir_builder(allocator, analyzer.types(), &registry);
    IRModule* ir_module = ir_builder.build(program);
    if (!ir_module) {
        return Value::make_null();
    }

    BytecodeBuilder bc_builder;
    BCModule* module = bc_builder.build(ir_module);
    if (!module) {
        return Value::make_null();
    }

    registry.apply_to_module(module);

    RoxyVM vm;
    vm_init(&vm);
    vm_load_module(&vm, module);

    if (!vm_call(&vm, func_name, args)) {
        vm_destroy(&vm);
        delete module;
        return Value::make_null();
    }

    Value result = vm_get_result(&vm);
    vm_destroy(&vm);
    delete module;
    return result;
}

// ============================================================================
// Type Traits Tests
// ============================================================================

TEST_CASE("Interop - Type traits") {
    BumpAllocator allocator(1024);
    TypeCache types(allocator);

    SUBCASE("Primitive type mapping") {
        CHECK(RoxyType<void>::get(types) == types.void_type());
        CHECK(RoxyType<bool>::get(types) == types.bool_type());
        CHECK(RoxyType<i8>::get(types) == types.i8_type());
        CHECK(RoxyType<i16>::get(types) == types.i16_type());
        CHECK(RoxyType<i32>::get(types) == types.i32_type());
        CHECK(RoxyType<i64>::get(types) == types.i64_type());
        CHECK(RoxyType<u8>::get(types) == types.u8_type());
        CHECK(RoxyType<u16>::get(types) == types.u16_type());
        CHECK(RoxyType<u32>::get(types) == types.u32_type());
        CHECK(RoxyType<u64>::get(types) == types.u64_type());
        CHECK(RoxyType<f32>::get(types) == types.f32_type());
        CHECK(RoxyType<f64>::get(types) == types.f64_type());
    }

    SUBCASE("Value conversion - int") {
        Value v = Value::make_int(42);
        CHECK(RoxyType<i32>::from_value(v) == 42);
        CHECK(RoxyType<i64>::from_value(v) == 42);

        Value back = RoxyType<i32>::to_value(42);
        CHECK(back.is_int());
        CHECK(back.as_int == 42);
    }

    SUBCASE("Value conversion - bool") {
        Value v_true = Value::make_bool(true);
        Value v_false = Value::make_bool(false);

        CHECK(RoxyType<bool>::from_value(v_true) == true);
        CHECK(RoxyType<bool>::from_value(v_false) == false);

        Value back = RoxyType<bool>::to_value(true);
        CHECK(back.is_bool());
        CHECK(back.as_bool == true);
    }

    SUBCASE("Value conversion - float") {
        Value v = Value::make_float(3.14);
        CHECK(RoxyType<f64>::from_value(v) == doctest::Approx(3.14));
        CHECK(RoxyType<f32>::from_value(v) == doctest::Approx(3.14f));

        Value back = RoxyType<f64>::to_value(3.14);
        CHECK(back.is_float());
        CHECK(back.as_float == doctest::Approx(3.14));
    }
}

// ============================================================================
// Function Traits Tests
// ============================================================================

TEST_CASE("Interop - Function traits") {
    using Traits1 = FunctionTraits<decltype(&my_add)>;
    CHECK(std::is_same_v<Traits1::return_type, i32>);
    CHECK(Traits1::arity == 2);
    CHECK(std::is_same_v<Traits1::arg_type<0>, i32>);
    CHECK(std::is_same_v<Traits1::arg_type<1>, i32>);

    using Traits2 = FunctionTraits<decltype(&my_sqrt)>;
    CHECK(std::is_same_v<Traits2::return_type, f64>);
    CHECK(Traits2::arity == 1);
    CHECK(std::is_same_v<Traits2::arg_type<0>, f64>);

    using Traits3 = FunctionTraits<decltype(&my_is_positive)>;
    CHECK(std::is_same_v<Traits3::return_type, bool>);
    CHECK(Traits3::arity == 1);
    CHECK(std::is_same_v<Traits3::arg_type<0>, i32>);

    using Traits4 = FunctionTraits<decltype(&my_void_func)>;
    CHECK(std::is_same_v<Traits4::return_type, void>);
    CHECK(Traits4::arity == 1);
}

// ============================================================================
// Registry Tests
// ============================================================================

TEST_CASE("Interop - Registry basic") {
    BumpAllocator allocator(4096);
    TypeCache types(allocator);
    NativeRegistry registry(allocator, types);

    SUBCASE("Bind and lookup") {
        registry.bind<my_add>("add");
        registry.bind<my_multiply>("multiply");

        CHECK(registry.size() == 2);
        CHECK(registry.is_native(StringView("add", 3)));
        CHECK(registry.is_native(StringView("multiply", 8)));
        CHECK(!registry.is_native(StringView("unknown", 7)));

        CHECK(registry.get_index(StringView("add", 3)) == 0);
        CHECK(registry.get_index(StringView("multiply", 8)) == 1);
        CHECK(registry.get_index(StringView("unknown", 7)) == -1);
    }

    SUBCASE("Entry info") {
        registry.bind<my_add>("add");

        const auto& entry = registry.get_entry(0);
        CHECK(entry.name == StringView("add", 3));
        CHECK(entry.param_count == 2);
        CHECK(entry.is_manual == false);
        CHECK(entry.return_type_kind == NativeTypeKind::I32);
        CHECK(entry.param_type_kinds.size() == 2);
        CHECK(entry.param_type_kinds[0] == NativeTypeKind::I32);
        CHECK(entry.param_type_kinds[1] == NativeTypeKind::I32);
    }
}

// ============================================================================
// Integration Tests - Bound Functions
// ============================================================================

TEST_CASE("Interop - Bound integer functions") {
    BumpAllocator alloc(4096);
    TypeCache types(alloc);
    NativeRegistry registry(alloc, types);

    // Register bound functions
    registry.bind<my_add>("add");
    registry.bind<my_multiply>("mul");
    registry.bind<my_abs>("abs");
    registry.bind<my_negate>("neg");
    registry.bind<my_square>("square");

    SUBCASE("add(10, 20) = 30") {
        const char* source = R"(
            fun test(): i32 {
                return add(10, 20);
            }
        )";
        Value result = compile_and_run_with_registry(source, StringView("test", 4), registry);
        CHECK(result.is_int());
        CHECK(result.as_int == 30);
    }

    SUBCASE("mul(6, 7) = 42") {
        const char* source = R"(
            fun test(): i32 {
                return mul(6, 7);
            }
        )";
        Value result = compile_and_run_with_registry(source, StringView("test", 4), registry);
        CHECK(result.is_int());
        CHECK(result.as_int == 42);
    }

    SUBCASE("abs(-42) = 42") {
        const char* source = R"(
            fun test(): i32 {
                return abs(-42);
            }
        )";
        Value result = compile_and_run_with_registry(source, StringView("test", 4), registry);
        CHECK(result.is_int());
        CHECK(result.as_int == 42);
    }

    SUBCASE("neg(42) = -42") {
        const char* source = R"(
            fun test(): i32 {
                return neg(42);
            }
        )";
        Value result = compile_and_run_with_registry(source, StringView("test", 4), registry);
        CHECK(result.is_int());
        CHECK(result.as_int == -42);
    }

    SUBCASE("square(7) = 49") {
        const char* source = R"(
            fun test(): i32 {
                return square(7);
            }
        )";
        Value result = compile_and_run_with_registry(source, StringView("test", 4), registry);
        CHECK(result.is_int());
        CHECK(result.as_int == 49);
    }

    SUBCASE("Chained calls: square(add(3, 4)) = 49") {
        const char* source = R"(
            fun test(): i32 {
                return square(add(3, 4));
            }
        )";
        Value result = compile_and_run_with_registry(source, StringView("test", 4), registry);
        CHECK(result.is_int());
        CHECK(result.as_int == 49);
    }
}

TEST_CASE("Interop - Bound boolean functions") {
    BumpAllocator alloc(4096);
    TypeCache types(alloc);
    NativeRegistry registry(alloc, types);

    registry.bind<my_is_positive>("is_positive");
    registry.bind<my_is_even>("is_even");

    SUBCASE("is_positive(5) = true") {
        const char* source = R"(
            fun test(): bool {
                return is_positive(5);
            }
        )";
        Value result = compile_and_run_with_registry(source, StringView("test", 4), registry);
        CHECK(result.is_bool());
        CHECK(result.as_bool == true);
    }

    SUBCASE("is_positive(-5) = false") {
        const char* source = R"(
            fun test(): bool {
                return is_positive(-5);
            }
        )";
        Value result = compile_and_run_with_registry(source, StringView("test", 4), registry);
        CHECK(result.is_bool());
        CHECK(result.as_bool == false);
    }

    SUBCASE("is_even(4) = true") {
        const char* source = R"(
            fun test(): bool {
                return is_even(4);
            }
        )";
        Value result = compile_and_run_with_registry(source, StringView("test", 4), registry);
        CHECK(result.is_bool());
        CHECK(result.as_bool == true);
    }
}

TEST_CASE("Interop - Bound float functions") {
    BumpAllocator alloc(4096);
    TypeCache types(alloc);
    NativeRegistry registry(alloc, types);

    registry.bind<my_sqrt>("sqrt");
    registry.bind<my_add_f>("add_f");

    SUBCASE("sqrt(4.0) = 2.0") {
        const char* source = R"(
            fun test(): f64 {
                return sqrt(4.0);
            }
        )";
        Value result = compile_and_run_with_registry(source, StringView("test", 4), registry);
        CHECK(result.is_float());
        CHECK(result.as_float == doctest::Approx(2.0));
    }

    SUBCASE("sqrt(2.0) = 1.414...") {
        const char* source = R"(
            fun test(): f64 {
                return sqrt(2.0);
            }
        )";
        Value result = compile_and_run_with_registry(source, StringView("test", 4), registry);
        CHECK(result.is_float());
        CHECK(result.as_float == doctest::Approx(1.41421356));
    }

    SUBCASE("add_f(1.5, 2.5) = 4.0") {
        const char* source = R"(
            fun test(): f64 {
                return add_f(1.5, 2.5);
            }
        )";
        Value result = compile_and_run_with_registry(source, StringView("test", 4), registry);
        CHECK(result.is_float());
        CHECK(result.as_float == doctest::Approx(4.0));
    }
}

TEST_CASE("Interop - Void return function") {
    BumpAllocator alloc(4096);
    TypeCache types(alloc);
    NativeRegistry registry(alloc, types);

    registry.bind<my_void_func>("void_fn");

    const char* source = R"(
        fun test(): i32 {
            void_fn(42);
            return 1;
        }
    )";
    Value result = compile_and_run_with_registry(source, StringView("test", 4), registry);
    CHECK(result.is_int());
    CHECK(result.as_int == 1);
}

// ============================================================================
// Integration Tests - Built-in Natives via Registry
// ============================================================================

TEST_CASE("Interop - Built-in natives via registry") {
    SUBCASE("print works") {
        const char* source = R"(
            fun test(): i32 {
                print(42);
                return 0;
            }
        )";
        Value result = compile_and_run_with_builtins(source, StringView("test", 4));
        CHECK(result.is_int());
        CHECK(result.as_int == 0);
    }

    SUBCASE("array_new_int and array_len work") {
        const char* source = R"(
            fun test(): i32 {
                var arr: i32[] = array_new_int(5);
                return array_len(arr);
            }
        )";
        Value result = compile_and_run_with_builtins(source, StringView("test", 4));
        CHECK(result.is_int());
        CHECK(result.as_int == 5);
    }

    SUBCASE("Array operations work") {
        const char* source = R"(
            fun test(): i32 {
                var arr: i32[] = array_new_int(3);
                arr[0] = 10;
                arr[1] = 20;
                arr[2] = 30;
                return arr[0] + arr[1] + arr[2];
            }
        )";
        Value result = compile_and_run_with_builtins(source, StringView("test", 4));
        CHECK(result.is_int());
        CHECK(result.as_int == 60);
    }
}

// ============================================================================
// Mixed Functions Test
// ============================================================================

// Helper for mixed function tests - allows registering custom functions alongside builtins
template<typename BindFunc>
static Value compile_and_run_mixed(const char* source, StringView func_name, BindFunc bind_fn) {
    BumpAllocator allocator(8192);
    TypeCache types(allocator);

    // Create registry and register built-in natives
    NativeRegistry registry(allocator, types);
    register_builtin_natives(registry);

    // Let the caller add custom bindings
    bind_fn(registry);

    u32 len = 0;
    while (source[len]) len++;

    Lexer lexer(source, len);
    Parser parser(lexer, allocator);
    Program* program = parser.parse();

    if (!program || parser.has_error()) {
        return Value::make_null();
    }

    SemanticAnalyzer analyzer(allocator, &registry);
    if (!analyzer.analyze(program)) {
        return Value::make_null();
    }

    IRBuilder ir_builder(allocator, analyzer.types(), &registry);
    IRModule* ir_module = ir_builder.build(program);
    if (!ir_module) {
        return Value::make_null();
    }

    BytecodeBuilder bc_builder;
    BCModule* module = bc_builder.build(ir_module);
    if (!module) {
        return Value::make_null();
    }

    registry.apply_to_module(module);

    RoxyVM vm;
    vm_init(&vm);
    vm_load_module(&vm, module);

    if (!vm_call(&vm, func_name, {})) {
        vm_destroy(&vm);
        delete module;
        return Value::make_null();
    }

    Value result = vm_get_result(&vm);
    vm_destroy(&vm);
    delete module;
    return result;
}

TEST_CASE("Interop - Mixed bound and built-in functions") {
    const char* source = R"(
        fun test(): i32 {
            var arr: i32[] = array_new_int(3);
            arr[0] = abs(-5);
            arr[1] = square(4);
            arr[2] = array_len(arr);
            print(arr[0]);
            print(arr[1]);
            print(arr[2]);
            return arr[0] + arr[1] + arr[2];
        }
    )";

    Value result = compile_and_run_mixed(source, StringView("test", 4),
        [](NativeRegistry& reg) {
            reg.bind<my_square>("square");
            reg.bind<my_abs>("abs");
        });
    CHECK(result.is_int());
    CHECK(result.as_int == 5 + 16 + 3);  // abs(-5) + square(4) + len
}
