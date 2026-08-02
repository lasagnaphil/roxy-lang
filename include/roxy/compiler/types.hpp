#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/span.hpp"
#include "roxy/core/string.hpp"
#include "roxy/core/string_view.hpp"
#include "roxy/core/vector.hpp"
#include "roxy/core/bump_allocator.hpp"

#include "roxy/core/tsl/robin_map.h"

#include <cstring>

namespace rx {

// Forward declarations
struct Type;
struct Decl;
struct StructDecl;
struct EnumDecl;
struct FunDecl;
struct ConstructorDecl;
struct DestructorDecl;
enum class BinaryOp : u8;  // defined in ast.hpp; used by the primitive op tables
enum class UnaryOp : u8;

enum class TypeKind : u8 {
    // Primitives
    Void,
    Bool,
    I8, I16, I32, I64,
    U8, U16, U32, U64,
    F32, F64,
    String,

    // Compound types
    List,
    Map,
    Coroutine,
    Function,
    Struct,
    Enum,
    Trait,

    // Reference wrappers
    Uniq,
    Ref,
    Weak,

    // Generic type parameter
    TypeParam,

    // Self type (used in trait method signatures)
    Self,

    // Unsuffixed integer literal type (polymorphic, defaults to i32)
    IntLiteral,

    // Unsuffixed float literal type (polymorphic, defaults to f64)
    FloatLiteral,

    // Exception handling
    ExceptionRef,  // Opaque handle in catch-all blocks, only message() callable

    // Special types
    Nil,    // Type of nil literal, assignable to reference types
    Error,  // Sentinel for type errors, allows analysis to continue
};

// Constructor information for struct types
struct ConstructorInfo {
    StringView name;               // empty for default constructor
    Span<Type*> param_types;
    Decl* decl;                    // Points to the ConstructorDecl AST node
};

// Destructor information for struct types
struct DestructorInfo {
    StringView name;               // empty for default destructor
    Span<Type*> param_types;       // Destructors can have params
    Decl* decl;                    // Points to the DestructorDecl AST node
};

// Method information for struct types
struct MethodInfo {
    StringView name;
    Span<Type*> param_types;       // NOT including implicit self
    Type* return_type;
    Decl* decl;                    // Points to the MethodDecl AST node
    StringView native_name;        // Non-empty for native/builtin methods
};

// Field information for struct types
struct FieldInfo {
    StringView name;
    Type* type;
    bool is_pub;
    u32 index;       // Field index in declaration order
    u32 slot_offset; // Offset in u32 slots from struct start
    u32 slot_count;  // Number of u32 slots this field occupies (1 for 32-bit, 2 for 64-bit)
};

// Variant field info for tagged unions (field within a specific variant)
struct VariantFieldInfo {
    StringView name;
    Type* type;
    bool is_pub;
    u32 slot_offset;    // Offset WITHIN the union (from union start)
    u32 slot_count;
};

// Variant info for tagged unions (one case in the when clause)
struct VariantInfo {
    StringView case_name;           // e.g., "Attack"
    i64 discriminant_value;         // Enum value for this variant
    Span<VariantFieldInfo> fields;  // Fields for this variant
    u32 variant_slot_count;         // Total size of this variant in slots
};

// When clause info for tagged unions
struct WhenClauseInfo {
    StringView discriminant_name;   // e.g., "type"
    Type* discriminant_type;        // Enum type
    u32 discriminant_slot_offset;   // Where discriminant is in struct
    u32 union_slot_offset;          // Where union data starts
    u32 union_slot_count;           // Max of all variant sizes
    Span<VariantInfo> variants;
};

// Trait method information
struct TraitMethodInfo {
    StringView name;
    Span<Type*> param_types;   // Self type entries use TypeKind::Self
    Type* return_type;         // Self type entries use TypeKind::Self
    Decl* decl;                // Points to the DeclMethod AST node
    bool has_default;          // true if method has a body (default implementation)
};

// Forward declaration
struct TypeParam;

// Resolved trait bound on a type parameter
struct TraitBound {
    Type* trait;            // Resolved trait type
    Span<Type*> type_args;  // Resolved type args (e.g., {i32} for Add<i32>). Empty for non-generic.
};

// Record of a trait implementation on a struct (includes type args for generic traits)
struct TraitImplRecord {
    Type* trait;
    Span<Type*> type_args;  // Empty for non-generic traits
};

// Type info for trait types
struct TraitTypeInfo {
    StringView name;
    Decl* decl;                        // Points to the DeclTrait AST node
    Type* parent;                      // Parent trait type, nullptr if no inheritance
    Span<TraitMethodInfo> methods;     // Trait methods (required and default)
    Span<TypeParam> type_params;       // Generic type params: <T, U>
};

// Type info for struct types
struct StructTypeInfo {
    StringView name;
    StringView module_name;        // Module that defined this struct (for visibility checking)
    Decl* decl;                    // Points to the StructDecl AST node
    Type* parent;                  // Parent struct type, nullptr if no inheritance
    Span<FieldInfo> fields;        // All fields including inherited
    Span<ConstructorInfo> constructors;  // Constructors for this struct
    Span<DestructorInfo> destructors;    // Destructors for this struct
    Span<MethodInfo> methods;            // Methods for this struct
    Span<WhenClauseInfo> when_clauses;   // Tagged union discriminants
    Span<TraitImplRecord> implemented_traits;  // Trait implementations (with type args for generic traits)
    // Arena capacities of the constructor/destructor/method tables above, for
    // the geometric growth in the append_* builders (types.cpp). Zero — or
    // stale after a direct span assignment (always <= the span size) — reads
    // as "full", so the next append reallocates; direct assignment stays legal.
    u32 constructors_capacity;
    u32 destructors_capacity;
    u32 methods_capacity;
    u32 slot_count;                // Total u32 slots needed for this struct
    bool members_resolved;         // Fields/layout resolved (resolve_struct_members ran, or
                                   // the type is registry-built/synthesized and owns its layout)

