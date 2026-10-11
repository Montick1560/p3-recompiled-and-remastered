#pragma once
// aemu_postoffice relay wire format (PPSSPP's "server-provided packet relay",
// TCP 27313 on the ad hoc server host). Written from the protocol description
// in docs/superpowers/plans/2026-10-11-adhoc-relay.md; little endian, MAC
// fields are 8 bytes of which 6 are used.
#include <array>
#include <cstdint>
#include <vector>

#include "net/pro_protocol.h"

namespace relay {

enum InitType : int32_t { INIT_PDP = 0, INIT_PTP_LISTEN = 1, INIT_PTP_CONNECT = 2, INIT_PTP_ACCEPT = 3 };

constexpr uint16_t kDefaultPort = 27313;
constexpr int kInitSize = 24;
constexpr int kPdpHeaderSize = 14;
constexpr int kPtpHeaderSize = 4;
constexpr int kConnectSize = 10;
constexpr int kPdpMax = 10 * 1024;
constexpr int kPtpMax = 50 * 1024;
constexpr int kTooSmall = -1;

// PSP port <-> port on the wire (PPSSPP offset_port_simple / reverse_port_simple).
uint16_t wire_port(uint16_t psp_port, uint16_t offset);
uint16_t psp_port(uint16_t wire_port, uint16_t offset);

std::array<uint8_t, kInitSize> encode_init(InitType type, const pro::Mac& src, uint16_t sport,
                                           const pro::Mac& dst, uint16_t dport);
std::array<uint8_t, kPdpHeaderSize> encode_pdp_header(const pro::Mac& mac, uint16_t port, uint32_t size);
std::array<uint8_t, kPtpHeaderSize> encode_ptp_header(uint32_t size);
// A PTP connect request (listener side) or ack (both sides): mac + port.
void parse_connect(const uint8_t* raw, pro::Mac* mac, uint16_t* port);

// Incoming PDP frames from a relay stream.
class PdpFrames {
public:
    void feed(const uint8_t* data, int len);
    // Payload size of the first complete datagram, or -1.
    int next_size() const;
    // Pops the first complete datagram into buf. *len: in capacity, out size.
    // kTooSmall (datagram kept, *len = its size) when it does not fit; -1 when
    // none is complete.
    int pop(pro::Mac* from, uint16_t* port, uint8_t* buf, int* len);

private:
    std::vector<uint8_t> buf_;
};

// Incoming PTP frames from a relay stream: a read returns bytes of one frame.
class PtpFrames {
public:
    void feed(const uint8_t* data, int len);
    int read(uint8_t* out, int cap);  // bytes copied, 0 when no complete frame is pending
    bool pending() const;

private:
    std::vector<uint8_t> buf_;
    size_t frame_left_ = 0;  // payload bytes of the current (complete) frame not yet read
};

}  // namespace relay
