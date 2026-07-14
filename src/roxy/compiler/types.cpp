#include "roxy/compiler/types.hpp"

#include "roxy/core/string.hpp"

#include <cstring>

namespace rx {

// === Value-lifecycle predicates (docs/internals/lifetimes.md "Value lifecycle") ===
//
// Recursion is finite: the only recursive cases descend into *value* struct
// fields (embedded structs), which cannot form cycles (direct value cycles are
// rejected at compile time — see recursive-types.md). Every indirecting kind
// (uniq / List / Map / ref / coro / function) is a leaf here — its predicate is
// a constant that does not consult the pointee.

bool Type::needs_drop() const {
    switch (kind) {
        case TypeKind::String:      // reference-counted → release (free at zero)
        case TypeKind::Uniq:        // owns a heap object → run dtor + free
        case TypeKind::Ref:         // counted borrow → ref_dec the pointee
        case TypeKind::List:        // owns a heap buffer (+ maybe owning elements)
        case TypeKind::Map:
        case TypeKind::Coroutine:   // owns a heap state struct
        case TypeKind::Function:    // closure: owns a heap env
            return true;
        case TypeKind::Struct: {
            // A struct with a (synthesized or user) destructor always drops.
            if (noncopyable()) return true;
            // An otherwise-copyable struct still drops iff it transitively holds a
            // `ref` (its only count-bearing copyable member).
            for (const auto& field : struct_info.fields) {
                if (field.type && field.type->needs_drop()) return true;
            }
            for (const auto& clause : struct_info.when_clauses) {
                for (const auto& variant : clause.variants) {
                    for (const auto& vf : variant.fields) {
                        if (vf.type && vf.type->needs_drop()) return true;
                    }
                }
            }
            return false;
        }
        default:
            return false;  // primitives, string, weak, enum, …
    }
}

bool Type::needs_retain() const {
    if (kind == TypeKind::Ref) return true;       // a copy of a borrow is another borrow
    if (kind == TypeKind::String) return true;    // a copy of a string retains (finding 9b)
    if (!is_copy()) return false;                 // move-only: no implicit-copy path
    if (kind == TypeKind::Struct) {
        for (const auto& field : struct_info.fields) {
            if (field.type && field.type->needs_retain()) return true;
        }
        for (const auto& clause : struct_info.when_clauses) {
            for (const auto& variant : clause.variants) {
                for (const auto& vf : variant.fields) {
                    if (vf.type && vf.type->needs_retain()) return true;
                }
            }
        }
        return false;
    }
    return false;  // primitives, string, weak, enum, …
}

// === Unified drop derivation (docs/internals/lifetimes.md "Value lifecycle") ===

// A struct whose drop the VM may inline as a descriptor field-walk: parentless
// (inherited structs chain to the parent's dtor, so they stay on the call path)
// and with a *synthetic* (compiler-generated, body-less) default destructor (a
// user default destructor has a body that must run, so it stays CallDtor).
static bool drop_struct_walk_eligible(Type* s) {
    if (!s || !s->is_struct()) return false;
    if (s->struct_info.parent != nullptr) return false;
    bool synthetic_default = false;
    for (const auto& dtor : s->struct_info.destructors) {
        if (dtor.name.empty()) {
            if (dtor.decl != nullptr) return false;  // user-defined default destructor
            synthetic_default = true;
        }
    }
    return synthetic_default;
}

static bool drop_struct_has_default_dtor(Type* s) {
    if (!s || !s->is_struct()) return false;
    for (const auto& dtor : s->struct_info.destructors) {
        if (dtor.name.empty()) return true;
    }
    return false;
}

DropPlan compute_drop_plan(Type* type) {
    DropPlan p;
    if (!type) return p;
    switch (type->kind) {
        case TypeKind::Uniq: {
            p.free_obj = true;  // owns a heap object
            Type* pointee = type->ref_info.inner_type;
            if (drop_struct_walk_eligible(pointee)) {
                p.kind = DropKind::WalkFields; p.struct_type = pointee;
            } else if (drop_struct_has_default_dtor(pointee)) {
                p.kind = DropKind::CallDtor; p.struct_type = pointee;
            }
            // else: uniq of a primitive / dtor-less value — just free.
            break;
        }
        case TypeKind::Ref:
            p.kind = DropKind::RefDec;  // counted borrow, never frees the pointee
            break;
        case TypeKind::String:
            // Reference-counted string: release (owner--; free at zero). The
            // string owns its own heap object, so free_obj is left false — release
            // itself frees when the count hits zero (finding 9b).
            p.kind = DropKind::StrRelease;
            break;
        case TypeKind::List:
            p.kind = DropKind::List; p.free_obj = true;
            p.elem_type = type->list_info.element_type;
            break;
        case TypeKind::Map:
            p.kind = DropKind::Map; p.free_obj = true;
            p.key_type = type->map_info.key_type;
            p.elem_type = type->map_info.value_type;
            break;
        case TypeKind::Function:
            p.kind = DropKind::Closure; p.free_obj = true;
            break;
        case TypeKind::Coroutine: {
            p.free_obj = true;  // heap state struct
            Type* coro_struct = type->coro_info.generated_struct_type;
            if (coro_struct) {
                // Known coroutine value (per-function type): its concrete state
                // struct — hence destructor — is statically known, so call it.
                if (drop_struct_has_default_dtor(coro_struct)) {
                    p.kind = DropKind::CallDtor; p.struct_type = coro_struct;
                }
            } else {
                // Erased Coro<T> (interned generic type — from an annotation or a
                // forwarded value): the concrete state struct is unknown here.
                // Dispatch the destructor at runtime by the value's identity,
                // exactly like a first-class closure (VM: closure_env_dtors by
                // type_id; C: __closure_delete by __resume_idx). A coroutine value
                // is layout-compatible — slot 0 is the dispatch index.
                p.kind = DropKind::Closure;
            }
            break;
        }
        case TypeKind::Struct:
            // An embedded value struct is cleaned in place (never freed).
            if (type->noncopyable()) {
                if (drop_struct_walk_eligible(type)) {
                    p.kind = DropKind::WalkFields; p.struct_type = type;
                } else if (drop_struct_has_default_dtor(type)) {
                    p.kind = DropKind::CallDtor; p.struct_type = type;
                }
            }
            break;
        default:
            break;  // primitives, string, weak, enum, … — nothing to drop
    }
    return p;
}

// Hash function for type interning
u64 TypeHash::operator()(const Type* t) const {
    if (!t) return 0;

    u64 hash = static_cast<u64>(t->kind);

    switch (t->kind) {
        case TypeKind::List:
            // Hash based on element type pointer
            hash ^= reinterpret_cast<u64>(t->list_info.element_type) * 31;
            break;

        case TypeKind::Map:
            // Hash based on key and value type pointers
            hash ^= reinterpret_cast<u64>(t->map_info.key_type) * 31;
            hash ^= reinterpret_cast<u64>(t->map_info.value_type) * 37;
            break;

        case TypeKind::Coroutine:
            // Hash based on yield type pointer
            hash ^= reinterpret_cast<u64>(t->coro_info.yield_type) * 31;
            break;

        case TypeKind::Function: {
            // Hash based on return type and parameter types
            hash ^= reinterpret_cast<u64>(t->func_info.return_type) * 31;
            for (u32 i = 0; i < t->func_info.param_types.size(); i++) {
                hash ^= reinterpret_cast<u64>(t->func_info.param_types[i]) * (37 + i);
            }
            break;
        }

        case TypeKind::Uniq:
        case TypeKind::Ref:
        case TypeKind::Weak:
            // Hash based on inner type pointer
            hash ^= reinterpret_cast<u64>(t->ref_info.inner_type) * 31;
            break;

        case TypeKind::Struct:
        case TypeKind::Enum:
        case TypeKind::Trait:
            // Named types use identity - hash by declaration pointer
            hash ^= reinterpret_cast<u64>(t->struct_info.decl) * 31;
            break;

        case TypeKind::TypeParam:
            // Hash by name and index
            hash ^= static_cast<u64>(t->type_param_info.index) * 31;
            break;

        default:
            // Primitives just use kind
            break;
    }

    return hash;
}

// Equality function for type interning
bool TypeEqual::operator()(const Type* a, const Type* b) const {
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->kind != b->kind) return false;

