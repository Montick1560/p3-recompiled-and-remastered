#pragma once
// Achievements (docs/contracts/achievements-v1.md; spec 2026-10-18 launcher part 5b §4-§5). The game
// module names facts in its progress struct (PspAchievementSource); the official catalogue says which
// value of which fact earns what. This core is pure: no threads, no I/O, no getenv. A check reads only
// the bytes its pending achievements name and never allocates.
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// One readable value in the progress struct, relative to its base.
struct PspFact {
    const char* name;   // e.g. "hero.level"
    uint32_t offset;
    uint8_t size;       // 1, 2 or 4 bytes, little endian
    bool is_signed;
    int8_t bit;         // -1 = the whole value; 0..31 = that bit of it (0 or 1)
    uint16_t count;     // 0 = one value; N = how many of N entries (`size` bytes each) are non-zero
    int64_t min, max;   // plausible range; a value outside it means the struct is not valid right now
};

// What the game module provides for one release.
struct PspAchievementSource {
    const char* release;                               // release.json id, e.g. "dxd-1.1.4"
    uint32_t (*base)(const uint8_t* rdram);            // struct address now (0 = none)
    bool (*sane)(const uint8_t* rdram, uint32_t base); // cheap plausibility check of the struct
    void (*hero)(const uint8_t* rdram, uint32_t base, char16_t out[17]);  // hero name, 0-terminated
    uint32_t size;                                     // struct size in bytes
    const PspFact* facts;
    size_t fact_count;
};

enum class PspAchOp : uint8_t { Ge, Eq, Ne };
struct PspAchLeaf { uint16_t fact; PspAchOp op; int64_t value; };

// One achievement not earned yet: its leaves are checks.leaves[first, first + n).
struct PspAchPending {
    uint16_t index;   // position in the catalogue
    bool any;         // true: any leaf holds; false: every leaf holds
    uint8_t streak;   // consecutive valid checks in which the condition held
    uint16_t first, n;
};

struct PspAchChecks {
    std::vector<PspAchLeaf> leaves;
    std::vector<PspAchPending> pending;
    std::vector<uint16_t> used;      // facts the pending checks read, once each
    std::vector<int64_t> values;     // scratch, one per source fact (sized at compile time)
};

struct PspAchEntry {
    std::string id, icon, name_es, name_en;
    bool hidden = false, has_when = false;
};

struct PspAchCatalogue {
    std::vector<PspAchEntry> entries;  // every catalogue entry, in file order (index = position)
    PspAchChecks checks;
    std::vector<std::string> log;      // "[ACH] ..." lines
};

// The value of one fact (count / bit / signed rules of PspFact).
int64_t psp_ach_read(const PspFact& f, const uint8_t* rdram, uint32_t base);

// Parses the catalogue and compiles every entry that has a `when` and is not in `earned` into checks
// over src.facts. An entry whose `when` names an unknown fact, has a bad op, a non-integer value, an
// empty or nested all/any is left out with one log line. At most 255 entries.
PspAchCatalogue psp_ach_compile(const std::string& json, const PspAchievementSource& src,
                                const std::vector<std::string>& earned);

// One check. Grants (writes catalogue indices into granted[0..max), removes them from checks.pending)
// the achievements whose condition held in this valid check and the previous one. An invalid struct
// (no base, outside user RAM, sane() false, a used fact outside its range) grants nothing and resets
// every streak. Returns how many were granted.
size_t psp_ach_evaluate(PspAchChecks& checks, const PspAchievementSource& src, const uint8_t* rdram,
                        uint16_t* granted, size_t max);

// Everything one check granted, on its way from the game thread to the worker (one push per check,
// so the worker never splits a batch: more than 3 = one summary toast).
constexpr size_t kAchBatchMax = 64;
struct PspAchBatch { uint16_t n; uint16_t index[kAchBatchMax]; char16_t hero[17]; };

// Single-producer single-consumer ring (game thread pushes, worker pops). Lock-free.
template <size_t N>
class PspAchRing {
public:
    bool push(const PspAchBatch& g) {
        const size_t h = head_.load(std::memory_order_relaxed);
        if (h - tail_.load(std::memory_order_acquire) == N) return false;
        buf_[h % N] = g;
        head_.store(h + 1, std::memory_order_release);
        return true;
    }
    bool pop(PspAchBatch* out) {
        const size_t t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire)) return false;
        *out = buf_[t % N];
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }
private:
    std::array<PspAchBatch, N> buf_{};
    std::atomic<size_t> head_{0}, tail_{0};
};
