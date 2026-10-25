#pragma once
// The game window's title. A build names its game with the CMake cache entry PSPRECOMP_WINDOW_TITLE
// (e.g. "Patapon 3 DxD Remastered", compiled into psp_event_loop.cpp); otherwise the generic title.
#ifndef PSPRECOMP_WINDOW_TITLE
#define PSPRECOMP_WINDOW_TITLE "PSPrecomp Runtime"
#endif

inline const char* psp_window_title() { return PSPRECOMP_WINDOW_TITLE; }
