#include "net/direct_link.h"

#include "psp_socket_compat.h"  // before windows.h (winsock2 first)

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>

#include "monocypher.h"
#include "net/secure_random.h"
#include "net/stun.h"

#ifndef _WIN32
#include <netdb.h>
#include <sys/select.h>
#endif

namespace direct {

namespace {

constexpr int kSelectMs = 20;
constexpr size_t kMaxCands = 4;
constexpr size_t kMaxStatsPairs = 8;  // tools/infra_server.py MAX_DIRECT_PAIRS

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

sockaddr_in to_sockaddr(const Addr& a) {
    sockaddr_in s{};
    s.sin_family = AF_INET;
    s.sin_addr.s_addr = htonl(a.ip);
    s.sin_port = htons(a.port);
    return s;
}

// IPv4 addresses of a name ("host:port" for STUN servers, or this PC's own name); blocking DNS.
std::vector<uint32_t> resolve(const std::string& host) {
    std::vector<uint32_t> ips;
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0) return ips;
    for (addrinfo* r = res; r; r = r->ai_next)
        ips.push_back(ntohl(reinterpret_cast<const sockaddr_in*>(r->ai_addr)->sin_addr.s_addr));
    freeaddrinfo(res);
    return ips;
}

// This PC's LAN addresses: host candidates, which the server forwards only to a peer behind the same NAT.
std::vector<uint32_t> lan_ips() {
    char name[256] = {};
    if (gethostname(name, sizeof name - 1) != 0) return {};
    std::vector<uint32_t> out;
    for (uint32_t ip : resolve(name))
        if ((ip >> 24) != 127 && (ip >> 16) != 0xA9FE) out.push_back(ip);  // not loopback, not link-local
    return out;
}

const char* type_name(CandType t) { return t == CandType::Host ? "host" : "srflx"; }

}  // namespace

bool parse_impair(const std::string& text, Impair* out) {
    Impair v;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find(',', at);
        if (end == std::string::npos) end = text.size();
        const std::string kv = text.substr(at, end - at);
        const size_t eq = kv.find('=');
        if (eq == std::string::npos || eq + 1 >= kv.size()) return false;
        const std::string k = kv.substr(0, eq), num = kv.substr(eq + 1);
        if (num.size() > 5 || num.find_first_not_of("0123456789") != std::string::npos) return false;
        const int n = std::stoi(num);
        if (k == "delay") v.delay = n;
        else if (k == "jitter") v.jitter = n;
        else if (k == "loss" && n <= 100) v.loss = n;
        else if (k == "reorder" && n <= 100) v.reorder = n;
        else return false;
        at = end + 1;
    }
    *out = v;
    return true;
}

bool Link::start(const LinkOptions& opt, DeliverFn deliver, RelayFn relay) {
    stop();
    uint64_t probe;
    if (!net::secure_random(reinterpret_cast<uint8_t*>(&probe), sizeof probe)) return false;
    psp_sock_startup();
    const int fd = static_cast<int>(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if (fd < 0) return false;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    socklen_t len = sizeof a;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 ||
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len) != 0) {
        psp_sock_close(fd);
        return false;
    }
#ifdef _WIN32
    BOOL off = FALSE;  // an ICMP "port unreachable" must not turn into a recvfrom error
    DWORD ret = 0;
    WSAIoctl(static_cast<SOCKET>(fd), _WSAIOW(IOC_VENDOR, 12), &off, sizeof off, nullptr, 0, &ret, nullptr, nullptr);
#endif
    std::lock_guard<std::mutex> l(mu_);
    fd_ = fd;
    port_ = ntohs(a.sin_port);
    opt_ = opt;
    deliver_ = std::move(deliver);
    relay_ = std::move(relay);
    rng_ = [] {
        uint64_t v = 0;
        net::secure_random(reinterpret_cast<uint8_t*>(&v), sizeof v);
        return v;
    };
    impair_ = Impair{};
    if (!opt.impair.empty()) parse_impair(opt.impair, &impair_);
    peers_.clear();
    stun_asks_.clear();
    srflx_.clear();
    finished_.clear();
    running_ = true;
    thread_ = std::thread([this] { run(); });
    return true;
}