    // Whether this struct is move-only — see derive_struct_move_only. Read by
    // `noncopyable()`; never set it directly.
    //
    // Precomputed rather than derived on demand because the definition recurses
    // through field types, and a recursive predicate would have to defend against
    // value cycles. Those are rejected as infinitely sized elsewhere, but not
    // here, and "elsewhere" is not a guarantee a predicate can rely on.
    bool is_move_only;
    // Debug tripwire: set once the flag above is derived. Reading `is_move_only`
    // before then would silently report a move-only struct as copyable — the
    // unsound direction — so `noncopyable()` asserts on it rather than letting a
    // default-initialized `false` through.
    bool move_only_derived;

    // Find a field by name, returns nullptr if not found
    const FieldInfo* find_field(StringView field_name) const;

    // Find a variant field by name in any when clause
    // Returns nullptr if not found, sets out_clause and out_variant if found
    const VariantFieldInfo* find_variant_field(StringView field_name,
                                                const WhenClauseInfo** out_clause = nullptr,
                                                const VariantInfo** out_variant = nullptr) const;
};

// One variant of an enum. Variants are resolved through the enum type's own
// table (Enum::Variant, when-statement cases, tagged-union when clauses) —
// NOT through the flat symbol namespace, where same-named variants of
// different enums shadow each other.
struct EnumVariantInfo {
    StringView name;
    i64 value;
};

// Type info for enum types
struct EnumTypeInfo {
    StringView name;
    Decl* decl;           // Points to the EnumDecl AST node
    Type* underlying;     // Underlying integer type (defaults to i32)
    Span<MethodInfo> methods;  // Builtin methods (eq, ne)
    Span<EnumVariantInfo> variants;  // Populated by resolve_enum_members

