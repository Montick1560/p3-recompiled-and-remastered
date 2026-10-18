#pragma once
// <portable root>/achievements/unlocked.json (docs/contracts/achievements-v1.md): what this PC earned.
// Written only by the game, append-only, temp file + rename; the launcher only reads it.
#include <ctime>
#include <map>
#include <string>
#include <utility>
#include <vector>

struct PspEarned { std::string date, save, hero, release; };

enum class PspStoreState { Ok, Missing, Corrupt, Newer };

// The earned entries (entries that are not objects are skipped). Missing file = Missing; unreadable or
// invalid JSON = Corrupt; a valid file with another "version" = Newer (never written by this build).
std::map<std::string, PspEarned> psp_ach_store_read(const std::string& path, PspStoreState* state);

// Adds the ids the file does not have yet and rewrites it as <path>.tmp + rename. Every entry already
// in the file stays exactly as it is (also ids this build does not know, and keys it does not use).
// Creates the folder. A Corrupt file is first renamed to unlocked.corrupt.json (same folder) so nothing
// is lost; a Newer file is never touched (returns false). Returns false with *err on any failure.
bool psp_ach_store_add(const std::string& path, const std::vector<std::pair<std::string, PspEarned>>& add,
                       std::string* err);

// "2026-10-18T12:00:00Z"
std::string psp_ach_iso_utc(std::time_t t);

// UTF-16 (0-terminated or at most `max` units; surrogate pairs joined, lone ones become U+FFFD) to UTF-8.
std::string psp_utf16_to_utf8(const char16_t* s, size_t max);