    switch (a->kind) {
        case TypeKind::List:
            return a->list_info.element_type == b->list_info.element_type;

        case TypeKind::Map:
            return a->map_info.key_type == b->map_info.key_type &&
                   a->map_info.value_type == b->map_info.value_type;

        case TypeKind::Coroutine:
            return a->coro_info.yield_type == b->coro_info.yield_type;

        case TypeKind::Function: {
            if (a->func_info.return_type != b->func_info.return_type) return false;
            if (a->func_info.param_types.size() != b->func_info.param_types.size()) return false;
            for (u32 i = 0; i < a->func_info.param_types.size(); i++) {
                if (a->func_info.param_types[i] != b->func_info.param_types[i]) return false;
            }
            return true;
        }

        case TypeKind::Uniq:
        case TypeKind::Ref:
        case TypeKind::Weak:
            return a->ref_info.inner_type == b->ref_info.inner_type;

        case TypeKind::Struct:
        case TypeKind::Enum:
        case TypeKind::Trait:
            // Named types are equal only if they're the same declaration
            return a->struct_info.decl == b->struct_info.decl;

        case TypeKind::TypeParam:
            return a->type_param_info.index == b->type_param_info.index &&
                   a->type_param_info.name == b->type_param_info.name;

        default:
            // Primitives are equal if kinds match
            return true;
    }
}