    // Find a variant by name, returns nullptr if not found
    const EnumVariantInfo* find_variant(StringView variant_name) const;
};

// Type info for list types
struct ListTypeInfo {
    Type* element_type;
    Span<MethodInfo> methods;          // Builtin methods with concrete types
    StringView alloc_native_name;      // "list_alloc" — set by SemanticAnalyzer
    StringView copy_native_name;       // "list_copy" — deep-copy for value parameter passing
};

// Type info for map types
struct MapTypeInfo {
    Type* key_type;
    Type* value_type;
    Span<MethodInfo> methods;          // Builtin methods with concrete types
    StringView alloc_native_name;      // "map_alloc" — set by SemanticAnalyzer
    StringView copy_native_name;       // "map_copy" — deep-copy for value parameter passing
};

// Type info for coroutine types (Coro<T>)
struct CoroutineTypeInfo {
    Type* yield_type;                  // T in Coro<T>
    Type* generated_struct_type;       // Synthetic struct holding coroutine state
    Span<MethodInfo> methods;          // resume() and done()
    StringView func_name;             // Name of the coroutine function (for method mangling)
};

// Type info for function types
struct FunctionTypeInfo {
    Span<Type*> param_types;
    Type* return_type;
};

// Type info for reference wrapper types (uniq, ref, weak)
struct RefTypeInfo {
    Type* inner_type;
};

// Type info for generic type parameters (T, U, etc.)
struct TypeParamInfo {
    StringView name;    // "T", "U", etc.
    u32 index;          // Position in type param list
};

// The main Type structure - a tagged union
struct Type {
    TypeKind kind;

    union {
        StructTypeInfo struct_info;
        EnumTypeInfo enum_info;
        TraitTypeInfo trait_info;
        ListTypeInfo list_info;
        MapTypeInfo map_info;
        CoroutineTypeInfo coro_info;
        FunctionTypeInfo func_info;
        RefTypeInfo ref_info;
        TypeParamInfo type_param_info;
    };

    // Default constructor - initializes to error type with zeroed union
    Type() : kind(TypeKind::Error) {
        memset(&struct_info, 0, sizeof(struct_info));
    }
    ~Type() {}

    // Helper methods
    bool is_primitive() const {
        return kind >= TypeKind::Void && kind <= TypeKind::String;
    }

    bool is_integer() const {
        return kind >= TypeKind::I8 && kind <= TypeKind::U64;
    }

    bool is_signed_integer() const {
        return kind >= TypeKind::I8 && kind <= TypeKind::I64;
    }

    bool is_unsigned_integer() const {
        return kind >= TypeKind::U8 && kind <= TypeKind::U64;
    }

    // Integer types narrower than 32 bits. Under Java/C#-style numeric promotion
    // these have no native arithmetic — they widen to i32 for every operation.
    // (u32/u64 are deliberately excluded: their values can exceed 2^31, so they
    // need genuine unsigned ops rather than promotion.)
    bool is_narrow_integer() const {
        return kind == TypeKind::I8 || kind == TypeKind::I16 ||
               kind == TypeKind::U8 || kind == TypeKind::U16;
    }

    bool is_float() const {
        return kind == TypeKind::F32 || kind == TypeKind::F64;
    }

    bool is_numeric() const {
        return is_integer() || is_float();
    }

    bool is_reference() const {
        return kind == TypeKind::Uniq || kind == TypeKind::Ref || kind == TypeKind::Weak;
    }

    bool is_error() const {
        return kind == TypeKind::Error;
    }

    bool is_void() const {
        return kind == TypeKind::Void;
    }

    bool is_bool() const {
        return kind == TypeKind::Bool;
    }

    bool is_struct() const {
        return kind == TypeKind::Struct;
    }

    bool is_enum() const {
        return kind == TypeKind::Enum;
    }

    bool is_trait() const {
        return kind == TypeKind::Trait;
    }

    bool is_list() const {
        return kind == TypeKind::List;
    }

    bool is_map() const {
        return kind == TypeKind::Map;
    }

    // Builtin container types (List, Map) — types that own a collection of elements
    // requiring element-by-element cleanup via native function calls.
    bool is_container() const {
        return kind == TypeKind::List || kind == TypeKind::Map;
    }

    bool is_coroutine() const {
        return kind == TypeKind::Coroutine;
    }

    bool is_function() const {
        return kind == TypeKind::Function;
    }

    bool is_nil() const {
        return kind == TypeKind::Nil;
    }

    bool is_type_param() const {
        return kind == TypeKind::TypeParam;
    }

    bool is_self() const {
        return kind == TypeKind::Self;
    }

    bool is_int_literal() const {
        return kind == TypeKind::IntLiteral;
    }

    bool is_float_literal() const {
        return kind == TypeKind::FloatLiteral;
    }

    // Either polymorphic literal kind — an unsuffixed literal that hasn't been
    // given a concrete type by its context yet.
    bool is_numeric_literal() const {
        return is_int_literal() || is_float_literal();
    }

