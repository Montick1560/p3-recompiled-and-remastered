#pragma once
// Network event log of infrastructure mode (PSPRECOMP_INFRA_STATS=<file>): one
// CSV row per game datagram / stream segment sent, received, read by the game
// or dropped, stamped with steady_clock microseconds. On Windows that clock is
// QueryPerformanceCounter, the same in every process of the PC, so the logs of
// two instances can be joined (tools/infra_lagjoin.py). Each row carries an
// FNV-1a hash of the payload to match a datagram across the two logs.
// Thread-safe; record() is a no-op while the log is off. record() only
// appends to memory: a stats thread writes the file, so a disk that stalls
// never stalls the network threads (over 16 MiB waiting, rows are dropped
// and counted).
#include <cstddef>
#include <cstdint>
#include <functional>

namespace infra::stats {

// CSV names: tx_call, tx_done, rx, pop, drop.
enum class Ev { TxCall, TxDone, Rx, Pop, Drop };

// Opens (truncates) the log. false, and one "[INFRA] stats: cannot open" line,
// when the file cannot be created; the log then stays off.
bool open(const char* path);
// The first call opens PSPRECOMP_INFRA_STATS when it is set.
bool enabled();
int64_t now_us();
uint32_t hash(const uint8_t* data, size_t len);
// Row `t_us,ev,kind,vip,src,dst,len,hash,extra_us`; vip is the destination for
// tx_* and the source for rx/pop/drop.
void record(Ev ev, uint8_t kind, uint32_t vip, uint16_t src, uint16_t dst, const uint8_t* data, size_t len,
            int64_t extra_us = 0);
// Stops the stats thread after it wrote every row.
void close();
// Tests: runs on the stats thread before each file write (e.g. to make the disk slow); empty to clear.
void set_write_hook_for_test(std::function<void()> hook);

}  // namespace infra::stats
