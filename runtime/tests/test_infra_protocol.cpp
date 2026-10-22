// infra_protocol: frame codec of the infrastructure-mode server
// (tools/infra_server.py): u32 length (LE, of kind + body) | u8 kind | body.
#include "net/infra_protocol.h"

#include <cstdio>
#include <cstring>
#include <vector>

static int failures = 0;
#define CHECK(cond, msg) \
    do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

static void json_roundtrip_split_feed() {
    const nlohmann::json j = {{"id", 7}, {"op", "login"}, {"online_id", "P3A"}};
    const auto wire = infra::encode_json(j);
    const std::string text = j.dump();
    CHECK(wire.size() == 5 + text.size(), "frame size = 4 + 1 + json");
    CHECK(wire[0] == static_cast<uint8_t>(text.size() + 1) && wire[1] == 0 && wire[4] == infra::kKindJson,
          "length counts kind + body, little endian");
    infra::FrameReader r;
    infra::Frame f;
    int got = 0;
    for (uint8_t b : wire) {
        r.feed(&b, 1);
        while (r.next(&f)) {
            got++;
            CHECK(f.kind == infra::kKindJson && f.json == j, "same JSON back");
        }
    }
    CHECK(got == 1, "one frame out of a byte-by-byte feed");
    CHECK(!r.next(&f), "nothing left");
}

static void datagram_roundtrip() {
    std::vector<uint8_t> payload(1396);
    for (size_t i = 0; i < payload.size(); i++) payload[i] = static_cast<uint8_t>(i * 7);
    const auto wire = infra::encode_datagram(0x0A500002, 3658, 3658, payload.data(), payload.size());
    CHECK(wire.size() == 5 + 8 + payload.size(), "datagram frame size");
    static const uint8_t head[13] = {0x7D, 0x05, 0, 0, 2, 0x02, 0, 0x50, 0x0A, 0x4A, 0x0E, 0x4A, 0x0E};
    CHECK(std::memcmp(wire.data(), head, 13) == 0, "datagram header layout (vip, dst port, src port LE)");
    infra::FrameReader r;
    r.feed(wire.data(), wire.size());
    infra::Frame f;
    CHECK(r.next(&f) && f.kind == infra::kKindDatagram, "datagram frame");
    CHECK(f.vip == 0x0A500002 && f.dst_port == 3658 && f.src_port == 3658, "datagram header back");
    CHECK(f.payload == payload, "datagram payload back");
}

static void two_frames_one_feed() {
    auto wire = infra::encode_json({{"ev", "member_joined"}});
    const uint8_t d[3] = {1, 2, 3};
    const auto w2 = infra::encode_datagram(0x0A500001, 1, 2, d, 3);
    wire.insert(wire.end(), w2.begin(), w2.end());
    infra::FrameReader r;
    r.feed(wire.data(), wire.size());
    infra::Frame f;
    CHECK(r.next(&f) && f.kind == infra::kKindJson && f.json["ev"] == "member_joined", "first frame");
    CHECK(r.next(&f) && f.kind == infra::kKindDatagram && f.payload.size() == 3 && f.src_port == 2, "second frame");
    CHECK(!r.next(&f), "then none");
}

static void bad_json_skipped() {
    const char bad[] = "{not json";
    std::vector<uint8_t> wire = {static_cast<uint8_t>(sizeof(bad)), 0, 0, 0, infra::kKindJson};
    wire.insert(wire.end(), bad, bad + sizeof(bad) - 1);
    const auto good = infra::encode_json({{"id", 1}, {"ok", true}});
    wire.insert(wire.end(), good.begin(), good.end());
    infra::FrameReader r;
    r.feed(wire.data(), wire.size());
    infra::Frame f;
    CHECK(r.next(&f) && f.json["id"] == 1, "the frame after a bad one decodes");
    CHECK(r.bad_frames() == 1, "bad frame counted");
    const auto arr = infra::encode_json(nlohmann::json::array({1, 2}));
    r.feed(arr.data(), arr.size());
    CHECK(!r.next(&f) && r.bad_frames() == 2, "a JSON frame that is not an object is skipped too");
}

static void oversized_frame_poisons() {
    const uint8_t huge[5] = {0x00, 0x00, 0x02, 0x00, infra::kKindJson};  // 128 KiB
    infra::FrameReader r;
    r.feed(huge, 5);
    infra::Frame f;
    CHECK(!r.next(&f) && r.broken(), "a frame over 64 KiB breaks the stream");
}

static void stream_roundtrip() {
    const uint8_t seg[4] = {3, 'a', 'b', 'c'};  // type DATA + bytes
    const auto wire = infra::encode_segment(infra::kKindStream, 0x0A500001, 50000, 3659, seg, 4);
    CHECK(wire.size() == 5 + 8 + 4 && wire[4] == infra::kKindStream, "stream segment = kind 3, datagram layout");
    infra::FrameReader r;
    r.feed(wire.data(), wire.size());
    infra::Frame f;
    CHECK(r.next(&f) && f.kind == infra::kKindStream && f.vip == 0x0A500001 && f.dst_port == 50000 &&
              f.src_port == 3659 && f.payload == std::vector<uint8_t>(seg, seg + 4), "stream segment back");
    CHECK(r.bad_frames() == 0, "kind 3 is not a bad frame");
}

static void hex_roundtrip() {
    const uint8_t b[3] = {0x00, 0xff, 0x10};
    CHECK(infra::to_hex(b, 3) == "00ff10", "to_hex");
    CHECK(infra::from_hex("00ff10") == std::vector<uint8_t>(b, b + 3), "from_hex");
    CHECK(infra::from_hex("00FF10") == std::vector<uint8_t>(b, b + 3), "upper case hex");
    CHECK(infra::from_hex("0g").empty() && infra::from_hex("abc").empty(), "bad hex -> empty");
}

int main() {
    json_roundtrip_split_feed();
    datagram_roundtrip();
    two_frames_one_feed();
    bad_json_skipped();
    oversized_frame_poisons();
    stream_roundtrip();
    hex_roundtrip();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("infra_protocol: all tests passed\n");
    return 0;
}
