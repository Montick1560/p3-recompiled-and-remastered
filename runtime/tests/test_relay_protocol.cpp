// relay_protocol: aemu_postoffice wire format (init, PDP and PTP framing) and
// the PSP <-> wire port mapping.
#include "net/relay_protocol.h"

#include <cstdio>
#include <cstring>
#include <vector>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

static const pro::Mac kA = {0x02, 0, 0, 0, 0, 0x0a};
static const pro::Mac kB = {0x60, 0xbb, 0x59, 0xfd, 0x4d, 0x9a};

static void ports() {
    CHECK(relay::wire_port(3000, 10000) == 13000, "PSP port + offset");
    CHECK(relay::wire_port(0, 10000) == 0, "port 0 stays 0");
    CHECK(relay::wire_port(55536, 10000) == 65535, "a sum of 65536 becomes 65535");
    CHECK(relay::wire_port(60000, 10000) == 4464, "wraps like a u16 otherwise");
    CHECK(relay::psp_port(13000, 10000) == 3000, "wire port - offset");
}

static void init_packet() {
    const auto p = relay::encode_init(relay::INIT_PTP_CONNECT, kA, 13100, kB, 13101);
    static const uint8_t want[24] = {2, 0, 0, 0,  0x02, 0, 0, 0, 0, 0x0a, 0, 0,  0x2C, 0x33,
                                     0x60, 0xbb, 0x59, 0xfd, 0x4d, 0x9a, 0, 0,  0x2D, 0x33};
    CHECK(p.size() == 24 && std::memcmp(p.data(), want, 24) == 0, "init layout");
}

static void pdp_frames() {
    const auto h = relay::encode_pdp_header(kB, 13000, 3);
    static const uint8_t want[14] = {0x60, 0xbb, 0x59, 0xfd, 0x4d, 0x9a, 0, 0, 0xC8, 0x32, 3, 0, 0, 0};
    CHECK(h.size() == 14 && std::memcmp(h.data(), want, 14) == 0, "PDP header layout");

    relay::PdpFrames f;
    CHECK(f.next_size() < 0, "empty: no datagram");
    std::vector<uint8_t> wire(h.begin(), h.end());
    const uint8_t data[3] = {7, 8, 9};
    wire.insert(wire.end(), data, data + 3);
    const auto h2 = relay::encode_pdp_header(kA, 13001, 0);
    wire.insert(wire.end(), h2.begin(), h2.end());  // an empty datagram follows
    f.feed(wire.data(), 5);
    CHECK(f.next_size() < 0, "partial header: nothing yet");
    f.feed(wire.data() + 5, 12);
    CHECK(f.next_size() == 3, "complete frame: size known");
    uint8_t out[8];
    pro::Mac mac{};
    uint16_t port = 0;
    int len = 2;
    CHECK(f.pop(&mac, &port, out, &len) == relay::kTooSmall && len == 3, "buffer too small: size reported");
    CHECK(f.next_size() == 3, "datagram stays queued");
    len = 8;
    CHECK(f.pop(&mac, &port, out, &len) == 0 && len == 3 && mac == kB && port == 13000 &&
              out[0] == 7 && out[2] == 9, "datagram delivered");
    CHECK(f.next_size() < 0, "second frame not complete yet");
    f.feed(wire.data() + 17, static_cast<int>(wire.size()) - 17);
    len = 8;
    CHECK(f.pop(&mac, &port, out, &len) == 0 && len == 0 && mac == kA && port == 13001, "empty datagram");
}

static void ptp_frames() {
    const auto h = relay::encode_ptp_header(5);
    CHECK(h.size() == 4 && h[0] == 5 && h[1] == 0 && h[2] == 0 && h[3] == 0, "PTP header layout");
    relay::PtpFrames f;
    std::vector<uint8_t> wire(h.begin(), h.end());
    const uint8_t a[5] = {1, 2, 3, 4, 5};
    wire.insert(wire.end(), a, a + 5);
    const auto h2 = relay::encode_ptp_header(2);
    wire.insert(wire.end(), h2.begin(), h2.end());
    wire.push_back(6);
    wire.push_back(7);
    uint8_t out[8];
    CHECK(f.read(out, 8) == 0, "empty");
    f.feed(wire.data(), 6);
    CHECK(f.read(out, 8) == 0, "frame not complete: nothing");
    f.feed(wire.data() + 6, static_cast<int>(wire.size()) - 6);
    CHECK(f.read(out, 3) == 3 && out[0] == 1 && out[2] == 3, "part of the first frame");
    CHECK(f.read(out, 8) == 2 && out[0] == 4 && out[1] == 5, "rest of the first frame only");
    CHECK(f.read(out, 8) == 2 && out[0] == 6 && out[1] == 7, "second frame");
    CHECK(f.read(out, 8) == 0, "drained");
}

static void connect_packet() {
    const uint8_t raw[10] = {0x60, 0xbb, 0x59, 0xfd, 0x4d, 0x9a, 0xAA, 0xBB, 0x2C, 0x33};
    pro::Mac mac{};
    uint16_t port = 0;
    relay::parse_connect(raw, &mac, &port);
    CHECK(mac == kB && port == 13100, "connect request/ack: mac + port, padding ignored");
}

int main() {
    ports();
    init_packet();
    pdp_frames();
    ptp_frames();
    connect_packet();
    if (failures == 0) std::printf("relay_protocol tests passed\n");
    return failures == 0 ? 0 : 1;
}
