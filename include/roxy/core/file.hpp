#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/vector.hpp"
#include "roxy/core/bump_allocator.hpp"

namespace rx {

bool read_file_to_buf(const char* path, u8*& buf, BumpAllocator& bump_allocator);
bool read_file_to_buf(const char* path, Vector<u8>& buf);

}