#include "roxy/core/doctest/doctest.h"

#include "roxy/core/bump_allocator.hpp"
#include "roxy/shared/lexer.hpp"
#include "roxy/compiler/parser.hpp"
#include "roxy/compiler/semantic.hpp"
#include "roxy/compiler/type_env.hpp"
#include "roxy/compiler/ssa_ir.hpp"
#include "roxy/compiler/ir_builder.hpp"
#include "roxy/compiler/lowering.hpp"
#include "roxy/compiler/module_registry.hpp"
#include "roxy/vm/vm.hpp"
#include "roxy/vm/interpreter.hpp"
#include "roxy/vm/natives.hpp"
#include "roxy/vm/binding/interop.hpp"

#include <cmath>
#include <cstring>

using namespace rx;

// ============================================================================
// Simple C++ functions to bind
// ============================================================================

// Simple integer functions (all take RoxyVM* as first parameter)
i32 my_add(RoxyVM* vm, i32 a, i32 b) { (void)vm; return a + b; }
i32 my_multiply(RoxyVM* vm, i32 a, i32 b) { (void)vm; return a * b; }
i32 my_abs(RoxyVM* vm, i32 x) { (void)vm; return x < 0 ? -x : x; }
i32 my_negate(RoxyVM* vm, i32 x) { (void)vm; return -x; }
i32 my_square(RoxyVM* vm, i32 x) { (void)vm; return x * x; }

// Boolean functions
bool my_is_positive(RoxyVM* vm, i32 x) { (void)vm; return x > 0; }
bool my_is_even(RoxyVM* vm, i32 x) { (void)vm; return x % 2 == 0; }
bool my_and(RoxyVM* vm, bool a, bool b) { (void)vm; return a && b; }
bool my_not(RoxyVM* vm, bool b) { (void)vm; return !b; }

// Float functions
f64 my_sqrt(RoxyVM* vm, f64 x) { (void)vm; return std::sqrt(x); }
f64 my_add_f(RoxyVM* vm, f64 a, f64 b) { (void)vm; return a + b; }
f64 my_floor(RoxyVM* vm, f64 x) { (void)vm; return std::floor(x); }

// Void function
void my_void_func(RoxyVM* vm, i32 x) { (void)vm; (void)x; }

// ============================================================================
// Test Helpers
// ============================================================================

// Helper to compile and run with registry
// The registry should only contain automatically bound functions (no manual bindings
// that depend on types). Manual bindings will be skipped.
static Value compile_and_run_with_registry(const char* source, StringView func_name,
                                           BumpAllocator& allocator, TypeEnv& type_env,
                                           NativeRegistry& registry,
                                           Span<Value> args = {}) {
    u32 len = 0;
    while (source[len]) len++;

    Lexer lexer(source, len);
    Parser parser(lexer, allocator);
    Program* program = parser.parse();

    if (!program || parser.has_error()) {
        return Value::make_null();
    }

    // Create module registry and SemanticAnalyzer with shared TypeEnv
    ModuleRegistry modules(allocator);
    SemanticAnalyzer analyzer(allocator, type_env, modules, &registry);

    if (!analyzer.analyze(program)) {
        return Value::make_null();
    }

    IRBuilder ir_builder(allocator, type_env, registry, analyzer.symbols(), modules);
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
    TypeEnv type_env(allocator);

    // Create registry and register built-in natives
    NativeRegistry registry(allocator, type_env.types());
    register_builtin_natives(registry);

    u32 len = 0;
    while (source[len]) len++;

    Lexer lexer(source, len);
    Parser parser(lexer, allocator);
    Program* program = parser.parse();

    if (!program || parser.has_error()) {
        return Value::make_null();
    }

    // Create module registry and register builtin module for prelude auto-import
    ModuleRegistry modules(allocator);
    modules.register_native_module(BUILTIN_MODULE_NAME, &registry, type_env.types());

    SemanticAnalyzer analyzer(allocator, type_env, modules);
    if (!analyzer.analyze(program)) {
        return Value::make_null();
    }

    IRBuilder ir_builder(allocator, type_env, registry, analyzer.symbols(), modules);
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
    CHECK(Traits1::arity == 3);  // RoxyVM* + 2 params
    CHECK(std::is_same_v<Traits1::arg_type<0>, RoxyVM*>);
    CHECK(std::is_same_v<Traits1::arg_type<1>, i32>);
    CHECK(std::is_same_v<Traits1::arg_type<2>, i32>);

    using Traits2 = FunctionTraits<decltype(&my_sqrt)>;
    CHECK(std::is_same_v<Traits2::return_type, f64>);
    CHECK(Traits2::arity == 2);  // RoxyVM* + 1 param
    CHECK(std::is_same_v<Traits2::arg_type<0>, RoxyVM*>);
    CHECK(std::is_same_v<Traits2::arg_type<1>, f64>);

    using Traits3 = FunctionTraits<decltype(&my_is_positive)>;
    CHECK(std::is_same_v<Traits3::return_type, bool>);
    CHECK(Traits3::arity == 2);  // RoxyVM* + 1 param
    CHECK(std::is_same_v<Traits3::arg_type<0>, RoxyVM*>);
    CHECK(std::is_same_v<Traits3::arg_type<1>, i32>);

    using Traits4 = FunctionTraits<decltype(&my_void_func)>;
    CHECK(std::is_same_v<Traits4::return_type, void>);
    CHECK(Traits4::arity == 2);  // RoxyVM* + 1 param
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
        CHECK(registry.is_native("add"));
        CHECK(registry.is_native("multiply"));
        CHECK(!registry.is_native("unknown"));

        CHECK(registry.get_index("add") == 0);
        CHECK(registry.get_index("multiply") == 1);
        CHECK(registry.get_index("unknown") == -1);
    }

    SUBCASE("Entry info") {
        registry.bind<my_add>("add");

        const auto& entry = registry.get_entry(0);
        CHECK(entry.name == "add");
        CHECK(entry.param_count == 2);
        CHECK(entry.type_info_mode == NativeTypeInfoMode::Resolver);
        // Resolver mode: types resolved via resolver functions
        CHECK(entry.return_resolver(types) == types.i32_type());
        CHECK(entry.param_resolvers.size() == 2);
        CHECK(entry.param_resolvers[0](types) == types.i32_type());
        CHECK(entry.param_resolvers[1](types) == types.i32_type());
    }
}

