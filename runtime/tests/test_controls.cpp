// Unit tests for psp_controls (docs/contracts/settings-v1.md [controls]): defaults equal the old fixed
// tables, PSPRECOMP_CONTROLS parsing, invalid / reserved / duplicate entries.
#include "psp_controls.h"
#include "psp_pad.h"

#include <cstdio>
#include <map>
#include <string>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

// A tiny stand-in for SDL's name tables (keycodes are arbitrary but distinct).
static const std::map<std::string, int32_t> kKeys = {
    {"up", 1}, {"down", 2}, {"left", 3}, {"right", 4}, {"x", 5}, {"return", 6}, {"z", 7}, {"a", 8},
    {"s", 9}, {"q", 10}, {"w", 11}, {"space", 12}, {"v", 13}, {"tab", 14}, {"k", 15}, {"f1", 16},
    {"f11", 17}, {"left shift", 18}, {"j", 19}, {"l", 20}};
static const std::map<std::string, int> kPads = {
    {"a", 0}, {"b", 1}, {"x", 2}, {"y", 3}, {"back", 4}, {"guide", 5}, {"start", 6},
    {"leftstick", 7}, {"rightstick", 8}, {"leftshoulder", 9}, {"rightshoulder", 10},
    {"dpup", 11}, {"dpdown", 12}, {"dpleft", 13}, {"dpright", 14}};
static std::string lower(std::string s) { for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }
static const KeyLookup kKey = [](const std::string& n) { auto it = kKeys.find(lower(n)); return it == kKeys.end() ? 0 : it->second; };
static const PadLookup kPad = [](const std::string& n) { auto it = kPads.find(lower(n)); return it == kPads.end() ? -1 : it->second; };

static bool logged(const std::vector<std::string>& log, const char* needle) {
    for (const auto& l : log) if (l.find(needle) != std::string::npos) return true;
    return false;
}

static void defaults_are_todays_mapping() {
    std::vector<std::string> log;
    const ControlMap m = ControlMap::build("", kKey, kPad, &log);
    CHECK(log.empty(), "no log for the defaults");
    CHECK(m.key_mask(1) == 0x0010 && m.key_mask(2) == 0x0040 && m.key_mask(3) == 0x0080 && m.key_mask(4) == 0x0020, "arrows");
    CHECK(m.key_mask(5) == 0x4000 && m.key_mask(6) == 0x4000, "X and Return = cross");
    CHECK(m.key_mask(7) == 0x2000 && m.key_mask(8) == 0x8000 && m.key_mask(9) == 0x1000, "Z A S");
    CHECK(m.key_mask(10) == 0x0100 && m.key_mask(11) == 0x0200, "Q W");
    CHECK(m.key_mask(12) == 0x0008 && m.key_mask(13) == 0x0001 && m.key_mask(14) == 0x0001, "Space V Tab");
    CHECK(m.key_mask(15) == 0, "K unbound");
    for (int b = 0; b <= 20; b++) CHECK(m.pad_mask(b) == pad_button_to_psp(b), "pad default equals pad_button_to_psp");
    CHECK(m.describe().rfind("up=Up|dpup;down=Down|dpdown;", 0) == 0, "describe starts in contract order");
}

static void spec_replaces_named_actions_only() {
    std::vector<std::string> log;
    const ControlMap m = ControlMap::build("cross=K|y;triangle=S|", kKey, kPad, &log);
    CHECK(log.empty(), "valid spec, no log");
    CHECK(m.key_mask(15) == 0x4000, "K is cross");
    CHECK(m.key_mask(5) == 0 && m.key_mask(6) == 0, "X / Return no longer cross");
    CHECK(m.pad_mask(3) == 0x4000, "pad y is cross");
    CHECK(m.pad_mask(0) == 0, "pad a unbound now");
    CHECK(m.key_mask(9) == 0x1000, "triangle keeps S");
    CHECK(m.key_mask(7) == 0x2000, "circle untouched");
}

