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
    u32 slot_count;                // Total u32 slots needed for this struct

    // Find a field by name, returns nullptr if not found
    const FieldInfo* find_field(StringView field_name) const;

    // Find a variant field by name in any when clause
    // Returns nullptr if not found, sets out_clause and out_variant if found
    const VariantFieldInfo* find_variant_field(StringView field_name,
                                                const WhenClauseInfo** out_clause = nullptr,
                                                const VariantInfo** out_variant = nullptr) const;
};

// Type info for enum types
struct EnumTypeInfo {
    StringView name;
    Decl* decl;           // Points to the EnumDecl AST node
    Type* underlying;     // Underlying integer type (defaults to i32)
    Span<MethodInfo> methods;  // Builtin methods (eq, ne)
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

    bool is_exception_ref() const {
        return kind == TypeKind::ExceptionRef;
    }

    // Returns true for noncopyable types (require move semantics).
    // This includes:
    //   - uniq references
    //   - structs with a default destructor (synthetic or user-defined)
    bool noncopyable() const {
        if (kind == TypeKind::Uniq) return true;
        if (kind == TypeKind::Struct) {
            for (const auto& dtor : struct_info.destructors) {
                if (dtor.name.empty()) return true;
            }
        }
        return false;
    }

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

    // Unified lookup: works for structs (via hierarchy) AND primitives
    const MethodInfo* lookup_method(Type* type, StringView name, Type** found_in = nullptr) const;
    bool implements_trait(Type* type, Type* trait) const;
    bool implements_trait(Type* type, Type* trait, Span<Type*> type_args) const;

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
    Type* m_exception_ref;

    // Type interning cache for compound types
    tsl::robin_map<Type*, Type*, TypeHash, TypeEqual> m_interned;

    // Primitive method and trait tables (keyed by TypeKind)
    tsl::robin_map<u8, Vector<MethodInfo>> m_primitive_methods;
    tsl::robin_map<u8, Vector<Type*>> m_primitive_traits;
};

// String representation of types (for error messages)
const char* type_kind_to_string(TypeKind kind);
void type_to_string(const Type* type, String& out);

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