// ============================================================================
// Integration Tests - Bound Functions
// ============================================================================

TEST_CASE("Interop - Bound integer functions") {
    BumpAllocator alloc(4096);
    TypeEnv type_env(alloc);
    NativeRegistry registry(alloc, type_env.types());

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
        Value result = compile_and_run_with_registry(source, "test", alloc, type_env, registry);
        CHECK(result.is_int());
        CHECK(result.as_int == 30);
    }

    SUBCASE("mul(6, 7) = 42") {
        const char* source = R"(
            fun test(): i32 {
                return mul(6, 7);
            }
        )";
        Value result = compile_and_run_with_registry(source, "test", alloc, type_env, registry);
        CHECK(result.is_int());
        CHECK(result.as_int == 42);
    }

    SUBCASE("abs(-42) = 42") {
        const char* source = R"(
            fun test(): i32 {
                return abs(-42);
            }
        )";
        Value result = compile_and_run_with_registry(source, "test", alloc, type_env, registry);
        CHECK(result.is_int());
        CHECK(result.as_int == 42);
    }

    SUBCASE("neg(42) = -42") {
        const char* source = R"(
            fun test(): i32 {
                return neg(42);
            }
        )";
        Value result = compile_and_run_with_registry(source, "test", alloc, type_env, registry);
        CHECK(result.is_int());
        CHECK(result.as_int == -42);
    }

    SUBCASE("square(7) = 49") {
        const char* source = R"(
            fun test(): i32 {
                return square(7);
            }
        )";
        Value result = compile_and_run_with_registry(source, "test", alloc, type_env, registry);
        CHECK(result.is_int());
        CHECK(result.as_int == 49);
    }

    SUBCASE("Chained calls: square(add(3, 4)) = 49") {
        const char* source = R"(
            fun test(): i32 {
                return square(add(3, 4));
            }
        )";
        Value result = compile_and_run_with_registry(source, "test", alloc, type_env, registry);
        CHECK(result.is_int());
        CHECK(result.as_int == 49);
    }
}

