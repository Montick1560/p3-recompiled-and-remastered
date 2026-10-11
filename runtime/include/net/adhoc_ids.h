#pragma once
// Socket ids for ad hoc PDP and PTP sockets. PPSSPP keeps both kinds in one
// table (adhocSockets[255]), so an id is never used by a PDP and a PTP
// socket at the same time; games may rely on that.
#include <array>
#include <mutex>

namespace adhoc_ids {

constexpr int kMax = 255;

class Allocator {
public:
    int alloc() {  // 1..255, or -1 when full
        std::lock_guard<std::mutex> lock(mu_);
        for (int i = 0; i < kMax; i++) {
            if (!used_[i]) {
                used_[i] = true;
                return i + 1;
            }
        }
        return -1;
    }
    void release(int id) {
        std::lock_guard<std::mutex> lock(mu_);
        if (id >= 1 && id <= kMax) used_[id - 1] = false;
    }

private:
    std::mutex mu_;
    std::array<bool, kMax> used_{};
};

inline Allocator& shared() {
    static Allocator a;
    return a;
}

}  // namespace adhoc_ids
