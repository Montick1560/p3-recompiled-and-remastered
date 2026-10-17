// See psp_save_crypt.h. DecryptData / EncryptData / GetSDKMainVersion are PPSSPP's
// (Core/Dialog/SavedataParam.cpp), unchanged apart from being free functions.
#include "hle/psp_save_crypt.h"

#include <cstring>
#include <mutex>

#include "Common/CommonTypes.h"
#include "Common/Swap.h"
#include "Core/HLE/sceChnnlsv.h"

namespace psp_save_crypt {
namespace {

void init_kirk_once() {
    static std::once_flag once;
    std::call_once(once, [] { kirk_init(__ChnnlsvKirkState()); });
}

int align16(int n) { return (n + 15) & ~15; }

int GetSDKMainVersion(int sdkVersion) {
    if (sdkVersion > 0x0307FFFF) return 6;
    if (sdkVersion > 0x0300FFFF) return 5;
    if (sdkVersion > 0x0206FFFF) return 4;
    if (sdkVersion > 0x0205FFFF) return 3;
    if (sdkVersion >= 0x02000000) return 2;
    if (sdkVersion >= 0x01000000) return 1;
    return 0;
}

// Note: The work is done in-place, hence the memmove etc.
int EncryptData(unsigned int mode, unsigned char *data, int *dataLen, int *alignedLen, unsigned char *hash, const u8 *cryptkey) {
	pspChnnlsvContext1 ctx1{};
	pspChnnlsvContext2 ctx2{};

	/* Make room for the IV in front of the data. */
	memmove(data + 0x10, data, *alignedLen);

	/* Set up buffers */
	memset(hash, 0, 0x10);

	// Zero out the IV before we begin.
	memset(data, 0, 0x10);

	/* Build the 0x10-byte IV and setup encryption */
	if (sceSdCipherInit(ctx2, mode, 1, data, cryptkey) < 0)
		return -1;
	if (sceSdMacInit(ctx1, mode) < 0)
		return -2;
	if (sceSdMacUpdate(ctx1, data, 0x10) < 0)
		return -3;
	if (sceSdCipherUpdate(ctx2, data + 0x10, *alignedLen) < 0)
		return -4;

	/* Clear any extra bytes left from the previous steps */
	memset(data + 0x10 + *dataLen, 0, *alignedLen - *dataLen);

	/* Encrypt the data */
	if (sceSdMacUpdate(ctx1, data + 0x10, *alignedLen) < 0)
		return -5;

	/* Verify encryption */
	if (sceSdCipherFinal(ctx2) < 0)
		return -6;

	/* Build the file hash from this PSP */
	if (sceSdMacFinal(ctx1, hash, cryptkey) < 0)
		return -7;

	/* Adjust sizes to account for IV */
	*alignedLen += 0x10;
	*dataLen += 0x10;

	/* All done */
	return 0;
}

// Note: The work is done in-place, hence the memmove etc.
int DecryptData(unsigned int mode, unsigned char *data, int *dataLen, int *alignedLen, const u8 *cryptkey, const u8 *expectedHash) {
	pspChnnlsvContext1 ctx1{};
	pspChnnlsvContext2 ctx2{};

	/* Need a 16-byte IV plus some data */
	if (*alignedLen <= 0x10)
		return -1;
	*dataLen -= 0x10;
	*alignedLen -= 0x10;

	/* Perform the magic */
	if (sceSdMacInit(ctx1, mode) < 0)
		return -2;
	if (sceSdCipherInit(ctx2, mode, 2, data, cryptkey) < 0)
		return -3;
	if (sceSdMacUpdate(ctx1, data, 0x10) < 0)
		return -4;
	if (sceSdMacUpdate(ctx1, data + 0x10, *alignedLen) < 0)
		return -5;
	if (sceSdCipherUpdate(ctx2, data + 0x10, *alignedLen) < 0)
		return -6;

	/* Verify that it decrypted correctly */
	if (sceSdCipherFinal(ctx2) < 0)
		return -7;

	if (expectedHash) {
		u8 hash[16];
		if (sceSdMacFinal(ctx1, hash, cryptkey) < 0)
			return -7;
		if (memcmp(hash, expectedHash, sizeof(hash)) != 0)
			return -8;
	}

	/* The decrypted data starts at data + 0x10, so shift it back. */
	memmove(data, data + 0x10, *dataLen);
	return 0;
}

}  // namespace

int mode_from_params(const uint8_t* params, size_t size) {
    if (params == nullptr || size == 0) return 0;
    switch (params[0]) {
        case 0x00: return 0;
        case 0x01: return 1;
        case 0x21: return 3;
        case 0x41: return 5;
        default: return 1;  // PPSSPP: "Well, it's not zero, so yes."
    }
}

int request_mode(bool full_request, uint32_t secure_version, uint32_t sdk_version) {
    if (!full_request) return 1;
    const bool new_hash = GetSDKMainVersion(static_cast<int>(sdk_version)) >= 4;
    switch (secure_version) {
        case 0: return new_hash ? 5 : 3;
        case 2: return 3;
        case 3: return new_hash ? 5 : 1;
        default: return 1;
    }
}

int decrypt(int mode, const std::vector<uint8_t>& file, const uint8_t* key,
            const uint8_t* expected_hash, std::vector<uint8_t>* out) {
    init_kirk_once();
    if (file.size() > 0x7FFFFFF0u) return -1;
    int len = static_cast<int>(file.size());
    int aligned = align16(len);
    std::vector<uint8_t> buf(static_cast<size_t>(aligned), 0);
    if (len > 0) std::memcpy(buf.data(), file.data(), file.size());
    uint8_t k[16];
    if (key != nullptr) std::memcpy(k, key, 16);
    int err = DecryptData(static_cast<unsigned>(mode), buf.data(), &len, &aligned,
                          key != nullptr ? k : nullptr, expected_hash);
    if (err != 0) return err;
    out->assign(buf.begin(), buf.begin() + len);
    return 0;
}

int encrypt(int mode, const std::vector<uint8_t>& plain, const uint8_t* key,
            std::vector<uint8_t>* out, uint8_t hash[16]) {
    init_kirk_once();
    if (plain.size() > 0x7FFFFFE0u) return -1;
    int len = static_cast<int>(plain.size());
    int aligned = align16(len);
    std::vector<uint8_t> buf(static_cast<size_t>(aligned) + 0x10, 0);
    if (len > 0) std::memcpy(buf.data(), plain.data(), plain.size());
    uint8_t k[16];
    if (key != nullptr) std::memcpy(k, key, 16);
    int err = EncryptData(static_cast<unsigned>(mode), buf.data(), &len, &aligned, hash,
                          key != nullptr ? k : nullptr);
    if (err != 0) return err;
    out->assign(buf.begin(), buf.begin() + len);
    return 0;
}

}  // namespace psp_save_crypt