TEST_CASE("Interop - Bound boolean functions") {
    BumpAllocator alloc(4096);
    TypeEnv type_env(alloc);
    NativeRegistry registry(alloc, type_env.types());

    registry.bind<my_is_positive>("is_positive");
    registry.bind<my_is_even>("is_even");

    SUBCASE("is_positive(5) = true") {
        const char* source = R"(
            fun test(): bool {
                return is_positive(5);
            }
        )";
        Value result = compile_and_run_with_registry(source, "test", alloc, type_env, registry);
        // With untyped registers, bools are 0 or 1
        CHECK(result.as_u64() == 1);  // true
    }

    SUBCASE("is_positive(-5) = false") {
        const char* source = R"(
            fun test(): bool {
                return is_positive(-5);
            }
        )";
        Value result = compile_and_run_with_registry(source, "test", alloc, type_env, registry);
        // With untyped registers, bools are 0 or 1
        CHECK(result.as_u64() == 0);  // false
    }

    SUBCASE("is_even(4) = true") {
        const char* source = R"(
            fun test(): bool {
                return is_even(4);
            }
        )";
        Value result = compile_and_run_with_registry(source, "test", alloc, type_env, registry);
        // With untyped registers, bools are 0 or 1
        CHECK(result.as_u64() == 1);  // true
    }
}

TEST_CASE("Interop - Bound float functions") {
    BumpAllocator alloc(4096);
    TypeEnv type_env(alloc);
    NativeRegistry registry(alloc, type_env.types());

    registry.bind<my_sqrt>("sqrt");
    registry.bind<my_add_f>("add_f");

    SUBCASE("sqrt(4.0) = 2.0") {
        const char* source = R"(
            fun test(): f64 {
                return sqrt(4.0);
            }
        )";
        Value result = compile_and_run_with_registry(source, "test", alloc, type_env, registry);
        // With untyped registers, interpret result bits as float
        Value float_result = Value::float_from_u64(result.as_u64());
        CHECK(float_result.as_float == doctest::Approx(2.0));
    }

    SUBCASE("sqrt(2.0) = 1.414...") {
        const char* source = R"(
            fun test(): f64 {
                return sqrt(2.0);
            }
        )";
        Value result = compile_and_run_with_registry(source, "test", alloc, type_env, registry);
        // With untyped registers, interpret result bits as float
        Value float_result = Value::float_from_u64(result.as_u64());
        CHECK(float_result.as_float == doctest::Approx(1.41421356));
    }

    SUBCASE("add_f(1.5, 2.5) = 4.0") {
        const char* source = R"(
            fun test(): f64 {
                return add_f(1.5, 2.5);
            }
        )";
        Value result = compile_and_run_with_registry(source, "test", alloc, type_env, registry);
        // With untyped registers, interpret result bits as float
        Value float_result = Value::float_from_u64(result.as_u64());
        CHECK(float_result.as_float == doctest::Approx(4.0));
    }
}

TEST_CASE("Interop - Void return function") {
    BumpAllocator alloc(4096);
    TypeEnv type_env(alloc);
    NativeRegistry registry(alloc, type_env.types());

    registry.bind<my_void_func>("void_fn");

    const char* source = R"(
        fun test(): i32 {
            void_fn(42);
            return 1;
        }
    )";
    Value result = compile_and_run_with_registry(source, "test", alloc, type_env, registry);
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
                print(f"{42}");
                return 0;
            }
        )";
        Value result = compile_and_run_with_builtins(source, "test");
        CHECK(result.is_int());
        CHECK(result.as_int == 0);
    }

    SUBCASE("List construction and len work") {
        const char* source = R"(
            fun test(): i32 {
                var lst: List<i32> = List<i32>();
                lst.push(1);
                lst.push(2);
                lst.push(3);
                lst.push(4);
                lst.push(5);
                return lst.len();
            }
        )";
        Value result = compile_and_run_with_builtins(source, "test");
        CHECK(result.is_int());
        CHECK(result.as_int == 5);
    }

    SUBCASE("List operations work") {
        const char* source = R"(
            fun test(): i32 {
                var lst: List<i32> = List<i32>();
                lst.push(10);
                lst.push(20);
                lst.push(30);
                return lst[0] + lst[1] + lst[2];
            }
        )";
        Value result = compile_and_run_with_builtins(source, "test");
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
    TypeEnv type_env(allocator);

    // Create registry and register built-in natives
    NativeRegistry registry(allocator, type_env.types());
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

    // Create module registry and register builtin module for prelude auto-import
    ModuleRegistry modules(allocator);
    modules.register_native_module(BUILTIN_MODULE_NAME, &registry, type_env.types());

    SemanticAnalyzer analyzer(allocator, type_env, modules);
    if (!analyzer.analyze(program)) {
        return Value::make_null();
    }

    IRBuilder ir_builder(allocator, type_env, registry, analyzer.symbols(), modules);
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
            var lst: List<i32> = List<i32>();
            lst.push(abs(-5));
            lst.push(square(4));
            lst.push(lst.len());
            print(f"{lst[0]}");
            print(f"{lst[1]}");
            print(f"{lst[2]}");
            return lst[0] + lst[1] + lst[2];
        }
    )";

    Value result = compile_and_run_mixed(source, "test",
        [](NativeRegistry& reg) {
            reg.bind<my_square>("square");
            reg.bind<my_abs>("abs");
        });
    CHECK(result.is_int());
    CHECK(result.as_int == 5 + 16 + 2);  // abs(-5) + square(4) + len(at time of push)
}

