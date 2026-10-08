# Platform Support

This page states, factually, where psprecomp is known to work today. The
project is developed on macOS; Windows (MinGW clang) builds and runs; Linux
has never been run. See [ARCHITECTURE.md](../ARCHITECTURE.md) for the structure these
notes refer to and [README.md](../README.md) for build/run instructions.

## Summary

| Platform      | Rust pipeline (analyze/recompile) | C++ runtime (build + run)        |
|---------------|-----------------------------------|----------------------------------|
| macOS         | Verified                          | Verified                         |
| Linux         | Expected to work (untested)       | Likely portable, untested        |
| Windows       | Verified (Ghidra via `.bat`)      | Builds, unit tests pass, boots   |

"Verified" means it is the platform on which the project is actively built and
run. "Untested" means exactly that — it has not been tried, not that it is
known to fail.

## What is platform-independent

The two halves of the project have very different platform exposure.

- **Rust pipeline** (`crates/psp-*`): pure Rust with no OS-specific code paths
  in the hot path. `analyze` shells out to a Ghidra headless install (the only
  external dependency); `recompile` reads `analysis.json` and emits C++ text.
  Continuous integration builds and unit-tests this half on `ubuntu-latest`
  (see [`.github/workflows/ci.yml`](../.github/workflows/ci.yml)), so the Rust
  crates are exercised on Linux on every push, even though the project is
  otherwise macOS-only.
- **Generated C++ output** (`output/`): standard C++17. It carries no
  platform assumptions of its own; portability is determined by the runtime
  that links it.

## macOS (verified)

This is the reference platform. Both the Rust pipeline and the C++ runtime are
built and run here.

- **SDL2** is located via `pkg-config` (`pkg_check_modules(SDL2 REQUIRED ...)`
  in `runtime/CMakeLists.txt`), matching a Homebrew `sdl2` install. This is a
  deliberate choice over CMake's `find_package(SDL2)` because the Homebrew
  package ships a `.pc` file but not always SDL2's CMake config.
- **OpenGL 3.3 core profile**: the runtime requests a 3.3 core context
  (`SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR/MINOR_VERSION, 3, 3)` in
  `runtime/src/psp_event_loop.cpp`) and loads function pointers with GLAD2.
- **Main-thread GL via the render queue**: macOS requires all OpenGL calls to
  happen on the thread that created the context (the main thread). The runtime
  is built around this: game threads never call GL directly — they post
  requests onto a condvar-based render queue that the main thread drains
  (`runtime/src/psp_render_queue.cpp`, `runtime/src/psp_event_loop.cpp`). A
  debug-build assertion `GL_THREAD_CHECK()` (`runtime/include/psp_runtime.h`)
  aborts if a GL call is ever made off the main thread.

## Linux (likely portable, untested)

Nothing in the design is intentionally macOS-only, but the runtime has never
been built or run on Linux, so this is unverified.

- **Rust pipeline**: expected to build and test the same as on macOS; CI
  already builds the workspace on Linux. The `analyze` step still needs a
  Ghidra install with the ghidra-allegrex processor extension.
- **SDL2 / OpenGL**: `pkg_check_modules(SDL2 ...)` and `find_package(OpenGL)`
  should resolve against a distro `libsdl2-dev` + Mesa/driver GL the same way
  they do on macOS; `zlib` is already discovered via `find_package(ZLIB)`,
  noted in the CMake as pkg-config-discoverable on Linux.
- **Assumptions to revisit before claiming Linux support**:
  - The main-thread-GL constraint that shaped the render queue is a macOS
    requirement; on Linux it is not strictly necessary, but the render-queue
    architecture is correct there too — it would simply be stricter than
    required, not wrong.
  - `GL_SILENCE_DEPRECATION` is defined to quiet macOS's OpenGL deprecation
    warnings; it is a no-op elsewhere but signals the macOS focus.
  - The GL 3.3 core context request must be honored by the Linux GL driver;
    Mesa supports it, but this has not been exercised.

## Windows (MinGW clang)

Built and run on Windows 11 with the llvm-mingw clang toolchain (UCRT), Ninja
and Git Bash. The Rust pipeline needs no changes beyond launching Ghidra's
`support/analyzeHeadless.bat` and using the OS temp dir for its project.
Everything Windows-specific in the runtime is behind `#ifdef _WIN32` /
`if(WIN32)`, so the POSIX paths are unchanged.

- **SDL2**: the official `SDL2-devel-<ver>-mingw` package, found through its
  CMake config package (`-DSDL2_DIR=<pkg>/x86_64-w64-mingw32/lib/cmake/SDL2`);
  SDL2main is not linked (`SDL_MAIN_HANDLED`). Copy `SDL2.dll` next to the exe.
- **zlib**: MinGW toolchains do not ship it; CMake builds it from source via
  FetchContent (`-DFETCHCONTENT_SOURCE_DIR_ZLIB=<zlib-1.3.1>` for an offline
  tree).
- **Sockets**: the debug socket uses Winsock2 through
  `runtime/include/psp_socket_compat.h` (links `ws2_32`). `stop()` closes the
  client sockets on Windows because `shutdown()` does not wake a blocked
  `recv()` there.
- **Memory / files / signals**: guest RAM comes from `VirtualAlloc`; host files
  are opened with `O_BINARY` (text mode would cut reads at 0x1A and translate
  CRLF); `std::signal` replaces `sigaction`.
- **pthreads**: provided by MinGW's winpthreads; `<unistd.h>`, `<dirent.h>` and
  `<fcntl.h>` come from the MinGW headers.
- **Smart App Control**: when enabled it may block freshly linked unsigned
  executables; relinking produces a new file that is usually allowed.
- **Not yet ported**: the `patapon` (Patapon 1) game module still uses `mmap`
  and opens files without `O_BINARY`; build other games with their own module
  or `-DPSPRECOMP_GAME=none`.

Example configure (paths are illustrative):

```bash
cmake -G Ninja -B build/rt -S runtime -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DPSPRECOMP_GAME=none -DPSPRECOMP_OUTPUT_DIR=/abs/path/to/output \
  -DSDL2_DIR=/path/SDL2-2.32.10/x86_64-w64-mingw32/lib/cmake/SDL2 \
  -DFETCHCONTENT_SOURCE_DIR_ZLIB=/path/zlib-1.3.1
```

## Contributing platform support

Porting to Linux is welcome. The Rust pipeline is the easy half (it already
runs in Linux CI); the work is in the C++ runtime's SDL2/GL/threading
assumptions listed above. The honest state is that macOS has been exercised end
to end, Windows through boot of Patapon 3, and Linux not at all.
