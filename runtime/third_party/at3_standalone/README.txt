at3_standalone - standalone ATRAC3 / ATRAC3plus decoder
=======================================================

Vendored from the PPSSPP project, ext/at3_standalone
(https://github.com/hrydgard/ppsspp), which derives from FFmpeg's
libavcodec atrac3.c / atrac3plus*.c / atrac.c (Copyright (c) 2006-2008
Maxim Poliakovski, Benjamin Larsson, 2010-2013 Maxim Poliakovski, and
others as stated in each file header).

License: GNU Lesser General Public License, version 2.1 or (at your
option) any later version. The per-file license headers are kept
unchanged. The LGPL-2.1+ text is at
https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html .

External API: see at3_decoders.h (atrac3_alloc / atrac3p_alloc and the
matching *_decode_frame / *_flush_buffers / *_free functions).

Modifications for psprecomp (everything else is verbatim, only line
endings were normalised to LF):
  - at3_arch.h (new): local replacement for PPSSPP's ppsspp_config.h
    (atrac3plusdsp.cpp includes it instead).
  - mem.cpp: PPSSPP's AllocateAlignedMemory/FreeAlignedMemory replaced by
    _aligned_malloc/_aligned_free (Windows) or posix_memalign/free.
  - compat.cpp: PPSSPP's Common/Log.h logging replaced by a stderr sink
    that is silent unless PSPRECOMP_ATRAC_LOG is set.
  - aac_defines.h (unused by the decoder, pulls in libavutil) is not
    vendored.