// ============================================================================
// Native Struct and Method Binding Tests
// ============================================================================

// C++ struct to bind as a native struct
struct CppPoint { i32 x, y; };

// Free functions acting as methods (RoxyVM* first, self pointer second)
i32 point_sum(RoxyVM* vm, CppPoint* self) { (void)vm; return self->x + self->y; }
i32 point_diff(RoxyVM* vm, CppPoint* self) { (void)vm; return self->x - self->y; }
bool point_is_origin(RoxyVM* vm, CppPoint* self) { (void)vm; return self->x == 0 && self->y == 0; }
i32 point_scaled_sum(RoxyVM* vm, CppPoint* self, i32 scale) { (void)vm; return (self->x + self->y) * scale; }
i32 point_weighted(RoxyVM* vm, CppPoint* self, i32 wx, i32 wy) { (void)vm; return self->x * wx + self->y * wy; }

TEST_CASE("Interop - Native struct with auto-bound method") {
    BumpAllocator alloc(8192);
    TypeEnv type_env(alloc);
    NativeRegistry registry(alloc, type_env.types());

    registry.register_struct("Point", {
        {"x", NativeTypeKind::I32},
        {"y", NativeTypeKind::I32}
    });
    registry.bind_method<point_sum>("Point", "sum");

    const char* source = R"(
        fun test(): i32 {
            var p = Point { x = 3, y = 4 };
            return p.sum();
        }
    )";
    Value result = compile_and_run_with_registry(source, "test", alloc, type_env, registry);
    CHECK(result.is_int());
    CHECK(result.as_int == 7);
}

TEST_CASE("Interop - Native struct with manual method") {
    BumpAllocator alloc(8192);
    TypeEnv type_env(alloc);
    NativeRegistry registry(alloc, type_env.types());

    registry.register_struct("Point", {
        {"x", NativeTypeKind::I32},
        {"y", NativeTypeKind::I32}
    });

    // Manual native method: receives (self_ptr, ...) in registers
    auto native_product = [](RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
        u64* regs = vm->call_stack.back().registers;
        CppPoint* self = reinterpret_cast<CppPoint*>(regs[first_arg]);
        i32 result = self->x * self->y;
        regs[dst] = static_cast<u64>(static_cast<i64>(result));
    };

    registry.bind_method(native_product, "fun Point.product(): i32");

    const char* source = R"(
        fun test(): i32 {
            var p = Point { x = 5, y = 6 };
            return p.product();
        }
    )";
    Value result = compile_and_run_with_registry(source, "test", alloc, type_env, registry);
    CHECK(result.is_int());
    CHECK(result.as_int == 30);
}

TEST_CASE("Interop - Native method with parameters") {
    BumpAllocator alloc(8192);
    TypeEnv type_env(alloc);
    NativeRegistry registry(alloc, type_env.types());

    registry.register_struct("Point", {
        {"x", NativeTypeKind::I32},
        {"y", NativeTypeKind::I32}
    });
    registry.bind_method<point_scaled_sum>("Point", "scaled_sum");

    const char* source = R"(
        fun test(): i32 {
            var p = Point { x = 3, y = 4 };
            return p.scaled_sum(10);
        }
    )";
    Value result = compile_and_run_with_registry(source, "test", alloc, type_env, registry);
    CHECK(result.is_int());
    CHECK(result.as_int == 70);  // (3 + 4) * 10
}

