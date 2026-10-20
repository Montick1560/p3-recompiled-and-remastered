#pragma once
// PSP button bindings (pure): up to two keyboard keys and one gamepad button per PSP button, from
// PSPRECOMP_CONTROLS (docs/contracts/settings-v1.md, [controls]). Key and pad names are resolved by
// functions the caller passes in (SDL in the runtime, a small table in the tests).
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

struct ControlAction {
    const char* name;  // contract key prefix ("cross" -> cross_keys / cross_pad)
    uint32_t mask;     // PSP sceCtrl button
    const char* keys;  // default keys, comma separated SDL key names
    const char* pad;   // default SDL GameController button name
};

// The 12 actions in contract order (the order decides who keeps a duplicate), with today's defaults.
const std::vector<ControlAction>& control_actions();

using KeyLookup = std::function<int32_t(const std::string&)>;  // name -> keycode, 0 = unknown
using PadLookup = std::function<int(const std::string&)>;      // name -> button, -1 = unknown

class ControlMap {
public:
    // `spec` is PSPRECOMP_CONTROLS ("action=key1,key2|pad;..."); actions it does not name keep their
    // defaults. An invalid part falls back to that action's default and adds a "[CONTROLS] ..." line.
    static ControlMap build(const std::string& spec, const KeyLookup& key, const PadLookup& pad,
                            std::vector<std::string>* log);
    uint32_t key_mask(int32_t keycode) const;  // 0 when unbound
    uint32_t pad_mask(int button) const;       // 0 when unbound
    std::string describe() const;              // the effective map, canonical form, for the log
    // Reverse lookup for on-screen prompts: the first key the action with this PSP mask kept (its name as
    // configured, e.g. "X", "Return") and its pad button name ("a", "leftshoulder"); "" when none.
    std::string key_name(uint32_t mask) const;
    std::string pad_name(uint32_t mask) const;

private:
    std::map<int32_t, uint32_t> keys_;
    std::map<int, uint32_t> pads_;
    std::vector<std::string> desc_;
    std::map<uint32_t, std::string> first_key_;
    std::map<uint32_t, std::string> pad_name_;
};

// The runtime's effective map (psp_event_loop.cpp): built when SDL starts, before the game runs, and
// read-only after, so any thread may read it.
const ControlMap& psp_control_map();
