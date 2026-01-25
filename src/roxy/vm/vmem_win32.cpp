#ifdef _WIN32

#include "roxy/vm/vmem.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace rx {

void* VirtualMemoryOps::reserve(u64 size) {
    return VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_NOACCESS);
}

bool VirtualMemoryOps::commit(void* addr, u64 size) {
    return VirtualAlloc(addr, size, MEM_COMMIT, PAGE_READWRITE) != nullptr;
}

bool VirtualMemoryOps::decommit(void* addr, u64 size) {
    return VirtualFree(addr, size, MEM_DECOMMIT) != 0;
}

void VirtualMemoryOps::release(void* addr, u64 size) {
    (void)size;  // Windows ignores size for MEM_RELEASE
    VirtualFree(addr, 0, MEM_RELEASE);
}

bool VirtualMemoryOps::remap_to_zero(void* addr, u64 size) {
    // MEM_RESET marks pages as discardable - physical memory is released
    // and pages return zeros when accessed (zero-on-demand)
    VirtualAlloc(addr, size, MEM_RESET, PAGE_READWRITE);
    DWORD old_protect;
    return VirtualProtect(addr, size, PAGE_READONLY, &old_protect) != 0;
}

u64 VirtualMemoryOps::page_size() {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return static_cast<u64>(si.dwPageSize);
}

}

#endif // _WIN32
