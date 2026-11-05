#pragma once
// Mission effect actors (FUN_088D3AB4) paced to 30 Hz at 60 fps, chosen by effect name. Recon:
// build/autopilot/LEDGER_effect_select_2026-11-04.md.

// `field`: the actor's 16-byte effect-name field (descriptor a0+0x344, resource at descriptor+0x14, name at
// resource-0x100): the name's last 15 characters and an 'x', NUL-padded, not NUL-terminated when full.
// True for the effects that need pacing: Pyokojinete's hero ghost riders and Wooyari's hero wheel.
bool p3_effect_paced_name(const char* field);
