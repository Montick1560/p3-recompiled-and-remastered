#pragma once
// sceNp / sceNpAuth / sceNpService / sceNetApctl / sceNpMatching2
// (infrastructure mode, over tools/infra_server.py).
#include <string>

void psp_hle_register_np();
void psp_hle_register_np_matching2();

// The player's online id: PSPRECOMP_ADHOC_NICK (or "Patapon") reduced to
// the 3-16 [A-Za-z0-9_-] the server accepts.
std::string psp_np_online_id();
// Connects to the infrastructure server in the background (the netconf
// dialog waits on it); psp_infra_connecting() is true until that attempt ends.
void psp_infra_connect_async();
bool psp_infra_connecting();
