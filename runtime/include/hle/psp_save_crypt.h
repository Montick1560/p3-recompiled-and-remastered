// PSP savedata encryption (the sceChnnlsv cipher), for loading saves PPSSPP or a real PSP wrote
// encrypted. DecryptData / EncryptData come from PPSSPP Core/Dialog/SavedataParam.cpp
// (GPL-2.0-or-later, Copyright (c) 2012- PPSSPP Project); the cipher itself is the vendored
// third_party/kirk/Core/HLE/sceChnnlsv.cpp. Pure: no guest memory, no files, no logging.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace psp_save_crypt {

// Crypt mode a save was written with, from the first byte of its PARAM.SFO SAVEDATA_PARAMS
// (PPSSPP SavedataParam::GetSaveCryptMode): 0 plain, 1, 3 or 5. Empty params -> 0.
int mode_from_params(const uint8_t* params, size_t size);

// Mode a request implies (PPSSPP SavedataParam::DetermineCryptMode): `full_request` is a
// 1536-byte SceUtilitySavedataParam, `sdk_version` the game's compiled SDK version.
int request_mode(bool full_request, uint32_t secure_version, uint32_t sdk_version);

// Decrypts a whole encrypted save file. `key` is the request's 16-byte key or nullptr (mode 1);
// `expected_hash` the 16-byte hash from SAVEDATA_FILE_LIST or nullptr (no check). Returns 0 and
// the plain bytes in *out, else PPSSPP's negative DecryptData code (-8 = hash mismatch).
int decrypt(int mode, const std::vector<uint8_t>& file, const uint8_t* key,
            const uint8_t* expected_hash, std::vector<uint8_t>* out);

// Encrypts plain bytes the way PPSSPP saves them (used by the tests). Returns 0, the file bytes
// in *out and the 16-byte hash for SAVEDATA_FILE_LIST in hash.
int encrypt(int mode, const std::vector<uint8_t>& plain, const uint8_t* key,
            std::vector<uint8_t>* out, uint8_t hash[16]);

}  // namespace psp_save_crypt
