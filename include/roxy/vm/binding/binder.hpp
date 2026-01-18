#pragma once

#include "roxy/core/types.hpp"
#include "roxy/vm/value.hpp"
#include "roxy/vm/vm.hpp"
#include "roxy/vm/binding/type_traits.hpp"
#include "roxy/vm/binding/function_traits.hpp"

#include <utility>
#include <type_traits>

namespace rx {

namespace detail {

// Helper to call a function with arguments extracted from registers
template<typename Ret, typename... Args, std::size_t... Is>
Ret call_with_indices(Ret(*fn)(Args...), Value* regs, u8 first_arg, std::index_sequence<Is...>) {
    return fn(RoxyType<Args>::from_value(regs[first_arg + Is])...);
}

// Helper to call a void function
template<typename... Args, std::size_t... Is>
void call_void_with_indices(void(*fn)(Args...), Value* regs, u8 first_arg, std::index_sequence<Is...>) {
    fn(RoxyType<Args>::from_value(regs[first_arg + Is])...);
}

} // namespace detail

// FunctionBinder generates a native function wrapper for a C++ function
template<auto FnPtr>
struct FunctionBinder {
    using Traits = FunctionTraits<decltype(FnPtr)>;
    using ReturnType = typename Traits::return_type;
    static constexpr u32 Arity = Traits::arity;

    // The generated native function that matches NativeFunction signature
    static void invoke(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
        Value* regs = vm->call_stack.back().registers;

        if constexpr (std::is_void_v<ReturnType>) {
            // Call function, set result to null
            detail::call_void_with_indices(
                FnPtr, regs, first_arg, std::make_index_sequence<Arity>{});
            regs[dst] = Value::make_null();
        } else {
            // Call function and convert result to Value
            auto result = detail::call_with_indices(
                FnPtr, regs, first_arg, std::make_index_sequence<Arity>{});
            regs[dst] = RoxyType<ReturnType>::to_value(result);
        }
    }

    // Get the native function pointer
    static NativeFunction get() { return &invoke; }
};

// Helper function to get the native function wrapper for a C++ function
template<auto FnPtr>
NativeFunction bind() {
    return FunctionBinder<FnPtr>::get();
}

// Helper to get the arity of a bound function
template<auto FnPtr>
constexpr u32 arity() {
    return FunctionBinder<FnPtr>::Arity;
}

}