void Link::stop() {
    {
        std::lock_guard<std::mutex> l(mu_);
        if (!running_) return;
        running_ = false;
        const int64_t now = now_ms();
        for (auto& [vip, p] : peers_) {
            std::vector<Out> outs;
            p.path->close(now, &outs);
            for (const Out& o : outs) sendto_now(o);
        }
        peers_.clear();  // ~Path wipes the keys
        held_.clear();
    }
    stun_cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    std::lock_guard<std::mutex> l(mu_);
    if (fd_ >= 0) psp_sock_close(fd_);
    fd_ = -1;
}

std::vector<Cand> Link::local_candidates(int wait_ms) {
    if (!running_) return {};
    std::vector<std::pair<Addr, std::vector<uint8_t>>> asks;
    for (const std::string& s : opt_.stun) {
        const size_t colon = s.rfind(':');
        if (colon == std::string::npos) continue;
        const int port = std::atoi(s.c_str() + colon + 1);
        const std::vector<uint32_t> ips = resolve(s.substr(0, colon));
        if (ips.empty() || port <= 0 || port > 65535) continue;
        StunAsk ask{{ips[0], static_cast<uint16_t>(port)}, {}};
        if (!net::secure_random(ask.txid, sizeof ask.txid)) return {};
        {
            std::lock_guard<std::mutex> l(mu_);
            stun_asks_.push_back(ask);
        }
        asks.push_back({ask.server, stun::binding_request(ask.txid)});
    }
    std::unique_lock<std::mutex> l(mu_);
    for (const auto& [server, bytes] : asks) sendto_now({server, bytes});
    stun_cv_.wait_for(l, std::chrono::milliseconds(wait_ms), [this] {
        return !running_ || std::all_of(stun_asks_.begin(), stun_asks_.end(), [](const StunAsk& a) { return a.answered; });
    });
    stun_asks_.clear();  // a late answer is ignored
    std::vector<Cand> out;
    for (const Addr& a : srflx_) out.push_back({a, CandType::Srflx});
    l.unlock();
    std::vector<uint32_t> hosts = opt_.test_host_ips;
    for (uint32_t ip : lan_ips())
        if (std::find(hosts.begin(), hosts.end(), ip) == hosts.end()) hosts.push_back(ip);
    for (uint32_t ip : hosts) out.push_back({{ip, port_}, CandType::Host});
    if (out.size() > kMaxCands) out.resize(kMaxCands);
    return out;
}

void Link::add_peer(uint32_t my_vip, uint32_t peer_vip, uint32_t pair_id, const uint8_t key[32],
                    std::vector<Cand> cands) {
    Keys keys = derive_keys(key, my_vip, peer_vip);
    std::lock_guard<std::mutex> l(mu_);
    if (running_) {
        if (peers_.count(peer_vip)) finished_.push_back(entry(peers_[peer_vip].path->stats()));
        peers_[peer_vip] = Peer{std::make_unique<Path>(pair_id, keys, std::move(cands), now_ms(), rng_)};
    }
    keys.wipe();
}

void Link::drop_peer(uint32_t peer_vip) {
    std::lock_guard<std::mutex> l(mu_);
    auto it = peers_.find(peer_vip);
    if (it == peers_.end()) return;
    std::vector<Out> outs;
    it->second.path->close(now_ms(), &outs);
    for (const Out& o : outs) sendto_now(o);
    finished_.push_back(entry(it->second.path->stats()));
    if (it->second.logged == PathState::Up)
        std::fprintf(stderr, "[INFRA] direct: vip %08x back to relay (dropped)\n", static_cast<unsigned>(peer_vip));
    peers_.erase(it);
}

bool Link::has_peer(uint32_t peer_vip) const {
    std::lock_guard<std::mutex> l(mu_);
    return peers_.count(peer_vip) != 0;
}