TypeCache::TypeCache(BumpAllocator& allocator)
    : m_allocator(allocator)
{
    // Create primitive type singletons
    m_void = create_primitive(TypeKind::Void);
    m_bool = create_primitive(TypeKind::Bool);
    m_i8 = create_primitive(TypeKind::I8);
    m_i16 = create_primitive(TypeKind::I16);
    m_i32 = create_primitive(TypeKind::I32);
    m_i64 = create_primitive(TypeKind::I64);
    m_u8 = create_primitive(TypeKind::U8);
    m_u16 = create_primitive(TypeKind::U16);
    m_u32 = create_primitive(TypeKind::U32);
    m_u64 = create_primitive(TypeKind::U64);
    m_f32 = create_primitive(TypeKind::F32);
    m_f64 = create_primitive(TypeKind::F64);
    m_string = create_primitive(TypeKind::String);
    m_nil = create_primitive(TypeKind::Nil);
    m_error = create_primitive(TypeKind::Error);
    m_self = create_primitive(TypeKind::Self);
    m_int_literal = create_primitive(TypeKind::IntLiteral);
    m_exception_ref = create_primitive(TypeKind::ExceptionRef);
}

Type* TypeCache::create_primitive(TypeKind kind) {
    Type* type = m_allocator.emplace<Type>();
    type->kind = kind;
    return type;
}

Type* TypeCache::intern_type(Type* type) {
    auto it = m_interned.find(type);
    if (it != m_interned.end()) {
        return it->second;
    }
    m_interned[type] = type;
    return type;
}

Type* TypeCache::list_type(Type* element_type) {
    Type* type = m_allocator.emplace<Type>();
    type->kind = TypeKind::List;
    type->list_info.element_type = element_type;
    type->list_info.methods = Span<MethodInfo>();
    type->list_info.alloc_native_name = StringView(nullptr, 0);

    return intern_type(type);
}

Type* TypeCache::map_type(Type* key_type, Type* value_type) {
    Type* type = m_allocator.emplace<Type>();
    type->kind = TypeKind::Map;
    type->map_info.key_type = key_type;
    type->map_info.value_type = value_type;
    type->map_info.methods = Span<MethodInfo>();
    type->map_info.alloc_native_name = StringView(nullptr, 0);

    return intern_type(type);
}