TEST_CASE("Interop - Multiple methods on one struct") {
    BumpAllocator alloc(8192);
    TypeEnv type_env(alloc);
    NativeRegistry registry(alloc, type_env.types());

    registry.register_struct("Point", {
        {"x", NativeTypeKind::I32},
        {"y", NativeTypeKind::I32}
    });
    registry.bind_method<point_sum>("Point", "sum");
    registry.bind_method<point_diff>("Point", "diff");
    registry.bind_method<point_is_origin>("Point", "is_origin");

    SUBCASE("sum and diff") {
        const char* source = R"(
            fun test(): i32 {
                var p = Point { x = 10, y = 3 };
                return p.sum() + p.diff();
            }
        )";
        Value result = compile_and_run_with_registry(source, "test", alloc, type_env, registry);
        CHECK(result.is_int());
        CHECK(result.as_int == 13 + 7);  // sum=13, diff=7
    }

    SUBCASE("is_origin false") {
        const char* source = R"(
            fun test(): bool {
                var p = Point { x = 1, y = 2 };
                return p.is_origin();
            }
        )";
        Value result = compile_and_run_with_registry(source, "test", alloc, type_env, registry);
        CHECK(result.as_u64() == 0);  // false
    }

    SUBCASE("is_origin true") {
        const char* source = R"(
            fun test(): bool {
                var p = Point { x = 0, y = 0 };
                return p.is_origin();
            }
        )";
        Value result = compile_and_run_with_registry(source, "test", alloc, type_env, registry);
        CHECK(result.as_u64() == 1);  // true
    }
}

TEST_CASE("Interop - Native method with multiple parameters") {
    BumpAllocator alloc(8192);
    TypeEnv type_env(alloc);
    NativeRegistry registry(alloc, type_env.types());

    registry.register_struct("Point", {
        {"x", NativeTypeKind::I32},
        {"y", NativeTypeKind::I32}
    });
    registry.bind_method<point_weighted>("Point", "weighted");

    const char* source = R"(
        fun test(): i32 {
            var p = Point { x = 3, y = 4 };
            return p.weighted(2, 5);
        }
    )";
    Value result = compile_and_run_with_registry(source, "test", alloc, type_env, registry);
    CHECK(result.is_int());
    CHECK(result.as_int == 26);  // 3*2 + 4*5
}

TEST_CASE("Interop - Native struct field access") {
    BumpAllocator alloc(8192);
    TypeEnv type_env(alloc);
    NativeRegistry registry(alloc, type_env.types());

    registry.register_struct("Point", {
        {"x", NativeTypeKind::I32},
        {"y", NativeTypeKind::I32}
    });

    const char* source = R"(
        fun test(): i32 {
            var p = Point { x = 10, y = 20 };
            return p.x + p.y;
        }
    )";
    Value result = compile_and_run_with_registry(source, "test", alloc, type_env, registry);
    CHECK(result.is_int());
    CHECK(result.as_int == 30);
}

TEST_CASE("Interop - Native struct with free function") {
    BumpAllocator alloc(8192);
    TypeEnv type_env(alloc);
    NativeRegistry registry(alloc, type_env.types());

    registry.register_struct("Point", {
        {"x", NativeTypeKind::I32},
        {"y", NativeTypeKind::I32}
    });
    registry.bind_method<point_sum>("Point", "sum");
    registry.bind<my_square>("square");

    const char* source = R"(
        fun test(): i32 {
            var p = Point { x = 3, y = 4 };
            return square(p.sum());
        }
    )";
    Value result = compile_and_run_with_registry(source, "test", alloc, type_env, registry);
    CHECK(result.is_int());
    CHECK(result.as_int == 49);  // square(7) = 49
}

// ============================================================================
// RoxyList<T> Interop Tests
// ============================================================================

// C++ function that reads a list created by Roxy
i32 list_sum(RoxyVM* vm, RoxyList<i32> list) {
    (void)vm;
    i32 total = 0;
    for (u32 i = 0; i < list.len(); i++) {
        total += list.get(static_cast<i64>(i));
    }
    return total;
}

// C++ function that modifies a list
void list_push_42(RoxyVM* vm, RoxyList<i32> list) {
    (void)vm;
    list.push(42);
}

// C++ function with list + primitive params
i32 list_get_at(RoxyVM* vm, RoxyList<i32> list, i32 index) {
    (void)vm;
    return list.get(static_cast<i64>(index));
}

TEST_CASE("Interop - RoxyList: C++ reads list from Roxy") {
    const char* source = R"(
        fun test(): i32 {
            var lst: List<i32> = List<i32>();
            lst.push(10);
            lst.push(20);
            lst.push(30);
            return list_sum(lst);
        }
    )";

    Value result = compile_and_run_mixed(source, "test",
        [](NativeRegistry& reg) {
            reg.bind<list_sum>("list_sum");
        });
    CHECK(result.is_int());
    CHECK(result.as_int == 60);  // 10 + 20 + 30
}

