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
//
// The wrapped function signature is `Ret(Args...)` — the binder no longer
// prepends `RoxyVM*` to the call. Native functions that need the active
// runtime context (allocator, intern table, embedder user_data) call
// `roxy_get_ctx()` directly. The interpreter activates `vm->ctx` via
// `roxy::ScopedContext` on every public entry, so the context is always
// available during a native invocation.
template<auto FnPtr>
struct FunctionBinder {
    using Traits = FunctionTraits<decltype(FnPtr)>;
    using ReturnType = typename Traits::return_type;
    static constexpr u32 RegArity = Traits::arity;

    // The generated native function that matches NativeFunction signature
    static void invoke(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
        u64* regs = vm->call_stack_back().registers;
        invoke_impl(regs, dst, first_arg, std::make_index_sequence<RegArity>{});
    }

    // Get the native function pointer
    static NativeFunction get() { return &invoke; }

private:
    template<std::size_t... Is>
    static void invoke_impl(u64* regs, u8 dst, u8 first_arg,
                            std::index_sequence<Is...>) {
        using ArgsTuple = typename Traits::args_tuple;

        if constexpr (std::is_void_v<ReturnType>) {
            FnPtr(RoxyType<std::tuple_element_t<Is, ArgsTuple>>::from_reg(
                      regs[first_arg + Is])...);
            regs[dst] = 0;
        } else {
            auto result = FnPtr(
                RoxyType<std::tuple_element_t<Is, ArgsTuple>>::from_reg(
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
