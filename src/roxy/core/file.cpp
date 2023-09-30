#include "roxy/core/file.hpp"


#ifdef _WIN32
#ifndef WIN32_MEAN_AND_LEAN
#define WIN32_MEAN_AND_LEAN
#endif
#include "Windows.h"
#endif

namespace rx {

bool read_file_to_buf(const char* path, Vector<u8>& buf) {
#ifdef _WIN32
    HANDLE file = CreateFile(path, GENERIC_READ, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    uint32_t file_size = GetFileSize(file, NULL);
    buf.resize(file_size + 1);
    if (!ReadFile(file, buf.data(), file_size, NULL, NULL)) {
        CloseHandle(file);
        return false;
    }
    buf[file_size] = 0;
    CloseHandle(file);
#else
    FILE* file;
    if (fopen_s(&file, path, "rb") != 0) {
        return false;
    }

    fseek(file, 0L, SEEK_END);
    size_t file_size = ftell(file);
    rewind(file);

    buf.resize(file_size + 1);
    size_t bytes_read = fread(buf.data(), sizeof(char), file_size, file);
    if (bytes_read < file_size) {
        return false;
    }
    buf[file_size] = 0;

    fclose(file);
#endif

    return true;
}

}