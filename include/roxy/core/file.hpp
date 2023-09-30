#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/vector.hpp"

namespace rx {

bool read_file_to_buf(const char* path, Vector<u8>& buf);

}