Type* TypeCache::coroutine_type(Type* yield_type) {
    Type* type = m_allocator.emplace<Type>();
    type->kind = TypeKind::Coroutine;
    type->coro_info.yield_type = yield_type;
    type->coro_info.generated_struct_type = nullptr;
    type->coro_info.methods = Span<MethodInfo>();
    type->coro_info.func_name = StringView(nullptr, 0);

    return intern_type(type);
}

Type* TypeCache::coroutine_type_for_func(Type* yield_type, StringView func_name) {
    // Each coroutine function gets a unique (non-interned) type so that
    // method calls can be resolved to the correct mangled function names.
    Type* type = m_allocator.emplace<Type>();
    type->kind = TypeKind::Coroutine;
    type->coro_info.yield_type = yield_type;
    type->coro_info.generated_struct_type = nullptr;
    type->coro_info.methods = Span<MethodInfo>();
    type->coro_info.func_name = func_name;
    return type;  // Not interned — unique per coroutine function
}

Type* TypeCache::function_type(Span<Type*> param_types, Type* return_type) {
    Type* type = m_allocator.emplace<Type>();
    type->kind = TypeKind::Function;

    // Copy param types to allocator
    Type** params = reinterpret_cast<Type**>(
        m_allocator.alloc_bytes(sizeof(Type*) * param_types.size(), alignof(Type*)));
    for (u32 i = 0; i < param_types.size(); i++) {
        params[i] = param_types[i];
    }

    type->func_info.param_types = Span<Type*>(params, param_types.size());
    type->func_info.return_type = return_type;
    return intern_type(type);
}

Type* TypeCache::uniq_type(Type* inner_type) {
    Type* type = m_allocator.emplace<Type>();
    type->kind = TypeKind::Uniq;
    type->ref_info.inner_type = inner_type;
    return intern_type(type);
}

Type* TypeCache::ref_type(Type* inner_type) {
    Type* type = m_allocator.emplace<Type>();
    type->kind = TypeKind::Ref;
    type->ref_info.inner_type = inner_type;
    return intern_type(type);
}

Type* TypeCache::weak_type(Type* inner_type) {
    Type* type = m_allocator.emplace<Type>();
    type->kind = TypeKind::Weak;
    type->ref_info.inner_type = inner_type;
    return intern_type(type);
}

Type* TypeCache::borrowed(Type* inner_type) {
    if (!inner_type || inner_type->is_error()) return inner_type;
    // The one demotion the prototype performs: an owning heap reference becomes a
    // borrow of its pointee. This is the case that matters for `List<uniq T>` /
    // `Map<K, uniq V>` indexing, where it turns a move-out into a plain ref→uniq
    // type error.
    if (inner_type->kind == TypeKind::Uniq) {
        return ref_type(inner_type->inner_type());
    }
    // A function value is a pointer to a heap-allocated closure env (with a
    // header), so borrow it as `ref fun(...)` — same env-pointer representation,
    // and callable (see the call paths in ir_builder / semantic). This lets
    // `List<fun>` indexing yield a borrow instead of aliasing the owned closure.
    if (inner_type->kind == TypeKind::Function) {
        return ref_type(inner_type);
    }
    // Everything else is identity. Copyable values/structs copy out; `ref`/`weak`
    // are already borrows; and the remaining noncopyable kinds (inline value
    // struct, coroutine, List/Map) keep their type so all their *safe* uses keep
    // working — for a value struct that means storage + RAII cleanup + in-place
    // field reads / method calls (it has no object header, so it genuinely can't
    // be borrowed *out*, but it doesn't need to be for those). The lifetime
    // checker's native-index guard (LifetimeChecker::consume_noncopyable) is the
    // right backstop here: it rejects only the unsound bind/move-out while
    // leaving everything else intact.
    return inner_type;
}

