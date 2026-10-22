#pragma once
// sceNetInet: the game's UDP socket on the infrastructure server's virtual network.
void psp_hle_register_inet();
// Nothing received waits for the game: no stream segment or unread stream byte, no queued datagram.
bool psp_inet_streams_idle();