bool Link::is_up(uint32_t peer_vip) const {
    std::lock_guard<std::mutex> l(mu_);
    auto it = peers_.find(peer_vip);
    return it != peers_.end() && it->second.path->state() == PathState::Up;
}

bool Link::send_dgram(uint32_t peer_vip, uint16_t dst, uint16_t src, const uint8_t* data, size_t len) {
    std::lock_guard<std::mutex> l(mu_);
    auto it = peers_.find(peer_vip);
    if (!running_ || it == peers_.end()) return false;
    std::vector<uint8_t> body(4 + len);
    body[0] = static_cast<uint8_t>(dst), body[1] = static_cast<uint8_t>(dst >> 8);
    body[2] = static_cast<uint8_t>(src), body[3] = static_cast<uint8_t>(src >> 8);
    if (len) std::memcpy(body.data() + 4, data, len);
    std::vector<Out> outs;
    const int64_t now = now_ms();
    if (!it->second.path->send(Type::Dgram, body.data(), body.size(), now, &outs)) return false;
    transmit(outs, now);
    return true;
}

void Link::on_relayed(uint32_t peer_vip, const uint8_t* bytes, size_t len) {
    std::vector<Delivery> got;
    {
        std::lock_guard<std::mutex> l(mu_);
        auto it = peers_.find(peer_vip);
        Opened o;
        if (it != peers_.end() && it->second.path->on_relayed(bytes, len, now_ms(), &o))
            got.push_back({peer_vip, std::move(o)});
    }
    deliver_all(got);
}

void Link::set_impair(const std::string& text) {
    Impair v;
    if (!parse_impair(text, &v)) return;
    std::lock_guard<std::mutex> l(mu_);
    impair_ = v;
}

nlohmann::json Link::entry(const PathStats& s) const {
    const uint64_t lost = s.keepalives > s.keepalive_acks ? s.keepalives - s.keepalive_acks : 0;
    return {{"path", s.up_ms > 0 ? "direct" : "relay"},
            {"rtt_p50", std::max(0, s.rtt_p50)},
            {"rtt_p95", std::max(0, s.rtt_p95)},
            {"loss_x10", s.keepalives ? static_cast<int>(lost * 1000 / s.keepalives) : 0},
            {"direct_s", static_cast<int>(std::min<int64_t>(s.up_ms / 1000, 600000))},
            {"fallbacks", s.fallbacks}};
}

nlohmann::json Link::stats_json() const {
    std::lock_guard<std::mutex> l(mu_);
    nlohmann::json a = nlohmann::json::array();
    for (const auto& e : finished_) a.push_back(e);
    for (const auto& [vip, p] : peers_) a.push_back(entry(p.path->stats()));
    while (a.size() > kMaxStatsPairs) a.erase(a.begin());  // the newest pairs
    return a;
}

void Link::sendto_now(const Out& o) {
    const sockaddr_in to = to_sockaddr(o.to);
    ::sendto(fd_, reinterpret_cast<const char*>(o.bytes.data()), static_cast<int>(o.bytes.size()), 0,
             reinterpret_cast<const sockaddr*>(&to), sizeof to);
}

void Link::transmit(std::vector<Out>& outs, int64_t now) {
    static thread_local std::mt19937 rng{std::random_device{}()};
    for (Out& o : outs) {
        if (impair_.loss && static_cast<int>(rng() % 100) < impair_.loss) continue;
        int64_t hold = impair_.delay;
        if (impair_.jitter) hold += static_cast<int>(rng() % (2 * impair_.jitter + 1)) - impair_.jitter;
        if (impair_.reorder && static_cast<int>(rng() % 100) < impair_.reorder) hold += 1 + impair_.jitter + 20;
        if (hold <= 0) {
            sendto_now(o);
            continue;
        }
        Held h{now + hold, std::move(o)};
        held_.insert(std::upper_bound(held_.begin(), held_.end(), h.due,
                                      [](int64_t due, const Held& x) { return due < x.due; }),
                     std::move(h));
    }
    outs.clear();
}