Type* TypeCache::struct_type(StringView name, Decl* decl, StringView module_name) {
    Type* type = m_allocator.emplace<Type>();
    type->kind = TypeKind::Struct;
    type->struct_info.name = name;
    // Use empty string "" if module_name is empty to ensure valid StringView
    type->struct_info.module_name = module_name.empty() ? ""_sv : module_name;
    type->struct_info.decl = decl;
    type->struct_info.parent = nullptr;
    type->struct_info.fields = Span<FieldInfo>();
    type->struct_info.implemented_traits = Span<TraitImplRecord>();
    type->struct_info.slot_count = 0;
    type->struct_info.members_resolved = false;
    // Named types are not interned - each declaration creates a unique type
    return type;
}

Type* TypeCache::enum_type(StringView name, Decl* decl, Type* underlying) {
    Type* type = m_allocator.emplace<Type>();
    type->kind = TypeKind::Enum;
    type->enum_info.name = name;
    type->enum_info.decl = decl;
    type->enum_info.underlying = underlying ? underlying : m_i32;
    type->enum_info.variants = Span<EnumVariantInfo>();
    return type;
}

const EnumVariantInfo* EnumTypeInfo::find_variant(StringView variant_name) const {
    for (const auto& variant : variants) {
        if (variant.name == variant_name) return &variant;
    }
    return nullptr;
}

Type* TypeCache::trait_type(StringView name, Decl* decl) {
    Type* type = m_allocator.emplace<Type>();
    type->kind = TypeKind::Trait;
    type->trait_info.name = name;
    type->trait_info.decl = decl;
    type->trait_info.parent = nullptr;
    type->trait_info.methods = Span<TraitMethodInfo>();
    type->trait_info.type_params = Span<TypeParam>();
    return type;
}

Type* TypeCache::type_param(StringView name, u32 index) {
    Type* type = m_allocator.emplace<Type>();
    type->kind = TypeKind::TypeParam;
    type->type_param_info.name = name;
    type->type_param_info.index = index;
    return type;
}

Type* TypeCache::primitive_by_name(StringView name) {
    if (name == "void") return m_void;
    if (name == "bool") return m_bool;
    if (name == "i8") return m_i8;
    if (name == "i16") return m_i16;
    if (name == "i32") return m_i32;
    if (name == "i64") return m_i64;
    if (name == "u8") return m_u8;
    if (name == "u16") return m_u16;
    if (name == "u32") return m_u32;
    if (name == "u64") return m_u64;
    if (name == "f32") return m_f32;
    if (name == "f64") return m_f64;
    if (name == "string") return m_string;
    return nullptr;
}


const FieldInfo* StructTypeInfo::find_field(StringView field_name) const {
    for (const auto& field : fields) {
        if (field.name == field_name) {
            return &field;
        }
    }
    return nullptr;
}

const VariantFieldInfo* StructTypeInfo::find_variant_field(StringView field_name,
                                                            const WhenClauseInfo** out_clause,
                                                            const VariantInfo** out_variant) const {
    for (const auto& clause : when_clauses) {
        for (const auto& variant : clause.variants) {
            for (const auto& field : variant.fields) {
                if (field.name == field_name) {
                    if (out_clause) *out_clause = &clause;
                    if (out_variant) *out_variant = &variant;
                    return &field;
                }
            }
        }
    }
    return nullptr;
}

void TypeCache::register_primitive_method(TypeKind kind, const MethodInfo& method) {
    m_primitive_methods[static_cast<u8>(kind)].push_back(method);
}

void TypeCache::register_primitive_trait(TypeKind kind, Type* trait) {
    m_primitive_traits[static_cast<u8>(kind)].push_back(trait);
}

const MethodInfo* TypeCache::lookup_primitive_method(TypeKind kind, StringView name) const {
    auto it = m_primitive_methods.find(static_cast<u8>(kind));
    if (it == m_primitive_methods.end()) return nullptr;
    for (const auto& method : it->second) {
        if (method.name == name) return &method;
    }
    return nullptr;
}

bool TypeCache::primitive_implements_trait(TypeKind kind, Type* trait) const {
    auto it = m_primitive_traits.find(static_cast<u8>(kind));
    if (it == m_primitive_traits.end()) return false;
    for (auto* t : it->second) {
        if (t == trait) return true;
    }
    return false;
}

