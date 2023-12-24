#include "roxy/type.hpp"

namespace rx {
u16 PrimitiveType::s_prim_type_sizes[] = {
    [(u32)PrimTypeKind::Void] = 0,
    [(u32)PrimTypeKind::Bool] = 1,
    [(u32)PrimTypeKind::I8] = 1,
    [(u32)PrimTypeKind::I16] = 2,
    [(u32)PrimTypeKind::I32] = 4,
    [(u32)PrimTypeKind::I64] = 8,
    [(u32)PrimTypeKind::U8] = 1,
    [(u32)PrimTypeKind::U16] = 2,
    [(u32)PrimTypeKind::U32] = 4,
    [(u32)PrimTypeKind::U64] = 8,
    [(u32)PrimTypeKind::F32] = 4,
    [(u32)PrimTypeKind::F64] = 8,
    [(u32)PrimTypeKind::String] = 8,
};
}