void Link::log_state(uint32_t vip, Peer& p) {
    const PathState s = p.path->state();
    if (s == p.logged) return;
    if (s == PathState::Up) {
        std::fprintf(stderr, "[INFRA] direct: vip %08x up (%s, rtt %d ms)\n", static_cast<unsigned>(vip),
                     type_name(p.path->path_type()), p.path->stats().rtt_p50);
    } else if (s == PathState::Down || s == PathState::Closed) {
        std::fprintf(stderr, "[INFRA] direct: vip %08x back to relay (%s)\n", static_cast<unsigned>(vip),
                     s == PathState::Closed ? "closed" : p.logged == PathState::Up ? "no answer" : "unreachable");
    }
    p.logged = s;
}

void Link::receive(int64_t now, std::vector<Delivery>* got) {
    static thread_local std::mt19937 rng{std::random_device{}()};
    uint8_t buf[2048];
    for (int n = 0; n < 256; n++) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(static_cast<unsigned>(fd_), &rd);
        timeval tv{0, 0};
        if (select(fd_ + 1, &rd, nullptr, nullptr, &tv) <= 0) return;
        sockaddr_in from{};
        socklen_t fl = sizeof from;
        const int len = ::recvfrom(fd_, reinterpret_cast<char*>(buf), sizeof buf, 0,
                                   reinterpret_cast<sockaddr*>(&from), &fl);
        if (len <= 0) continue;
        if (impair_.loss && static_cast<int>(rng() % 100) < impair_.loss) continue;
        const Addr src{ntohl(from.sin_addr.s_addr), ntohs(from.sin_port)};
        const size_t size = static_cast<size_t>(len);
        if (stun::looks_like_stun(buf, size)) {
            for (StunAsk& a : stun_asks_) {
                uint32_t ip;
                uint16_t port;
                if (a.answered || !(a.server == src) || !stun::parse_binding_success(buf, size, a.txid, &ip, &port))
                    continue;
                a.answered = true;
                if (std::find(srflx_.begin(), srflx_.end(), Addr{ip, port}) == srflx_.end()) srflx_.push_back({ip, port});
                stun_cv_.notify_all();
            }
            continue;
        }
        uint32_t pair_id;
        uint64_t counter;
        auto it = peers_.end();
        if (read_header(buf, size, &pair_id, &counter))
            it = std::find_if(peers_.begin(), peers_.end(), [&](auto& kv) { return kv.second.path->pair_id() == pair_id; });
        if (it == peers_.end()) {
            dropped_unknown_++;
            continue;
        }
        std::vector<Out> outs;
        Opened o;
        if (it->second.path->on_packet(src, buf, size, now, &outs, &o)) got->push_back({it->first, std::move(o)});
        transmit(outs, now);
    }
}

void Link::deliver_all(std::vector<Delivery>& got) {
    for (Delivery& d : got) {
        if ((d.o.type != Type::Dgram && d.o.type != Type::Seg) || d.o.body.size() < 4 || !deliver_) continue;
        const uint8_t* b = d.o.body.data();
        const uint16_t dst = static_cast<uint16_t>(b[0] | b[1] << 8), src = static_cast<uint16_t>(b[2] | b[3] << 8);
        deliver_(d.vip, d.o.type, dst, src, std::vector<uint8_t>(d.o.body.begin() + 4, d.o.body.end()));
    }
    got.clear();
}

void Link::run() {
    std::vector<Delivery> got;
    while (running_) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(static_cast<unsigned>(fd_), &rd);
        timeval tv{0, kSelectMs * 1000};
        select(fd_ + 1, &rd, nullptr, nullptr, &tv);
        {
            std::lock_guard<std::mutex> l(mu_);
            if (!running_) break;
            const int64_t now = now_ms();
            receive(now, &got);
            for (auto& [vip, p] : peers_) {
                std::vector<Out> outs;
                p.path->tick(now, &outs);
                transmit(outs, now);
                log_state(vip, p);
            }
            while (!held_.empty() && held_.front().due <= now) {
                sendto_now(held_.front().out);
                held_.pop_front();
            }
        }
        deliver_all(got);  // outside mu_: the client takes its own lock
    }
}

}  // namespace direct