TEST_CASE("Interop - RoxyList: C++ modifies list") {
    const char* source = R"(
        fun test(): i32 {
            var lst: List<i32> = List<i32>();
            lst.push(1);
            list_push_42(lst);
            return lst[0] + lst[1];
        }
    )";

    Value result = compile_and_run_mixed(source, "test",
        [](NativeRegistry& reg) {
            reg.bind<list_push_42>("list_push_42");
        });
    CHECK(result.is_int());
    CHECK(result.as_int == 43);  // 1 + 42
}

TEST_CASE("Interop - RoxyList: C++ reads list with index param") {
    const char* source = R"(
        fun test(): i32 {
            var lst: List<i32> = List<i32>();
            lst.push(100);
            lst.push(200);
            lst.push(300);
            return list_get_at(lst, 1);
        }
    )";

    Value result = compile_and_run_mixed(source, "test",
        [](NativeRegistry& reg) {
            reg.bind<list_get_at>("list_get_at");
        });
    CHECK(result.is_int());
    CHECK(result.as_int == 200);
}

// ============================================================================
// RoxyString Interop Tests
// ============================================================================

// C++ function that reads a string from Roxy and returns its length
i32 str_get_len(RoxyVM* vm, RoxyString str) {
    (void)vm;
    return static_cast<i32>(str.length());
}

// C++ function that checks if a string equals "hello"
bool str_check_hello(RoxyVM* vm, RoxyString str) {
    (void)vm;
    return str.equals(RoxyString(string_alloc(vm, "hello", 5)));
}

// C++ function that creates a new string and returns it to Roxy
RoxyString str_make_greeting(RoxyVM* vm) {
    return RoxyString::alloc(vm, "hello from C++");
}

// C++ function that concatenates two strings
RoxyString str_join(RoxyVM* vm, RoxyString a, RoxyString b) {
    return a.concat(vm, b);
}

TEST_CASE("Interop - RoxyString: C++ reads string length") {
    const char* source = R"(
        fun test(): i32 {
            var s: string = "hello";
            return str_get_len(s);
        }
    )";

    Value result = compile_and_run_mixed(source, "test",
        [](NativeRegistry& reg) {
            reg.bind<str_get_len>("str_get_len");
        });
    CHECK(result.is_int());
    CHECK(result.as_int == 5);
}

TEST_CASE("Interop - RoxyString: C++ compares strings") {
    SUBCASE("matching string") {
        const char* source = R"(
            fun test(): bool {
                var s: string = "hello";
                return str_check_hello(s);
            }
        )";

        Value result = compile_and_run_mixed(source, "test",
            [](NativeRegistry& reg) {
                reg.bind<str_check_hello>("str_check_hello");
            });
        CHECK(result.as_u64() == 1);  // true
    }

    SUBCASE("non-matching string") {
        const char* source = R"(
            fun test(): bool {
                var s: string = "world";
                return str_check_hello(s);
            }
        )";

        Value result = compile_and_run_mixed(source, "test",
            [](NativeRegistry& reg) {
                reg.bind<str_check_hello>("str_check_hello");
            });
        CHECK(result.as_u64() == 0);  // false
    }
}

TEST_CASE("Interop - RoxyString: C++ creates string for Roxy") {
    const char* source = R"(
        fun test(): i32 {
            var s: string = str_make_greeting();
            return str_len(s);
        }
    )";

    Value result = compile_and_run_mixed(source, "test",
        [](NativeRegistry& reg) {
            reg.bind<str_make_greeting>("str_make_greeting");
        });
    CHECK(result.is_int());
    CHECK(result.as_int == 14);  // "hello from C++" is 14 chars
}

TEST_CASE("Interop - RoxyString: C++ concatenates strings") {
    const char* source = R"(
        fun test(): i32 {
            var a: string = "hello ";
            var b: string = "world";
            var c: string = str_join(a, b);
            return str_len(c);
        }
    )";

    Value result = compile_and_run_mixed(source, "test",
        [](NativeRegistry& reg) {
            reg.bind<str_join>("str_join");
        });
    CHECK(result.is_int());
    CHECK(result.as_int == 11);  // "hello world" is 11 chars
}
