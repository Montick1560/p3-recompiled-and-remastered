#pragma once

#include <cstdint>
#include <cstddef>

/// Start a TCP debug socket server on the given port (loopback only).
/// Runs a background thread that accepts one client at a time and
/// serves raw rdram bytes in response to R commands.
///
/// Protocol: client sends "R <hex_addr> <decimal_size>\n"
///           server replies with <size> raw bytes from rdram,
///           address masked with 0x07FFFFFFU.
///
/// @param rdram      Pointer to the 128MB PSP memory buffer.
/// @param rdram_size Size of rdram in bytes (PSP_MEM_SIZE = 0x08000000).
/// @param port       TCP port to listen on (default 9999).
void psp_debug_socket_start(uint8_t* rdram, size_t rdram_size, int port);

/// Stop the debug socket server and join the background thread.
/// Safe to call even if psp_debug_socket_start was never called.
void psp_debug_socket_stop();
