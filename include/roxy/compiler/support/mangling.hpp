#pragma once

#include "roxy/core/span.hpp"
#include "roxy/core/string.hpp"
#include "roxy/core/string_view.hpp"

namespace rx {

class BumpAllocator;
struct Type;

// Canonical name mangling — the single source of truth for how a Roxy struct
// member (method / constructor / destructor) is spelled as a flat function name.
//
// The "$$" scheme is a load-bearing ABI: the C backend parses these names by
// their structure (c_emitter.cpp `suffix_after_last_dollar_dollar`,
// `ends_with("$$get")`, the "$$resume" suffix), so the spelling defined here
// cannot change without updating that parser. Every site that produces a mangled
// name must route through these functions — previously the format literals were
// hand-copied across ir_builder / lowering / semantic / coroutine_lowering /
// c_emitter, an eight-way drift hazard. See
// docs/internals/identifier-interning.md §6.1.
//
// Two output modes share one format literal per kind (defined in mangling.cpp):
//   - the arena form returns a StringView living as long as `alloc` (the hot
//     IR-build minting path);
//   - the `_owned` form returns a String for transient lookup keys at sites
//     without an allocator at hand (e.g. BytecodeBuilder has no BumpAllocator).
//
// IRBuilder exposes member wrappers of the same name that bind its own allocator;
// inside those members call these as `rx::mangle_*` to reach the free function.

// "<struct>$$<method>"
StringView mangle_method(BumpAllocator& alloc, StringView struct_name, StringView method_name);
String     mangle_method_owned(StringView struct_name, StringView method_name);

// "<struct>$$new", or "<struct>$$new$$<ctor>" when ctor_name is non-empty.
StringView mangle_constructor(BumpAllocator& alloc, StringView struct_name, StringView ctor_name = {});

// "<struct>$$delete", or "<struct>$$delete$$<dtor>" when dtor_name is non-empty.
StringView mangle_destructor(BumpAllocator& alloc, StringView struct_name, StringView dtor_name = {});
String     mangle_destructor_owned(StringView struct_name, StringView dtor_name = {});

// "<module>::<name>", or `name` unchanged when module_name is empty (single-file
// mode) — keeps non-pub names module-private after IR modules are merged.
StringView mangle_module_local(BumpAllocator& alloc, StringView module_name, StringView name);

// Canonical spelling of a type as a mangled-name component: primitives by their
// keyword ("i32", "string"), structs/enums/traits by name, and compound types
// with a reserved single-'$' separator — "List$i32", "Map$string$i32",
// "uniq$Point", "fun$i32_ret$bool". A TypeParam argument spells as "$T"
// (reserved leading '$' — marks an ABSTRACT Phase-B instantiation and cannot
// collide with any user identifier). Used for generic-instantiation suffixes
// ("identity$i32"), synthesized container methods ("List$i32$$to_string"), and
// overload mangles.
StringView mangle_type_name(BumpAllocator& alloc, Type* type);

// "$ol$<name>$<param1>$<param2>..." — the flat name of one member of a
// function overload set (e.g. "$ol$print$i32", "$ol$f$List$i32$string").
// The reserved LEADING '$' is what makes it collision-proof: user identifiers
// can't start with '$', and every other mangled namespace (methods
// "Struct$$m", generic instances "name$i32", module-locals "mod::name")
// starts with a user identifier — so "$ol$print$i32" can never alias the
// generic instantiation "print$i32". Contains no "$$", so the C emitter's
// structural parsers ignore it. Return types are deliberately excluded
// (return-type-only overloads are redefinition errors). Only members of
// overload sets (2+ definitions) are mangled — single definitions keep their
// plain name.
StringView mangle_overload(BumpAllocator& alloc, StringView fun_name, Span<Type*> param_types);

}  // namespace rx
