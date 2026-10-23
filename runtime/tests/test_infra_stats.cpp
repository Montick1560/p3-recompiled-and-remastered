// infra_stats: the network event CSV of PSPRECOMP_INFRA_STATS (rows, payload
// hash, a bad path leaves it off) and the queue age a pop reports, against
// the in-process fake of tools/infra_server.py.
#include "net/infra_stats.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "fake_infra_server.h"
#include "net/infra_client.h"
#include "net/infra_protocol.h"

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

static std::string temp_csv(const char* name) {
    return (std::filesystem::temp_directory_path() / name).string();
}

static std::vector<std::vector<std::string>> read_rows(const std::string& path) {
    std::vector<std::vector<std::string>> rows;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        std::vector<std::string> cols;
        std::stringstream ss(line);
        std::string c;
        while (std::getline(ss, c, ',')) cols.push_back(c);
        rows.push_back(cols);
    }
    return rows;
}

static void stats_rows_and_hash() {
    CHECK(infra::stats::hash(reinterpret_cast<const uint8_t*>("a"), 1) == 0xE40C292C, "FNV-1a 32 of \"a\"");
    const std::string path = temp_csv("psprecomp_infra_stats_rows.csv");
    CHECK(infra::stats::open(path.c_str()), "open a temp file");
    CHECK(infra::stats::enabled(), "enabled once open");
    const uint8_t data[3] = {1, 2, 3};
    infra::stats::record(infra::stats::Ev::Rx, infra::kKindDatagram, 0x0A500002, 3658, 3659, data, 3);
    infra::stats::close();
    CHECK(!infra::stats::enabled(), "off after close");
    const auto rows = read_rows(path);
    CHECK(rows.size() == 2, "header and one row");
    if (rows.size() == 2) {
        CHECK(rows[0].size() == 9 && rows[0][0] == "t_us" && rows[0][8] == "extra_us", "header columns");
        const auto& r = rows[1];
        char h[16];
        std::snprintf(h, sizeof(h), "%08x", infra::stats::hash(data, 3));
        CHECK(r.size() == 9 && r[1] == "rx" && r[2] == "2" && r[4] == "3658" && r[5] == "3659" && r[6] == "3" &&
                  r[7] == h && r[8] == "0",
              "row: rx, kind, ports, len, hash, extra");
    }
    std::filesystem::remove(path);
}

static void stats_bad_path_is_off() {
    CHECK(!infra::stats::open("no_such_dir/x/y.csv"), "a bad path does not open");
    CHECK(!infra::stats::enabled(), "stays off");
    const uint8_t b = 1;
    infra::stats::record(infra::stats::Ev::Pop, infra::kKindDatagram, 1, 2, 3, &b, 1, 5);  // no crash
}

static void pop_reports_queue_age() {
    const std::string path = temp_csv("psprecomp_infra_stats_pop.csv");
    CHECK(infra::stats::open(path.c_str()), "open");
    FakeInfraServer s;
    s.start();
    infra::Client::Options o;
    o.ping_interval_ms = 100;
    infra::Client c(s.address(), o);
    CHECK(c.connect("P3A", 2000), "connect");
    const uint8_t d[2] = {4, 5};
    s.send_raw(infra::encode_datagram(0x0A500002, 3658, 3659, d, 2));
    infra::Client::Datagram g;
    const auto t = std::chrono::steady_clock::now();
    while (!c.has_datagram(3658) && std::chrono::steady_clock::now() - t < std::chrono::seconds(2)) fake_sleep_ms(1);
    fake_sleep_ms(30);
    CHECK(c.pop_datagram(3658, &g) && g.rx_us > 0, "popped, with its arrival time");
    c.disconnect();
    infra::stats::close();
    long long extra = -1;
    bool rx = false;
    for (const auto& r : read_rows(path)) {
        if (r.size() == 9 && r[1] == "pop") extra = std::stoll(r[8]);
        if (r.size() == 9 && r[1] == "rx") rx = true;
    }
    CHECK(rx, "an rx row");
    CHECK(extra >= 30000, "pop row: extra_us is the queue age (>= 30 ms)");
    std::filesystem::remove(path);
}

// A disk that stalls (the hook holds every file write for 500 ms) must not
// stall the network threads that record rows: record() only queues them.
static void stats_record_never_waits_for_disk() {
    const std::string path = temp_csv("psprecomp_infra_stats_slow.csv");
    CHECK(infra::stats::open(path.c_str()), "open");
    std::atomic<int> writes{0};
    infra::stats::set_write_hook_for_test([&] {
        writes++;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    });
    const uint8_t d[16] = {};
    const auto t = std::chrono::steady_clock::now();
    for (int i = 0; i < 5000; i++)
        infra::stats::record(infra::stats::Ev::TxCall, infra::kKindDatagram, 1, 2, 3, d, sizeof(d));
    const auto took = std::chrono::steady_clock::now() - t;
    CHECK(took < std::chrono::milliseconds(100), "5000 rows recorded while the disk is stalled");
    infra::stats::close();
    infra::stats::set_write_hook_for_test(nullptr);
    CHECK(writes > 0, "the file was written by the stats thread");
    CHECK(read_rows(path).size() == 5001, "header and every row on close");
    std::filesystem::remove(path);
}

int main() {
    stats_rows_and_hash();
    stats_bad_path_is_off();
    pop_reports_queue_age();
    stats_record_never_waits_for_disk();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("infra_stats: all tests passed\n");
    return 0;
}
