#include "roxy/type.hpp"

namespace rx {

// Array of primitive type sizes indexed by PrimTypeKind
// Order must match the PrimTypeKind enum in type.hpp
u16 PrimitiveType::s_prim_type_sizes[] = {
    0,  // Void (0)
    1,  // Bool (1)
    1,  // U8 (2)
    2,  // U16 (3)
    4,  // U32 (4)
    8,  // U64 (5)
    1,  // I8 (6)
    2,  // I16 (7)
    4,  // I32 (8)
    8,  // I64 (9)
    4,  // F32 (10)
    8,  // F64 (11)
    8,  // String (12)
};

}
