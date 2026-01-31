#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/span.hpp"
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
    Array,
    Function,
    Struct,
    Enum,

    // Reference wrappers
    Uniq,
    Ref,
    Weak,

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

// Type info for struct types
struct StructTypeInfo {
    StringView name;
    StringView module_name;        // Module that defined this struct (for visibility checking)
    Decl* decl;                    // Points to the StructDecl AST node
    Type* parent;                  // Parent struct type, nullptr if no inheritance
    Span<struct FieldInfo> fields; // All fields including inherited
    Span<ConstructorInfo> constructors;  // Constructors for this struct
    Span<DestructorInfo> destructors;    // Destructors for this struct
    u32 slot_count;                // Total u32 slots needed for this struct
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

// Type info for enum types
struct EnumTypeInfo {
    StringView name;
    Decl* decl;           // Points to the EnumDecl AST node
    Type* underlying;     // Underlying integer type (defaults to i32)
};

// Type info for array types
struct ArrayTypeInfo {
    Type* element_type;
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

// The main Type structure - a tagged union
struct Type {
    TypeKind kind;

    union {
        StructTypeInfo struct_info;
        EnumTypeInfo enum_info;
        ArrayTypeInfo array_info;
        FunctionTypeInfo func_info;
        RefTypeInfo ref_info;
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

    bool is_array() const {
        return kind == TypeKind::Array;
    }

    bool is_function() const {
        return kind == TypeKind::Function;
    }

    bool is_nil() const {
        return kind == TypeKind::Nil;
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

    // Factory methods for compound types (with interning)
    Type* array_type(Type* element_type);
    Type* function_type(Span<Type*> param_types, Type* return_type);
    Type* uniq_type(Type* inner_type);
    Type* ref_type(Type* inner_type);
    Type* weak_type(Type* inner_type);

    // Factory methods for named types (not interned - unique per declaration)
    Type* struct_type(StringView name, Decl* decl, StringView module_name = StringView(nullptr, 0));
    Type* enum_type(StringView name, Decl* decl, Type* underlying = nullptr);

    // Lookup primitive type by name
    Type* primitive_by_name(StringView name);

    // Named type registration and lookup (for structs and enums)
    void register_named_type(StringView name, Type* type);
    Type* named_type_by_name(StringView name);

    // Lookup any type by name (primitives first, then named types)
    Type* type_by_name(StringView name);

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

    // Type interning cache for compound types
    tsl::robin_map<Type*, Type*, TypeHash, TypeEqual> m_interned;

    // Named type registry (structs and enums)
    tsl::robin_map<StringView, Type*, StringViewHash, StringViewEqual> m_named_types;
};

// String representation of types (for error messages)
const char* type_kind_to_string(TypeKind kind);
void type_to_string(const Type* type, Vector<char>& out);

}
