#include "psp_memory.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <cstdio>

uint8_t* psp_memory_init() {
#ifdef _WIN32
    // Reserve+commit; Windows backs committed pages lazily on first touch.
    void* mem = VirtualAlloc(nullptr, PSP_MEM_SIZE, MEM_RESERVE | MEM_COMMIT,
                             PAGE_READWRITE);
    if (!mem) {
        std::fprintf(stderr, "psp_memory_init: VirtualAlloc(%zu bytes) failed\n",
                     PSP_MEM_SIZE);
        return nullptr;
    }
    return static_cast<uint8_t*>(mem);
#else
    // macOS uses MAP_ANON (not MAP_ANONYMOUS).
    // MAP_PRIVATE | MAP_ANON creates a sparse allocation — only touched
    // pages consume physical memory. This avoids committing the full 128MB.
    void* mem = mmap(
        nullptr,
        PSP_MEM_SIZE,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANON,
        -1,
        0
    );

    if (mem == MAP_FAILED) {
        std::fprintf(stderr, "psp_memory_init: mmap(%zu bytes) failed\n",
                     PSP_MEM_SIZE);
        return nullptr;
    }

    return static_cast<uint8_t*>(mem);
#endif
}

void psp_memory_cleanup(uint8_t* rdram) {
    if (rdram) {
#ifdef _WIN32
        VirtualFree(rdram, 0, MEM_RELEASE);
#else
        munmap(rdram, PSP_MEM_SIZE);
#endif
    }
}
