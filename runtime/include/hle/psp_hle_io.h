#pragma once
#include <cstdint>
#include <string>

// PSP I/O open flags
constexpr int PSP_O_RDONLY   = 0x0001;
constexpr int PSP_O_WRONLY   = 0x0002;
constexpr int PSP_O_RDWR     = 0x0003;
constexpr int PSP_O_APPEND   = 0x0100;
constexpr int PSP_O_CREAT    = 0x0200;
constexpr int PSP_O_TRUNC    = 0x0400;

// PSP I/O seek modes
constexpr int PSP_SEEK_SET = 0;
constexpr int PSP_SEEK_CUR = 1;
constexpr int PSP_SEEK_END = 2;

// PSP fd allocation
constexpr int PSP_FD_BASE = 3;
// Real firmware has PSP_COUNT_FDS = 64 fd slots, 3 of them reserved for
// stdin/stdout/stderr (PPSSPP sceIo.cpp:127-132, 364-374), leaving 61 usable.
// Exhaustion returns SCE_KERNEL_ERROR_MFILE (0x80020320).
constexpr int PSP_FD_MAX  = 61;

struct PspFileDesc {
    int host_fd;
    std::string psp_path;
    bool is_dir;
    void* dir_handle;      // DIR* for directory operations
    int64_t asyncResult = 0;   // Result stored by async ops, read by WaitAsync
    bool asyncPending = false;  // Whether an async op is in flight
    bool closePending = false;  // Whether sceIoCloseAsync was called
    int callbackId = 0;        // Callback UID from sceIoSetAsyncCallback
    int callbackArg = 0;       // Callback argument from sceIoSetAsyncCallback
    // BND slice view: when this fd is a packed-archive asset rerouted into
    // DATA_CMN.BND, the logical "file" is the byte range
    // [slice_base, slice_base+slice_len). All lseek/read operate relative to
    // slice_base, and SEEK_END returns slice_len — so the rerouted fd behaves
    // exactly like a standalone file of the asset's true size. is_slice gates
    // this so plain files are unaffected.
    bool is_slice = false;
    int64_t slice_base = 0;    // host-file offset of the asset's first byte
    int64_t slice_len = 0;     // asset size in bytes
};

// Initialize I/O subsystem
void psp_io_init(const char* disc0_host_path);

// Map PSP path to host path
std::string psp_path_to_host(const char* psp_path);

// Registration
void psp_hle_register_io();