    bool is_exception_ref() const {
        return kind == TypeKind::ExceptionRef;
    }

    // Returns true for MOVE-ONLY types — those that cannot be implicitly
    // duplicated, so binding one moves its source. This includes:
    //   - uniq references
    //   - Coro<T> (heap-allocated state struct)
    //   - Function types (own a uniq closure env via the type-erased wrapper)
    //   - List and Map (each owns a heap buffer)
    //   - structs that are structurally move-only (see derive_struct_move_only)
    //
    // The struct arm used to read "has a default destructor", which welded
    // move-only-ness to *drop*: a struct earned a destructor and became move-only
    // in the same instant. That is wrong in both directions — a `string` field is
    // reference-counted and perfectly copyable, while a `uniq` field is neither —
    // and it is why a `string` struct field leaked. A type is move-only exactly
    // when its drop has no inverse (lifetimes.md "The value lifecycle"), which is
    // a property of what it *holds*, not of whether it has a destructor.
    bool noncopyable() const {
        if (kind == TypeKind::Uniq) return true;
        if (kind == TypeKind::Coroutine) return true;
        if (kind == TypeKind::Function) return true;
        if (kind == TypeKind::Struct) {
            assert(struct_info.move_only_derived &&
                   "noncopyable(): is_move_only read before derive_struct_move_only ran");
            return struct_info.is_move_only;
        }
        // A List of `ref` is noncopyable (move-only): those elements are counted
        // borrows, so the container must be destroyed to release each element's
        // count on teardown (lifetimes.md "Applying the model"). `ref` itself is copyable; a *List
        // of* refs is not. (Map<_, ref> ref-counting is a follow-on; it stays
        // copyable for now, like its other-element behavior.)
        // Containers own a heap buffer, so they are move-only (like `uniq`): a
        // List/Map is always noncopyable. An explicit `.copy()` deep-copies when
        // a copy is genuinely wanted. (lifetimes.md "Applying the model" / overview.md.)
        if (kind == TypeKind::List) return true;
        if (kind == TypeKind::Map) return true;
        return false;
    }

    // === Value-lifecycle predicates (docs/internals/lifetimes.md "Value lifecycle") ===
    // The canonical, structural definition of a type's lifecycle needs — the
    // source of truth the future drop/copy/clone glue lowering will consume.
    // Introduced ahead of the migration (doc step 1); no consumers yet beyond a
    // cross-check assertion in build_delete_desc.

    // Implicit copy is permitted (vs move-only) — the `Copy` marker, exactly the
    // inverse of move-only. Note: a `Copy` type may still carry retain/drop glue
    // (a `ref`, or a copyable struct holding one); `Copy` is not "trivially
    // memcpy-able" — that is `is_trivial()`.
    bool is_copy() const { return !noncopyable(); }

    // The value owns or borrows a resource that must be released when its storage
    // dies (drop glue is non-empty). Reports `true` for a `ref` (a counted
    // borrow → ref_dec) wherever it appears, including as a struct field — which
    // the current descriptor machinery does not yet clean. That divergence is a
    // gap this design closes, not an error here.
    bool needs_drop() const;

    // Implicit copy (copy_init) has a side effect — i.e. the value (transitively)
    // contains a `ref`, whose copy is another counted borrow (ref_inc). Only
    // meaningful for `Copy` types; move-only types have no implicit-copy path and
    // report false.
    bool needs_retain() const;

    // No glue at all: copy is a plain memcpy and drop is a no-op. The
    // `is_trivially_destructible` analogue that lets the compiler emit nothing.
    bool is_trivial() const { return is_copy() && !needs_drop() && !needs_retain(); }

    // Get the inner type for reference types
    Type* inner_type() const {
        if (is_reference()) {
            return ref_info.inner_type;
        }
        return nullptr;
    }

    // Get the base type (unwraps all reference layers)
    Type* base_type() const {
        const Type* t = this;
        while (t->is_reference()) {
            t = t->ref_info.inner_type;
        }
        return const_cast<Type*>(t);
    }
};

// Hash function for Type pointers (used for type interning)
struct TypeHash {
    u64 operator()(const Type* t) const;
};

// Equality function for Type pointers
struct TypeEqual {
    bool operator()(const Type* a, const Type* b) const;
};

// TypeCache manages type creation and interning
// Ensures structural type equality via pointer comparison
class TypeCache {
public:
    explicit TypeCache(BumpAllocator& allocator);

