#include "net/direct_crypto.h"

#include "monocypher.h"

#include <cstring>

namespace direct {

namespace {

constexpr uint8_t kMagic[4] = {'P', '3', 'D', '1'};

void put_le(uint8_t* p, uint64_t v, int n) {
    for (int i = 0; i < n; i++) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

uint64_t get_le(const uint8_t* p, int n) {
    uint64_t v = 0;
    for (int i = n - 1; i >= 0; i--) v = v << 8 | p[i];
    return v;
}

bool known(Type t) { return t >= Type::Probe && t <= Type::Close; }

void nonce_of(uint64_t counter, uint8_t nonce[24]) {
    std::memset(nonce, 0, 24);
    put_le(nonce, counter, 8);
}

Key derive_one(const uint8_t pair_key[32], uint32_t from, uint32_t to) {
    uint8_t msg[16] = {'p', '3', 'd', '1', ' ', 'd', 'i', 'r'};
    for (int i = 0; i < 4; i++) {
        msg[8 + i] = static_cast<uint8_t>(from >> (24 - 8 * i));
        msg[12 + i] = static_cast<uint8_t>(to >> (24 - 8 * i));
    }
    Key k;
    crypto_blake2b_keyed(k.data(), k.size(), pair_key, 32, msg, sizeof msg);
    return k;
}

}  // namespace

void Keys::wipe() {
    crypto_wipe(tx.data(), tx.size());
    crypto_wipe(rx.data(), rx.size());
}

Keys derive_keys(const uint8_t pair_key[32], uint32_t my_vip, uint32_t peer_vip) {
    return Keys{derive_one(pair_key, my_vip, peer_vip), derive_one(pair_key, peer_vip, my_vip)};
}

std::vector<uint8_t> seal(const Key& key, uint32_t pair_id, uint64_t counter, Type type, const uint8_t* body,
                          size_t len) {
    const size_t size = kHeaderSize + 1 + len + kTagSize;
    if (counter == 0 || counter >= kCounterLimit || !known(type) || size > kMaxPacket) return {};
    std::vector<uint8_t> pkt(size);
    std::memcpy(pkt.data(), kMagic, 4);
    put_le(pkt.data() + 4, pair_id, 4);
    put_le(pkt.data() + 8, counter, 8);
    uint8_t* text = pkt.data() + kHeaderSize;
    text[0] = static_cast<uint8_t>(type);
    if (len) std::memcpy(text + 1, body, len);
    uint8_t nonce[24];
    nonce_of(counter, nonce);
    crypto_aead_lock(text, text + 1 + len, key.data(), nonce, pkt.data(), kHeaderSize, text, 1 + len);
    return pkt;
}

bool read_header(const uint8_t* pkt, size_t len, uint32_t* pair_id, uint64_t* counter) {
    if (len < kHeaderSize + 1 + kTagSize || len > kMaxPacket || std::memcmp(pkt, kMagic, 4) != 0) return false;
    *pair_id = static_cast<uint32_t>(get_le(pkt + 4, 4));
    *counter = get_le(pkt + 8, 8);
    return true;
}

bool open(const Key& key, const uint8_t* pkt, size_t len, Opened* out) {
    uint32_t pair_id;
    uint64_t counter;
    if (!read_header(pkt, len, &pair_id, &counter) || counter == 0 || counter >= kCounterLimit) return false;
    const size_t text_len = len - kHeaderSize - kTagSize;
    std::vector<uint8_t> text(text_len);
    uint8_t nonce[24];
    nonce_of(counter, nonce);
    if (crypto_aead_unlock(text.data(), pkt + len - kTagSize, key.data(), nonce, pkt, kHeaderSize,
                           pkt + kHeaderSize, text_len) != 0)
        return false;
    const Type type = static_cast<Type>(text[0]);
    if (!known(type)) return false;
    out->pair_id = pair_id;
    out->counter = counter;
    out->type = type;
    out->body.assign(text.begin() + 1, text.end());
    return true;
}

bool ReplayWindow::check(uint64_t counter) const {
    if (counter == 0) return false;
    if (counter > newest_) return true;
    return newest_ - counter < kSize && !test(counter);
}

void ReplayWindow::accept(uint64_t counter) {
    if (counter > newest_) {
        if (counter - newest_ >= kSize) {
            seen_.fill(0);
        } else {
            for (uint64_t c = newest_ + 1; c <= counter; c++) seen_[(c % kSize) / 64] &= ~(uint64_t{1} << (c % 64));
        }
        newest_ = counter;
    }
    seen_[(counter % kSize) / 64] |= uint64_t{1} << (counter % 64);
}

}  // namespace direct
