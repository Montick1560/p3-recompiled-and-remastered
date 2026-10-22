#pragma once
// sceNetInet errno values (PSP numbering, PPSSPP Core/HLE/NetInetConstants.h)
// and the sceNetInetGetPspError code for each, after PPSSPP
// convertInetErrno2PSPError (GPL-2.0-or-later). Pure.
#include <cstdint>

namespace inet_errno {
constexpr int kEINTR = 4, kEBADF = 9, kEAGAIN = 11, kEINVAL = 22, kEPIPE = 32, kEOPNOTSUPP = 95,
              kECONNRESET = 104, kEAFNOSUPPORT = 106, kENOTSOCK = 108, kECONNREFUSED = 111, kEADDRINUSE = 112,
              kENETUNREACH = 114, kEINPROGRESS = 119, kEALREADY = 120, kEMSGSIZE = 122, kEPROTONOSUPPORT = 123,
              kEISCONN = 127, kENOTCONN = 128;
}  // namespace inet_errno

inline uint32_t inet_errno_to_psp(int e) {
    using namespace inet_errno;
    switch (e) {
    case kEINTR: return 0x80010010;           // DEVICE_BUSY
    case kEINVAL: return 0x80010016;          // INVALID_ARGUMENT
    case kEOPNOTSUPP: return 0x8001B000;      // FUNCTION_NOT_SUPPORTED
    case kECONNRESET: return 0x80010068;      // CONNECTION_RESET
    case kEAFNOSUPPORT: return 0x8001B000;    // FUNCTION_NOT_SUPPORTED
    case kENOTSOCK: return 0x80010009;        // INVALID_FILE_DESCRIPTOR
    case kEADDRINUSE: return 0x80010062;      // FILE_ADDR_IN_USE
    case kENETUNREACH: return 0x80010013;     // DEVICE_NOT_FOUND
    case kEMSGSIZE: return 0x8001001B;        // FILE_IS_TOO_BIG
    case kEPROTONOSUPPORT: return 0x8001B000; // FUNCTION_NOT_SUPPORTED
    case kENOTCONN: return 0x80010080;        // NOT_CONNECTED
    case kEAGAIN: return 0x8001000B;          // RESOURCE_UNAVAILABLE
    case kEPIPE: return 0x80010002;           // FILE_NOT_FOUND
    case kECONNREFUSED: return 0x80010011;    // FILE_ALREADY_EXISTS
    case kEINPROGRESS: return 0x80010077;     // IN_PROGRESS
    case kEALREADY: return 0x80010078;        // ALREADY
    // PPSSPP sends EISCONN to ALREADY for UNO; Patapon 3's connect wrapper (0x089B6C0C) takes
    // IS_ALREADY_CONNECTED as connected and ALREADY as still pending.
    case kEISCONN: return 0x8001007F;         // IS_ALREADY_CONNECTED
    default: return static_cast<uint32_t>(e);
    }
}
