#pragma once

#include "roxy/core/types.hpp"
#include "roxy/vm/value.hpp"
#include "roxy/vm/vm.hpp"
#include "roxy/vm/binding/type_traits.hpp"
#include "roxy/vm/binding/function_traits.hpp"

#include <utility>
#include <type_traits>

namespace rx {

// FunctionBinder generates a native function wrapper for a C++ function.
// The C++ function must take RoxyVM* as its first parameter.
// Remaining parameters are read from registers.
template<auto FnPtr>
struct FunctionBinder {
    using Traits = FunctionTraits<decltype(FnPtr)>;
    using ReturnType = typename Traits::return_type;
    static constexpr u32 TotalArity = Traits::arity;
    static constexpr u32 RegArity = TotalArity - 1;  // params from registers (excluding RoxyVM*)

    static_assert(TotalArity >= 1,
                  "Bound function must take RoxyVM* as first parameter");
    static_assert(std::is_same_v<typename Traits::template arg_type<0>, RoxyVM*>,
                  "First parameter of bound function must be RoxyVM*");

    // The generated native function that matches NativeFunction signature
    static void invoke(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
        u64* regs = vm->call_stack.back().registers;
        invoke_impl(vm, regs, dst, first_arg, std::make_index_sequence<RegArity>{});
    }

    // Get the native function pointer
    static NativeFunction get() { return &invoke; }

private:
    template<std::size_t... Is>
    static void invoke_impl(RoxyVM* vm, u64* regs, u8 dst, u8 first_arg,
                            std::index_sequence<Is...>) {
        using ArgsTuple = typename Traits::args_tuple;

        if constexpr (std::is_void_v<ReturnType>) {
            FnPtr(vm, RoxyType<std::tuple_element_t<Is + 1, ArgsTuple>>::from_reg(
                           regs[first_arg + Is])...);
            regs[dst] = 0;
        } else {
            auto result = FnPtr(vm,
                RoxyType<std::tuple_element_t<Is + 1, ArgsTuple>>::from_reg(
                    regs[first_arg + Is])...);
            regs[dst] = RoxyType<ReturnType>::to_reg(result);
        }
    }
};

// Helper function to get the native function wrapper for a C++ function
template<auto FnPtr>
NativeFunction bind() {
    return FunctionBinder<FnPtr>::get();
}

}
