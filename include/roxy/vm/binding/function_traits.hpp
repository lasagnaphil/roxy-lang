#pragma once

#include "roxy/core/types.hpp"
#include <tuple>
#include <type_traits>

namespace rx {

// Primary template - undefined
template<typename Func>
struct FunctionTraits;

// Specialization for function pointers
template<typename Ret, typename... Args>
struct FunctionTraits<Ret(*)(Args...)> {
    using return_type = Ret;
    using args_tuple = std::tuple<Args...>;
    static constexpr u32 arity = sizeof...(Args);

    template<u32 N>
    using arg_type = std::tuple_element_t<N, args_tuple>;
};

// Specialization for function references
template<typename Ret, typename... Args>
struct FunctionTraits<Ret(&)(Args...)> : FunctionTraits<Ret(*)(Args...)> {};

// Specialization for member function pointers
template<typename Ret, typename Class, typename... Args>
struct FunctionTraits<Ret(Class::*)(Args...)> {
    using return_type = Ret;
    using class_type = Class;
    using args_tuple = std::tuple<Args...>;
    static constexpr u32 arity = sizeof...(Args);

    template<u32 N>
    using arg_type = std::tuple_element_t<N, args_tuple>;
};

// Specialization for const member function pointers
template<typename Ret, typename Class, typename... Args>
struct FunctionTraits<Ret(Class::*)(Args...) const> {
    using return_type = Ret;
    using class_type = Class;
    using args_tuple = std::tuple<Args...>;
    static constexpr u32 arity = sizeof...(Args);

    template<u32 N>
    using arg_type = std::tuple_element_t<N, args_tuple>;
};

// Helper to extract traits from function pointer type
template<auto FnPtr>
using FunctionPointerTraits = FunctionTraits<decltype(FnPtr)>;

}
