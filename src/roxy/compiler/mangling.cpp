#include "roxy/compiler/mangling.hpp"

#include "roxy/core/bump_allocator.hpp"
#include "roxy/core/format.hpp"

namespace rx {

namespace {
// One definition per mangling kind. Both the arena and owned-String forms format
// through these, so the "$$" ABI has exactly one source of truth.
constexpr const char* kMethod      = "{}$${}";
constexpr const char* kCtor        = "{}$$new";
constexpr const char* kCtorNamed   = "{}$$new$${}";
constexpr const char* kDtor        = "{}$$delete";
constexpr const char* kDtorNamed   = "{}$$delete$${}";
constexpr const char* kModuleLocal = "{}::{}";
}  // namespace

StringView mangle_method(BumpAllocator& alloc, StringView struct_name, StringView method_name) {
    return format_to_arena(alloc, runtime(kMethod), struct_name, method_name);
}

String mangle_method_owned(StringView struct_name, StringView method_name) {
    return format(runtime(kMethod), struct_name, method_name);
}

StringView mangle_constructor(BumpAllocator& alloc, StringView struct_name, StringView ctor_name) {
    if (ctor_name.empty()) {
        return format_to_arena(alloc, runtime(kCtor), struct_name);
    }
    return format_to_arena(alloc, runtime(kCtorNamed), struct_name, ctor_name);
}

StringView mangle_destructor(BumpAllocator& alloc, StringView struct_name, StringView dtor_name) {
    if (dtor_name.empty()) {
        return format_to_arena(alloc, runtime(kDtor), struct_name);
    }
    return format_to_arena(alloc, runtime(kDtorNamed), struct_name, dtor_name);
}

String mangle_destructor_owned(StringView struct_name, StringView dtor_name) {
    if (dtor_name.empty()) {
        return format(runtime(kDtor), struct_name);
    }
    return format(runtime(kDtorNamed), struct_name, dtor_name);
}

StringView mangle_module_local(BumpAllocator& alloc, StringView module_name, StringView name) {
    if (module_name.empty()) return name;
    return format_to_arena(alloc, runtime(kModuleLocal), module_name, name);
}

}  // namespace rx
