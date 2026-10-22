#pragma once
// sceNetInet: the game's UDP socket on the infrastructure server's virtual network.
void psp_hle_register_inet();
// No stream segment waits in the client and no received stream byte waits for the game.
bool psp_inet_streams_idle();