    // Primitive type singletons
    Type* void_type() { return m_void; }
    Type* bool_type() { return m_bool; }
    Type* i8_type() { return m_i8; }
    Type* i16_type() { return m_i16; }
    Type* i32_type() { return m_i32; }
    Type* i64_type() { return m_i64; }
    Type* u8_type() { return m_u8; }
    Type* u16_type() { return m_u16; }
    Type* u32_type() { return m_u32; }
    Type* u64_type() { return m_u64; }
    Type* f32_type() { return m_f32; }
    Type* f64_type() { return m_f64; }
    Type* string_type() { return m_string; }
    Type* nil_type() { return m_nil; }
    Type* error_type() { return m_error; }
    Type* self_type() { return m_self; }
    Type* int_literal_type() { return m_int_literal; }
    Type* float_literal_type() { return m_float_literal; }
    Type* exception_ref_type() { return m_exception_ref; }

    // Factory methods for compound types (with interning)
    Type* list_type(Type* element_type);
    Type* map_type(Type* key_type, Type* value_type);
    Type* coroutine_type(Type* yield_type);
    Type* coroutine_type_for_func(Type* yield_type, StringView func_name);
    Type* function_type(Span<Type*> param_types, Type* return_type);
    Type* uniq_type(Type* inner_type);
    Type* ref_type(Type* inner_type);
    Type* weak_type(Type* inner_type);

    // The `borrowed` type-level transform: demote an owning type to a borrow.
    //   uniq T       -> ref T        (owning heap reference -> borrow)
    //   fun(...)     -> ref fun(...) (closure value is a heap env pointer)
    //   everything else -> unchanged (copyable copies out; ref/weak already a
    //     borrow; inline value struct / coroutine / List / Map keep their type —
    //     their safe uses don't need a borrow, and the move-checker's native-index
    //     guard backstops the unsound move-out). Never returns null.
    Type* borrowed(Type* inner_type);

    // Factory methods for named types (not interned - unique per declaration)
    Type* struct_type(StringView name, Decl* decl, StringView module_name = StringView(nullptr, 0));
    Type* enum_type(StringView name, Decl* decl, Type* underlying = nullptr);
    Type* trait_type(StringView name, Decl* decl);

    // Factory method for generic type parameters
    Type* type_param(StringView name, u32 index);

    // Primitive trait/method support
    void register_primitive_method(TypeKind kind, const MethodInfo& method);
    void register_primitive_trait(TypeKind kind, Type* trait);
    const MethodInfo* lookup_primitive_method(TypeKind kind, StringView name) const;
    bool primitive_implements_trait(TypeKind kind, Type* trait) const;

    // Build the dense primitive operator-dispatch tables from the registered
    // primitive methods. Must be called once after all primitive operator
    // methods are registered (the tables hold pointers into m_primitive_methods,
    // which must be done growing). See OPTIMIZATION.md §3.5.
    void build_primitive_operator_tables();
    // O(1) operator lookup for primitive operands: a plain array index, replacing
    // the name-keyed hash + linear scan of lookup_primitive_method. Returns
    // nullptr for unregistered (kind, op) pairs — same fall-through as before.
    const MethodInfo* lookup_primitive_binary_op(TypeKind kind, BinaryOp op) const;
    const MethodInfo* lookup_primitive_unary_op(TypeKind kind, UnaryOp op) const;

    // Unified lookup: works for structs (via hierarchy) AND primitives
    const MethodInfo* lookup_method(Type* type, StringView name, Type** found_in = nullptr) const;
    bool implements_trait(Type* type, Type* trait) const;
    bool implements_trait(Type* type, Type* trait, Span<Type*> type_args) const;

    // The builtin Printable trait, set by TraitSystem::register_builtin_traits.
    // implements_trait uses it to answer structurally for containers (List<T>
    // printable iff T is; Map<K,V> iff K and V are).
    void set_printable_trait(Type* trait) { m_printable_trait = trait; }

    // Lookup primitive type by name
    Type* primitive_by_name(StringView name);

private:
    Type* create_primitive(TypeKind kind);
    Type* intern_type(Type* type);

    BumpAllocator& m_allocator;

