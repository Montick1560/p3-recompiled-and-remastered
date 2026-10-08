// games/patapon3/runtime/hooks_main.cpp — Patapon 3 game module.
//
// Registers no hooks yet (behaves like the generic build); exists so the
// runtime's module id matches the manifest id "patapon3". Hooks are added
// only when bring-up demands them (overlay banks arrive in M2, generically).

#include "psp_game_module.h"

namespace {

void patapon3_register_hooks(uint8_t* /*rdram*/) {}
void patapon3_on_boot_context(uint8_t* /*rdram*/, recomp_context* /*ctx*/) {}
void patapon3_on_thread_start(uint8_t* /*rdram*/, uint32_t /*k0_addr*/) {}

const PspGameModule g_patapon3_module = {
    /* id              */ "patapon3",
    /* register_hooks  */ patapon3_register_hooks,
    /* on_boot_context */ patapon3_on_boot_context,
    /* on_thread_start */ patapon3_on_thread_start,
};

}  // namespace

const PspGameModule* psp_game_module() {
    return &g_patapon3_module;
}
