#pragma once
// Network imports (psp_hle_net.cpp) and the ad hoc settings they use.
#include "net/adhoc_config.h"

const AdhocConfig& psp_net_config();
// Points the PDP sockets at the current adhocctl peers, bind address and port offset.
void psp_net_wire_pdp();
void psp_hle_register_net();
void psp_hle_register_net_matching();
// Guest event handler of a matching context (0 if none): diagnostics/tests.
uint32_t psp_net_matching_handler(int id);