static void invalid_entries_fall_back() {
    std::vector<std::string> log;
    ControlMap m = ControlMap::build("cross=Nope|a", kKey, kPad, &log);
    CHECK(m.key_mask(5) == 0x4000 && m.key_mask(6) == 0x4000, "unknown key name -> default keys");
    CHECK(logged(log, "[CONTROLS] cross"), "logged");
    log.clear();
    m = ControlMap::build("cross=X,Return,K|a", kKey, kPad, &log);
    CHECK(m.key_mask(15) == 0 && m.key_mask(5) == 0x4000, "third key -> defaults");
    CHECK(logged(log, "more than two keys"), "too many keys logged");
    log.clear();
    m = ControlMap::build("start=F1|guide", kKey, kPad, &log);
    CHECK(m.key_mask(16) == 0 && m.key_mask(12) == 0x0008, "F1 is reserved");
    CHECK(m.pad_mask(5) == 0 && m.pad_mask(6) == 0x0008, "guide is reserved");
    CHECK(logged(log, "reserved"), "reserved logged");
    log.clear();
    m = ControlMap::build("cross=X|nope;garbage;bogus=K|a", kKey, kPad, &log);
    CHECK(m.pad_mask(0) == 0x4000, "unknown pad -> default pad");
    CHECK(logged(log, "bogus") && logged(log, "garbage"), "unknown action and malformed entry logged");
}

static void duplicates_keep_the_first_action() {
    std::vector<std::string> log;
    const ControlMap m = ControlMap::build("circle=X|a", kKey, kPad, &log);
    CHECK(m.key_mask(5) == 0x4000, "X stays cross (cross comes first)");
    CHECK(m.pad_mask(0) == 0x4000, "a stays cross");
    CHECK(logged(log, "already used by cross"), "duplicate logged");
    CHECK(m.key_mask(7) == 0, "Z no longer circle (circle was replaced by X, which was dropped)");
}

static void empty_unbinds() {
    std::vector<std::string> log;
    const ControlMap m = ControlMap::build("select=|", kKey, kPad, &log);
    CHECK(log.empty(), "empty is valid");
    CHECK(m.key_mask(13) == 0 && m.key_mask(14) == 0 && m.pad_mask(4) == 0, "select fully unbound");
}

static void reverse_lookup_defaults() {
    std::vector<std::string> log;
    const ControlMap m = ControlMap::build("", kKey, kPad, &log);
    CHECK(m.key_name(0x4000) == "X", "cross shows its first key X");
    CHECK(m.key_name(0x0008) == "Space", "start key");
    CHECK(m.key_name(0x0001) == "V", "select shows its first key V");
    CHECK(m.pad_name(0x4000) == "a", "cross pad button");
    CHECK(m.pad_name(0x0100) == "leftshoulder", "l pad button");
    CHECK(m.key_name(0x0004).empty() && m.pad_name(0x0004).empty(), "no action -> empty");
}

static void reverse_lookup_remap() {
    std::vector<std::string> log;
    const ControlMap m = ControlMap::build("cross=K|y;circle=|", kKey, kPad, &log);
    CHECK(m.key_name(0x4000) == "K" && m.pad_name(0x4000) == "y", "remapped cross");
    CHECK(m.key_name(0x2000).empty() && m.pad_name(0x2000).empty(), "unbound circle");
    std::vector<std::string> log2;
    const ControlMap d = ControlMap::build("circle=X,Z|a", kKey, kPad, &log2);
    CHECK(d.key_name(0x2000) == "Z", "dropped duplicate X is not circle's first key");
    CHECK(d.pad_name(0x2000).empty(), "dropped duplicate pad button is not returned");
}

static void stick_actions() {
    std::vector<std::string> log;
    CHECK(control_actions().size() == 16 && std::string(control_actions()[12].name) == "analog_up" &&
              control_actions()[15].mask == kAnalogRight,
          "four stick actions after select");
    CHECK(ControlMap::build("", kKey, kPad, &log).key_mask(19) == 0 && log.empty(), "unbound by default");
    const ControlMap m = ControlMap::build("analog_left=J|;analog_right=L|", kKey, kPad, &log);
    CHECK(m.key_mask(19) == kAnalogLeft && m.key_mask(20) == kAnalogRight && log.empty(), "bind keys");
    const ControlMap d = ControlMap::build("analog_up=X|", kKey, kPad, &log);
    CHECK(d.key_mask(5) == 0x4000 && logged(log, "analog_up: key X already used by cross"), "cross keeps X");
}

int main() {
    defaults_are_todays_mapping();
    stick_actions();
    spec_replaces_named_actions_only();
    invalid_entries_fall_back();
    duplicates_keep_the_first_action();
    empty_unbinds();
    reverse_lookup_defaults();
    reverse_lookup_remap();
    std::printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
