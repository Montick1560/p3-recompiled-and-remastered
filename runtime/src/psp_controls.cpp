#include "psp_controls.h"

#include <algorithm>
#include <cctype>

namespace {

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) a++;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) b--;
    return s.substr(a, b - a);
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t a = 0;
    while (a <= s.size()) {
        size_t c = s.find(sep, a);
        if (c == std::string::npos) c = s.size();
        out.push_back(trim(s.substr(a, c - a)));
        a = c + 1;
    }
    return out;
}

std::vector<std::string> names(const std::string& keys) {
    std::vector<std::string> out;
    for (const std::string& n : split(keys, ',')) if (!n.empty()) out.push_back(n);
    return out;
}

bool reserved_key(const std::string& n) {
    const std::string l = lower(n);
    return l == "f1" || l == "f11";
}

}  // namespace

const std::vector<ControlAction>& control_actions() {
    static const std::vector<ControlAction> a = {
        {"up", 0x0010, "Up", "dpup"},          {"down", 0x0040, "Down", "dpdown"},
        {"left", 0x0080, "Left", "dpleft"},    {"right", 0x0020, "Right", "dpright"},
        {"cross", 0x4000, "X, Return", "a"},   {"circle", 0x2000, "Z", "b"},
        {"square", 0x8000, "A", "x"},          {"triangle", 0x1000, "S", "y"},
        {"l", 0x0100, "Q", "leftshoulder"},    {"r", 0x0200, "W", "rightshoulder"},
        {"start", 0x0008, "Space", "start"},   {"select", 0x0001, "V, Tab", "back"},
    };
    return a;
}

ControlMap ControlMap::build(const std::string& spec, const KeyLookup& key, const PadLookup& pad,
                             std::vector<std::string>* log) {
    std::map<std::string, std::pair<std::string, std::string>> given;
    for (const std::string& entry : split(spec, ';')) {
        if (entry.empty()) continue;
        const size_t eq = entry.find('=');
        const size_t bar = entry.find('|');
        if (eq == std::string::npos || bar == std::string::npos || bar < eq) {
            log->push_back("[CONTROLS] malformed entry \"" + entry + "\" ignored");
            continue;
        }
        const std::string name = lower(trim(entry.substr(0, eq)));
        const bool known = std::any_of(control_actions().begin(), control_actions().end(),
                                       [&](const ControlAction& a) { return name == a.name; });
        if (!known) {
            log->push_back("[CONTROLS] unknown action \"" + name + "\" ignored");
            continue;
        }
        given[name] = {entry.substr(eq + 1, bar - eq - 1), trim(entry.substr(bar + 1))};
    }

    ControlMap m;
    std::map<int32_t, std::string> key_owner;
    std::map<int, std::string> pad_owner;
    for (const ControlAction& a : control_actions()) {
        std::string keys = a.keys, padname = a.pad;
        if (auto it = given.find(a.name); it != given.end()) {
            keys = it->second.first;
            padname = it->second.second;
        }
        // Keys: up to two, all known and not reserved, else this action's defaults.
        std::vector<std::string> ks = names(keys);
        std::string problem;
        if (ks.size() > 2) problem = "more than two keys";
        for (const std::string& n : ks) {
            if (!problem.empty()) break;
            if (reserved_key(n)) problem = "key " + n + " is reserved";
            else if (key(n) == 0) problem = "unknown key " + n;
        }
        if (!problem.empty()) {
            log->push_back(std::string("[CONTROLS] ") + a.name + ": " + problem + ", using \"" + a.keys + "\"");
            ks = names(a.keys);
        }
        std::vector<std::string> kept;
        for (const std::string& n : ks) {
            const int32_t code = key(n);
            if (code == 0) continue;
            if (auto o = key_owner.find(code); o != key_owner.end()) {
                log->push_back(std::string("[CONTROLS] ") + a.name + ": key " + n + " already used by " +
                               o->second + ", dropped");
                continue;
            }
            key_owner[code] = a.name;
            m.keys_[code] = a.mask;
            kept.push_back(n);
        }
        // Pad: one button (or none), known and not guide, else the default.
        if (!padname.empty() && (pad(padname) < 0 || lower(padname) == "guide")) {
            log->push_back(std::string("[CONTROLS] ") + a.name + ": " +
                           (lower(padname) == "guide" ? "pad button guide is reserved" : "unknown pad button " + padname) +
                           ", using \"" + a.pad + "\"");
            padname = a.pad;
        }
        if (!padname.empty()) {
            const int b = pad(padname);
            if (auto o = pad_owner.find(b); o != pad_owner.end()) {
                log->push_back(std::string("[CONTROLS] ") + a.name + ": pad button " + padname +
                               " already used by " + o->second + ", dropped");
                padname.clear();
            } else if (b >= 0) {
                pad_owner[b] = a.name;
                m.pads_[b] = a.mask;
            }
        }
        std::string joined;
        for (const std::string& n : kept) joined += (joined.empty() ? "" : ",") + n;
        m.desc_.push_back(std::string(a.name) + "=" + joined + "|" + padname);
    }
    return m;
}

uint32_t ControlMap::key_mask(int32_t keycode) const {
    auto it = keys_.find(keycode);
    return it == keys_.end() ? 0 : it->second;
}

uint32_t ControlMap::pad_mask(int button) const {
    auto it = pads_.find(button);
    return it == pads_.end() ? 0 : it->second;
}

std::string ControlMap::describe() const {
    std::string s;
    for (const std::string& d : desc_) s += (s.empty() ? "" : ";") + d;
    return s;
}
