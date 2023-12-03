#include "roxy/opcode.hpp"

namespace rx {

const char* g_opcode_str[(u32)OpCode::_count] = {
#define X(val) #val,
    OPCODE_LIST(X)
#undef X
};

}