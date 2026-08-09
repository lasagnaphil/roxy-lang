#include "roxy/core/file.hpp"

#ifdef _WIN32
#ifndef WIN32_MEAN_AND_LEAN
#define WIN32_MEAN_AND_LEAN
#endif
#include "Windows.h"
#else
#include <cstdio>
#endif

namespace rx {

#ifndef _WIN32
// Measure an open stream and rewind it. Returns false if the stream is not
// seekable: ftell yields -1 there (a fifo, say), and storing that in an
// unsigned size_t underflows to SIZE_MAX — which makes the `size + 1`
// allocation below a 0-byte one and the `buf[size] = 0` terminator a wild
// write. fseek is checked for the same reason.
static bool stream_size(FILE* file, size_t& out_size) {
    if (fseek(file, 0L, SEEK_END) != 0)
        return false;
    long end = ftell(file);
    if (end < 0)
        return false;
    if (fseek(file, 0L, SEEK_SET) != 0)
        return false;
    out_size = static_cast<size_t>(end);
    return true;
}
#endif

bool read_file_to_buf(const char* path, u8*& buf, BumpAllocator& bump_allocator) {
#ifdef _WIN32
    HANDLE file = CreateFile(path, GENERIC_READ, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD file_size = GetFileSize(file, NULL);
    if (file_size == INVALID_FILE_SIZE) {
        CloseHandle(file);
        return false;
    }
    buf = bump_allocator.alloc_bytes(file_size + 1, 4);
    if (!ReadFile(file, buf, file_size, NULL, NULL)) {
        CloseHandle(file);
        return false;
    }
    buf[file_size] = 0;
    CloseHandle(file);
#else
    FILE* file = fopen(path, "rb");
    if (!file) {
        return false;
    }

    size_t file_size = 0;
    if (!stream_size(file, file_size)) {
        fclose(file);
        return false;
    }

    buf = bump_allocator.alloc_bytes(file_size + 1, 8);
    size_t bytes_read = fread(buf, sizeof(char), file_size, file);
    if (bytes_read < file_size) {
        fclose(file);
        return false;
    }
    buf[file_size] = 0;

    fclose(file);
#endif

    return true;
}

bool read_file_to_buf(const char* path, Vector<u8>& buf) {
#ifdef _WIN32
    HANDLE file = CreateFile(path, GENERIC_READ, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD file_size = GetFileSize(file, NULL);
    if (file_size == INVALID_FILE_SIZE) {
        CloseHandle(file);
        return false;
    }
    buf.resize(file_size + 1);
    if (!ReadFile(file, buf.data(), file_size, NULL, NULL)) {
        CloseHandle(file);
        return false;
    }
    buf[file_size] = 0;
    CloseHandle(file);
#else
    FILE* file = fopen(path, "rb");
    if (!file) {
        return false;
    }

    size_t file_size = 0;
    if (!stream_size(file, file_size)) {
        fclose(file);
        return false;
    }

    buf.resize(file_size + 1);
    size_t bytes_read = fread(buf.data(), sizeof(char), file_size, file);
    if (bytes_read < file_size) {
        fclose(file);
        return false;
    }
    buf[file_size] = 0;

    fclose(file);
#endif

    return true;
}

} // namespace rx