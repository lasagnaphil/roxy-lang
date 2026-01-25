#if !defined(_WIN32)

#include "roxy/vm/vmem.hpp"

#include <sys/mman.h>
#include <unistd.h>

namespace rx {

void* VirtualMemoryOps::reserve(u64 size) {
    void* addr = mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return (addr == MAP_FAILED) ? nullptr : addr;
}

bool VirtualMemoryOps::commit(void* addr, u64 size) {
    return mprotect(addr, size, PROT_READ | PROT_WRITE) == 0;
}

bool VirtualMemoryOps::decommit(void* addr, u64 size) {
    // MADV_DONTNEED releases physical pages but keeps the mapping
    return madvise(addr, size, MADV_DONTNEED) == 0;
}

void VirtualMemoryOps::release(void* addr, u64 size) {
    munmap(addr, size);
}

bool VirtualMemoryOps::remap_to_zero(void* addr, u64 size) {
    // Release physical pages (returns zeros on next read) and make read-only
    // MADV_DONTNEED causes the pages to return zeros when read
    madvise(addr, size, MADV_DONTNEED);
    return mprotect(addr, size, PROT_READ) == 0;
}

u64 VirtualMemoryOps::page_size() {
    return static_cast<u64>(sysconf(_SC_PAGESIZE));
}

}

#endif // !defined(_WIN32)