    // Primitive type singletons
    Type* m_void;
    Type* m_bool;
    Type* m_i8;
    Type* m_i16;
    Type* m_i32;
    Type* m_i64;
    Type* m_u8;
    Type* m_u16;
    Type* m_u32;
    Type* m_u64;
    Type* m_f32;
    Type* m_f64;
    Type* m_string;
    Type* m_nil;
    Type* m_error;
    Type* m_self;
    Type* m_int_literal;
    Type* m_float_literal;
    Type* m_exception_ref;

    // Type interning cache for compound types
    tsl::robin_map<Type*, Type*, TypeHash, TypeEqual> m_interned;

    // Primitive method and trait tables (keyed by TypeKind)
    tsl::robin_map<u8, Vector<MethodInfo>> m_primitive_methods;
    tsl::robin_map<u8, Vector<Type*>> m_primitive_traits;

    // Builtin Printable trait (see set_printable_trait) — enables the
    // structural container arm of implements_trait.
    Type* m_printable_trait = nullptr;

    // Dense primitive operator-dispatch tables (§3.5): [TypeKind][op] -> method
    // (nullptr = unregistered). PRIM_OP_KIND_COUNT covers every primitive kind
    // (is_primitive() is [Void, String]); the op-count constants are asserted
    // against the enums in build_primitive_operator_tables().
    static constexpr u32 PRIM_OP_KIND_COUNT = static_cast<u32>(TypeKind::String) + 1;
    static constexpr u32 BINARY_OP_COUNT = 18;  // BinaryOp::Add .. Shr
    static constexpr u32 UNARY_OP_COUNT = 4;    // UnaryOp::Negate .. Ref
    const MethodInfo* m_primitive_binary_ops[PRIM_OP_KIND_COUNT][BINARY_OP_COUNT] = {};
    const MethodInfo* m_primitive_unary_ops[PRIM_OP_KIND_COUNT][UNARY_OP_COUNT] = {};
};

// === Unified drop derivation (docs/internals/lifetimes.md "Value lifecycle") ===
// `compute_drop_plan` is the single, backend-agnostic decision for *what kind* of
// drop a type needs. Both backends consume it — the VM lowers it to a
// `BCDeleteDesc` (executed natively by delete_value), the C backend lowers it to a
// drop-glue / dtor call — so the dispatch is derived once instead of twice (the
// dual derivation is what made Map<_, ref V> need parallel fixes). It is a
// single-level decision: the recursion into element/field types stays in each
// backend's lowering.
enum class DropKind {
    None,        // no cleanup (free only, if free_obj)
    CallDtor,    // call the struct/coro `$$delete` (struct_type)
    WalkFields,  // walk struct_type's owned fields in place (VM); C calls its dtor
    List,        // List: clean elem_type members, free buffer + header
    Map,         // Map: clean key_type/elem_type members, free buffers + header
    Closure,     // type-erased closure env (dispatch by call idx)
    RefDec,      // release a counted borrow (ref_dec the pointee; never free it)
    StrRelease,  // release an owned string (owner--; free at zero; no-op if immortal)
};
struct DropPlan {
    DropKind kind = DropKind::None;
    bool free_obj = false;     // is `type` a heap pointer to free after cleanup
    Type* struct_type = nullptr;  // CallDtor: dtor target; WalkFields: struct to walk
    Type* elem_type = nullptr;    // List element / Map value
    Type* key_type = nullptr;     // Map key
};
DropPlan compute_drop_plan(Type* type);

// The mirror of compute_drop_plan: what must be *acquired* when a value is
// implicitly duplicated into a second location (lifetimes.md "The value
// lifecycle"). Kept beside the drop derivation deliberately — the two are read
// together, and a type whose drop is non-trivial must either have a retain that
// exactly inverts it or be move-only. Both halves in one place is what makes
// that checkable.
//
// Every move-only kind (`uniq`, List, Map, Coro, closures, and a struct holding
// one) reports None: they are moved, never implicitly duplicated, and their
// explicit `.copy()` is a different, deep operation.
//
// Single-level like the drop plan: WalkFields hands the per-field recursion to
// the emitter, which consults the plan again for each field. (The `is_move_only`
// probe inside the struct arm still recurses through `noncopyable()`, exactly as
// the drop side does.)
//
// Consumed by `emit_value_retain` / `emit_struct_clone_glue` at every
// duplication site, and by the `member_needs_retain` gate below.
enum class RetainKind {
    None,        // trivial: duplication is a plain memcpy
    StrRetain,   // string: owner++ (no-op on an immortal literal)
    RefInc,      // ref: another counted borrow
    WalkFields,  // copyable struct: retain each field that needs it
};
struct RetainPlan {
    RetainKind kind = RetainKind::None;
    Type* struct_type = nullptr;  // WalkFields: the struct whose fields to walk
};
RetainPlan compute_retain_plan(Type* type);

// Whether a value stored in an *opaque member slot* — a container element/value
// or a struct field — needs a cleanup action on the container/struct's teardown.
// Used by the synthetic-destructor pass, the struct field-walk, and both
// backends' container drops.
//
// DERIVED from compute_drop_plan rather than restated, so the "does this need
// dropping?" gate and the "what drop does it get?" lowering can never disagree.
// They did: this was hand-written as `noncopyable() || Ref`, which omits
// `string` — reference-counted but *copyable*, so noncopyable() is false —
// while compute_drop_plan has always returned StrRelease for it and both
// backends have always lowered that correctly. The gate was the only thing
// saying no, so every struct holding a `string` field leaked it.
//
// `free_obj` is part of the test: a `uniq` of a destructor-less value plans
// DropKind::None but still has a heap object to free.
//
// Still NON-recursive, on purpose: compute_drop_plan is a single-level
// decision, and a nested value-struct that owns something already carries its
// own synthesized destructor (propagated by the synthetic-destructor fixpoint).
// A recursive form would not terminate on a direct value cycle.
//
// `DropKind::StrRelease` used to be excluded here, which is why a `string`
// struct field leaked: this gate said no while both backends were perfectly able
// to lower the release. It could not simply be included, because move-only-ness
// was derived from "has a default destructor" — so admitting the release earned
// the struct a destructor and made it move-only in the same step, which broke
// ordinary code. Admitting it *without* that, but also without retain-on-copy,
// is worse still: two owners, two releases, use-after-free.
//
// Both preconditions now hold. Move-only is derived structurally
// (`derive_struct_move_only`), so a string-bearing struct earns a destructor and
// stays copyable; and every duplication site acquires a matching count —
// `emit_struct_clone_glue` for structs, `emit_value_retain` at `List.push` and
// the map value store — all keyed on *this* predicate, so the acquiring and
// releasing halves cannot drift apart.
inline bool member_needs_drop(Type* t) {
    if (!t) return false;
    DropPlan plan = compute_drop_plan(t);
    return plan.kind != DropKind::None || plan.free_obj;
}

// Whether storing a value of this type into a second owner — a container
// element, a struct field, a global — must ACQUIRE a count for that owner.
//
// True exactly when the value drops and its drop has an inverse. The two halves
// matter equally: without a drop there is nothing to balance, and a drop with no
// inverse (`uniq`, a container, a coroutine) belongs to a move-only type, whose
// value is transferred rather than duplicated. So this is the complement of
// move-only among the types that carry drop glue, and it is the acquisition
// counterpart of `member_needs_drop` — the pair is what keeps a container's
// pushes and its teardown exact inverses.
inline bool member_needs_retain(Type* t) {
    return member_needs_drop(t) && compute_retain_plan(t).kind != RetainKind::None;
}

// Whether a map KEY of this type carries a drop the container must run on
// teardown. Two kinds do, and they get there differently: a MOVE-ONLY key (a
// struct with a destructor) is moved into the map, so nothing was acquired and
// teardown simply destroys it; a `string` key is COUNTED, acquired by the
// runtime on insert and released on remove/clear, so teardown releases.
//
// A *copyable struct* key holding a counted member is deliberately neither. The
// runtime cannot walk it to acquire, and such a key could never match on lookup
// anyway — `map_keys_equal` compares key bytes, so two equal strings at
// different addresses miss. See `map_key_is_counted` in roxy_rt.cpp.
//
// Named rather than spelled out at each backend: the element gate is the shared
// `member_needs_drop`, and the key gate had drifted into a raw disjunction
// restated in both lowerings — the dual derivation this file exists to prevent.
inline bool map_key_needs_drop(Type* key_type) {
    return key_type && (key_type->noncopyable() || key_type->kind == TypeKind::String);
}

// True if `type` has a *default* (unnamed) destructor — user-written or
// synthesized. Only a default destructor chains automatically (named ones are
// invoked explicitly), so this is the shared "is there something to chain to /
// call here" test used by sema's synthetic-destructor pass, the IR builder,
// coroutine lowering, and bytecode lowering.
inline bool struct_has_default_dtor(const Type* type) {
    if (!type || !type->is_struct()) return false;
    for (const auto& dtor : type->struct_info.destructors) {
        if (dtor.name.empty()) return true;
    }
    return false;
}

// String representation of types (for error messages)
const char* type_kind_to_string(TypeKind kind);
void type_to_string(const Type* type, String& out);

// Number of 32-bit value-stack slots a value of `type` occupies. Small primitives
// widen to 1 slot; 64-bit primitives and pointers (i64/u64/f64, string, uniq, ref,
// list, map, coroutine, function-closure) take 2; weak refs take 4 (64-bit pointer
// + 64-bit generation); structs use their computed struct_info.slot_count. Returns
// 0 for null or types with no value representation (void, never, trait, ...).
u32 get_type_slot_count(Type* type);

// Append one entry to a bump-allocated Span list on StructTypeInfo. Each call
// rebuilds the span into fresh arena memory (O(n) copy) — fine at
// declaration-pass rates; see TODO.md for the Vector-freeze alternative.
// Shared by the semantic analyzer and the trait system.
void append_method(BumpAllocator& allocator, StructTypeInfo& info, MethodInfo method);
void append_constructor(BumpAllocator& allocator, StructTypeInfo& info, ConstructorInfo ctor);
void append_destructor(BumpAllocator& allocator, StructTypeInfo& info, DestructorInfo dtor);

// True if `info` has any field — regular or when-clause variant — whose type
// needs dropping (member_needs_drop). This is the condition under which a
// struct receives a synthetic default destructor; shared by the whole-program
// synthetic-destructor pass and both generic-instance resolution paths.
bool struct_needs_synthetic_dtor(const StructTypeInfo& info);

// Derive `info.is_move_only` and mark it derived. Returns whether the flag
// changed, so a caller can drive this to a fixpoint across a whole program —
// embedding a struct that later turns out to be move-only makes the embedder
// move-only too, and declaration order says nothing about which is seen first.
//
//   is_move_only(S) = S (or an ancestor) has a USER-WRITTEN default destructor
//                  || any field, regular or variant, is of a move-only type
//
// The user-written destructor is the one non-structural term, and it is a
// deliberate escape hatch: a `fun delete S()` has a body with arbitrary effects,
// and running it twice for one logical value is exactly what move-only prevents.
// A *synthetic* destructor is not such a term — it only ever releases what the
// fields hold, so it says nothing beyond what the fields already say. Reading it
// as one is the conflation this replaces.
//
// Must be called before anything asks `noncopyable()` about the struct; that
// predicate asserts on the flag being derived rather than defaulting to
// "copyable", which is the unsound answer.
bool derive_struct_move_only(StructTypeInfo& info);

// Append a synthetic default destructor (empty name, no params, decl == null)
// to `info`. Does NOT guard against an existing default destructor — callers
// reachable more than once check that first.
void add_synthetic_default_dtor(BumpAllocator& allocator, StructTypeInfo& info);

// Look up a method in a struct's type hierarchy (walks inheritance chain)
// Returns the MethodInfo and optionally sets found_in_type to where the method was defined
const MethodInfo* lookup_method_in_hierarchy(Type* struct_type, StringView name, Type** found_in_type = nullptr);

// Check if 'child' is a subtype of 'parent' (walks inheritance chain)
bool is_subtype_of(Type* child, Type* parent);

// Look up a method in a list type's builtin methods
const MethodInfo* lookup_list_method(const ListTypeInfo& info, StringView name);

// Look up a method in a map type's builtin methods
const MethodInfo* lookup_map_method(const MapTypeInfo& info, StringView name);

// Look up a method in a coroutine type's builtin methods
const MethodInfo* lookup_coro_method(const CoroutineTypeInfo& info, StringView name);

}