const MethodInfo* TypeCache::lookup_method(Type* type, StringView name, Type** found_in) const {
    if (type->is_struct()) {
        return lookup_method_in_hierarchy(type, name, found_in);
    }
    if (type->is_primitive()) {
        return lookup_primitive_method(type->kind, name);
    }
    if (type->is_list()) {
        return lookup_list_method(type->list_info, name);
    }
    if (type->is_map()) {
        return lookup_map_method(type->map_info, name);
    }
    if (type->is_coroutine()) {
        return lookup_coro_method(type->coro_info, name);
    }
    if (type->is_enum()) {
        for (const auto& method : type->enum_info.methods) {
            if (method.name == name) return &method;
        }
    }
    return nullptr;
}

bool TypeCache::implements_trait(Type* type, Type* trait) const {
    if (type->is_struct()) {
        StructTypeInfo& struct_type_info = type->struct_info;
        for (const auto& impl : struct_type_info.implemented_traits) {
            if (impl.trait == trait) return true;
        }
        return false;
    }
    if (type->is_primitive()) {
        return primitive_implements_trait(type->kind, trait);
    }
    if (type->is_enum()) {
        // Enums can implement traits through their underlying type (i32)
        return primitive_implements_trait(TypeKind::I32, trait);
    }
    return false;
}

bool TypeCache::implements_trait(Type* type, Type* trait, Span<Type*> type_args) const {
    // If no type args, delegate to the simple overload
    if (type_args.size() == 0) {
        return implements_trait(type, trait);
    }

    if (type->is_struct()) {
        StructTypeInfo& struct_type_info = type->struct_info;
        for (const auto& impl : struct_type_info.implemented_traits) {
            if (impl.trait != trait) continue;
            // Check type args match element-wise
            if (impl.type_args.size() != type_args.size()) continue;
            bool match = true;
            for (u32 i = 0; i < type_args.size(); i++) {
                if (impl.type_args[i] != type_args[i]) {
                    match = false;
                    break;
                }
            }
            if (match) return true;
        }
        return false;
    }
    if (type->is_primitive()) {
        // Primitives don't support generic trait impls, only non-generic
        return type_args.size() == 0 && primitive_implements_trait(type->kind, trait);
    }
    if (type->is_enum()) {
        return type_args.size() == 0 && primitive_implements_trait(TypeKind::I32, trait);
    }
    return false;
}

const MethodInfo* lookup_method_in_hierarchy(Type* struct_type, StringView name, Type** found_in_type) {
    Type* current = struct_type;
    while (current && current->is_struct()) {
        StructTypeInfo& struct_type_info = current->struct_info;
        for (auto& method : struct_type_info.methods) {
            if (method.name == name) {
                if (found_in_type) *found_in_type = current;
                return &method;
            }
        }
        current = struct_type_info.parent;
    }
    return nullptr;
}

const MethodInfo* lookup_list_method(const ListTypeInfo& info, StringView name) {
    for (const auto& method : info.methods) {
        if (method.name == name) return &method;
    }
    return nullptr;
}

const MethodInfo* lookup_map_method(const MapTypeInfo& info, StringView name) {
    for (const auto& method : info.methods) {
        if (method.name == name) return &method;
    }
    return nullptr;
}

const MethodInfo* lookup_coro_method(const CoroutineTypeInfo& info, StringView name) {
    for (const auto& method : info.methods) {
        if (method.name == name) return &method;
    }
    return nullptr;
}

bool is_subtype_of(Type* child, Type* parent) {
    if (child == parent) return true;
    if (!child || !parent) return false;
    if (!child->is_struct() || !parent->is_struct()) return false;

    Type* current = child->struct_info.parent;
    while (current) {
        if (current == parent) return true;
        current = current->struct_info.parent;
    }
    return false;
}

