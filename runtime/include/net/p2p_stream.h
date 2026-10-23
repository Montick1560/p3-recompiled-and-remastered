#pragma once
// The game's TCP P2P sockets (sceNetInetSocket type 10) carried as segments
// through the infrastructure server (kind 3 frames, net/infra_protocol.h).
// The server relays a member's frames in order and never drops them, so a
// stream needs no acknowledgements or retransmission: SYN / ACCEPT open it,
// DATA carries bytes, FIN ends one direction, RST refuses or resets.
// A listener answers a SYN at once (the connection waits in its backlog until
// the game accepts it), like a kernel TCP stack. Results are byte counts or
// -errno (net/inet_errno.h). Pure and single-threaded: the caller locks.
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <vector>

namespace p2p {

enum SegmentType : uint8_t { kSyn = 1, kAccept = 2, kData = 3, kFin = 4, kRst = 5 };
constexpr size_t kMaxSegmentData = 1024;
constexpr uint16_t kFirstLocalPort = 49152;  // connecting sockets get local ports from here
// A peer cannot make the game hold more than this: a connection with more unread bytes is reset, and a
// listener answers at most kMaxBacklog connections the game has not accepted yet.
constexpr size_t kMaxUnreadBytes = 4 * 1024 * 1024;
constexpr size_t kMaxBacklog = 16;

// On receive vip is the sender; bytes[0] is the SegmentType.
struct Segment {
    uint32_t vip = 0;
    uint16_t dst_port = 0, src_port = 0;
    std::vector<uint8_t> bytes;
};
using SendFn = std::function<void(uint32_t vip, uint16_t dst_port, uint16_t src_port, const std::vector<uint8_t>& bytes)>;

class Streams {
public:
    explicit Streams(SendFn send) : send_(std::move(send)) {}

    int listen(int id, uint16_t port);
    // -EINPROGRESS on the first call, -EALREADY while waiting, 0 once connected, -ECONNREFUSED.
    int connect(int id, uint32_t vip, uint16_t port);
    // Gives the oldest connection in the listener's backlog the socket id new_id; -EAGAIN if none.
    int accept(int listener, int new_id, uint32_t* vip, uint16_t* port);
    int send(int id, const uint8_t* data, size_t len);
    // Bytes copied; 0 at the end of the stream; -EAGAIN when nothing has arrived yet.
    int recv(int id, uint8_t* out, size_t cap);
    void close(int id);
    uint16_t local_port(int id) const;
    // No received byte waits to be read (accepted or not).
    bool idle() const;
    void on_segment(const Segment& s);

private:
    struct Conn {
        uint16_t local = 0;
        uint32_t peer_vip = 0;
        uint16_t peer_port = 0;
        bool connected = false, refused = false, reset = false, eof = false;
        std::deque<uint8_t> rx;
    };
    Conn* find(uint16_t local, uint32_t vip, uint16_t port);
    bool port_used(uint16_t port) const;
    uint16_t alloc_port();
    void emit(uint32_t vip, uint16_t dst, uint16_t src, uint8_t type, const uint8_t* data = nullptr, size_t len = 0);

    SendFn send_;
    std::map<int, Conn> conns_;                    // by socket id
    std::map<int, uint16_t> listeners_;            // socket id -> port
    std::map<uint16_t, std::deque<Conn>> backlog_;  // listener port -> answered, not yet accepted
    uint16_t next_port_ = kFirstLocalPort;
};

}  // namespace p2p