u32 get_type_slot_count(Type* type) {
    if (!type) return 0;

    switch (type->kind) {
        // 1 slot (4 bytes) - small types widened to 32-bit
        case TypeKind::Bool:
        case TypeKind::I8:  case TypeKind::U8:
        case TypeKind::I16: case TypeKind::U16:
        case TypeKind::I32: case TypeKind::U32:
        case TypeKind::F32:
        case TypeKind::Enum:        // Enums are stored as i32
        case TypeKind::IntLiteral:  // Safety net: defaults to i32 (1 slot)
            return 1;

        // 2 slots (8 bytes): 64-bit primitives and pointers
        case TypeKind::I64: case TypeKind::U64:
        case TypeKind::F64:
        case TypeKind::String:      // Heap-allocated string object (pointer)
        case TypeKind::Uniq:
        case TypeKind::Ref:
        case TypeKind::List:
        case TypeKind::Map:
        case TypeKind::Coroutine:
        // Function closures are a single uniq pointer to the env struct
        // (env's first field holds the call_idx; subsequent fields hold captures).
        case TypeKind::Function:
            return 2;

        case TypeKind::Weak:
            return 4;  // 64-bit pointer + 64-bit generation

        // Structs: use computed slot_count
        case TypeKind::Struct:
            return type->struct_info.slot_count;

        default:
            return 0;
    }
}

const char* type_kind_to_string(TypeKind kind) {
    switch (kind) {
        case TypeKind::Void: return "void";
        case TypeKind::Bool: return "bool";
        case TypeKind::I8: return "i8";
        case TypeKind::I16: return "i16";
        case TypeKind::I32: return "i32";
        case TypeKind::I64: return "i64";
        case TypeKind::U8: return "u8";
        case TypeKind::U16: return "u16";
        case TypeKind::U32: return "u32";
        case TypeKind::U64: return "u64";
        case TypeKind::F32: return "f32";
        case TypeKind::F64: return "f64";
        case TypeKind::String: return "string";
        case TypeKind::List: return "list";
        case TypeKind::Map: return "map";
        case TypeKind::Coroutine: return "coro";
        case TypeKind::Function: return "function";
        case TypeKind::Struct: return "struct";
        case TypeKind::Enum: return "enum";
        case TypeKind::Trait: return "trait";
        case TypeKind::Uniq: return "uniq";
        case TypeKind::Ref: return "ref";
        case TypeKind::Weak: return "weak";
        case TypeKind::TypeParam: return "<type_param>";
        case TypeKind::Self: return "Self";
        case TypeKind::ExceptionRef: return "ExceptionRef";
        case TypeKind::IntLiteral: return "i32";
        case TypeKind::Nil: return "nil";
        case TypeKind::Error: return "<error>";
    }
    return "<unknown>";
}

static void append_string(String& out, const char* str) {
    while (*str) {
        out.push_back(*str++);
    }
}

static void append_string(String& out, StringView str) {
    for (char c : str) {
        out.push_back(c);
    }
}

void type_to_string(const Type* type, String& out) {
    if (!type) {
        append_string(out, "<null>");
        return;
    }

    switch (type->kind) {
        case TypeKind::Void:
        case TypeKind::Bool:
        case TypeKind::I8:
        case TypeKind::I16:
        case TypeKind::I32:
        case TypeKind::I64:
        case TypeKind::U8:
        case TypeKind::U16:
        case TypeKind::U32:
        case TypeKind::U64:
        case TypeKind::F32:
        case TypeKind::F64:
        case TypeKind::String:
        case TypeKind::Nil:
        case TypeKind::Self:
        case TypeKind::IntLiteral:
        case TypeKind::ExceptionRef:
        case TypeKind::Error:
            append_string(out, type_kind_to_string(type->kind));
            break;

        case TypeKind::List:
            append_string(out, "List<");
            type_to_string(type->list_info.element_type, out);
            append_string(out, ">");
            break;

        case TypeKind::Map:
            append_string(out, "Map<");
            type_to_string(type->map_info.key_type, out);
            append_string(out, ", ");
            type_to_string(type->map_info.value_type, out);
            append_string(out, ">");
            break;

        case TypeKind::Coroutine:
            append_string(out, "Coro<");
            type_to_string(type->coro_info.yield_type, out);
            append_string(out, ">");
            break;

        case TypeKind::Function: {
            append_string(out, "fun(");
            for (u32 i = 0; i < type->func_info.param_types.size(); i++) {
                if (i > 0) append_string(out, ", ");
                type_to_string(type->func_info.param_types[i], out);
            }
            append_string(out, ")");
            // Match user-facing syntax: omit the arrow for void return.
            if (type->func_info.return_type && !type->func_info.return_type->is_void()) {
                append_string(out, " -> ");
                type_to_string(type->func_info.return_type, out);
            }
            break;
        }

        case TypeKind::Struct:
            append_string(out, type->struct_info.name);
            break;

        case TypeKind::Enum:
            append_string(out, type->enum_info.name);
            break;

        case TypeKind::Trait:
            append_string(out, "trait ");
            append_string(out, type->trait_info.name);
            break;

        case TypeKind::Uniq:
            append_string(out, "uniq ");
            type_to_string(type->ref_info.inner_type, out);
            break;

        case TypeKind::Ref:
            append_string(out, "ref ");
            type_to_string(type->ref_info.inner_type, out);
            break;

        case TypeKind::Weak:
            append_string(out, "weak ");
            type_to_string(type->ref_info.inner_type, out);
            break;

        case TypeKind::TypeParam:
            append_string(out, type->type_param_info.name);
            break;
    }
}

// ===== Span append helpers (shared by SemanticAnalyzer and TraitSystem) =====

namespace {
// Append `value` to an arena-backed member table with geometric capacity
// growth. `capacity` rides alongside the span on StructTypeInfo; a span the
// caller assigned directly leaves it stale (always <= the span size), which
// reads as "full" so the next append reallocates — both styles compose. There
// is no single "members complete" point to freeze at (appends arrive from
// Pass 2 source order, trait default injection, generic-instance registration,
// and synthetic-dtor generation), hence grow-in-place rather than
// build-then-freeze. On growth the old buffer becomes arena garbage, so total
// arena waste is O(final size) — the previous rebuild-into-fresh-arena-memory
// per append cost O(n^2) time and arena bytes to build an n-member table.
template<typename T>
void append_span(BumpAllocator& allocator, Span<T>& span, u32& capacity, T value) {
    u32 size = span.size();
    if (size >= capacity) {
        u32 new_capacity = size < 4 ? 4 : size * 2;
        T* data = reinterpret_cast<T*>(
            allocator.alloc_bytes(sizeof(T) * new_capacity, alignof(T)));
        for (u32 i = 0; i < size; i++) {
            data[i] = span[i];
        }
        span = Span<T>(data, size);
        capacity = new_capacity;
    }
    span.data()[size] = value;
    span = Span<T>(span.data(), size + 1);
}
}

void append_method(BumpAllocator& allocator, StructTypeInfo& info, MethodInfo method) {
    append_span(allocator, info.methods, info.methods_capacity, method);
}

void append_constructor(BumpAllocator& allocator, StructTypeInfo& info, ConstructorInfo ctor) {
    append_span(allocator, info.constructors, info.constructors_capacity, ctor);
}

void append_destructor(BumpAllocator& allocator, StructTypeInfo& info, DestructorInfo dtor) {
    append_span(allocator, info.destructors, info.destructors_capacity, dtor);
}

bool struct_needs_synthetic_dtor(const StructTypeInfo& info) {
    for (const auto& field : info.fields) {
        if (member_needs_drop(field.type)) return true;
    }
    for (const auto& clause : info.when_clauses) {
        for (const auto& variant : clause.variants) {
            for (const auto& variant_field : variant.fields) {
                if (member_needs_drop(variant_field.type)) return true;
            }
        }
    }
    return false;
}

void add_synthetic_default_dtor(BumpAllocator& allocator, StructTypeInfo& info) {
    DestructorInfo synthetic_dtor;
    synthetic_dtor.name = StringView();
    synthetic_dtor.param_types = Span<Type*>();
    synthetic_dtor.decl = nullptr;
    append_destructor(allocator, info, synthetic_dtor);
}

}
