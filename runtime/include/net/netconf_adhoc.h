#pragma once
// sceUtilityNetconf decision logic (PPSSPP Core/Dialog/PSPNetconfDialog.cpp):
// the ad hoc actions create, connect or join the requested group and the
// dialog finishes once adhocctl reports CONNECTED. There is no on-screen
// dialog in the port, so where PPSSPP waits for the player to press cancel
// after its 15 s timeout, this finishes with ABORT by itself.
// Infrastructure actions connect to the infrastructure-mode server
// (net/infra_client.h) and finish with success once that attempt is over; a
// failed connection is reported later by the game's own Matching2 messages.
// With PSPRECOMP_INFRA_SERVER=off they finish with ABORT, as before.
#include <cstdint>
#include <string>

namespace netconf {

enum Action : int {
    CONNECT_APNET = 0,
    STATUS_APNET = 1,
    CONNECT_ADHOC = 2,
    CONNECT_APNET_LAST = 3,
    CREATE_ADHOC = 4,
    JOIN_ADHOC = 5,
};

constexpr int kResultSuccess = 0;
constexpr int kResultAbort = 2;
constexpr int64_t kConnectTimeoutMs = 15000;  // NET_CONNECT_TIMEOUT
constexpr int64_t kRescanMs = 500;

enum class Command { None, Create, Connect, Scan, Join, InfraConnect };

struct Input {
    int action = CONNECT_ADHOC;
    bool have_group = true;       // NetconfData pointer valid
    int adhocctl_state = 0;       // adhocctl::State
    bool requested = false;       // create/connect/join already accepted
    bool group_seen = false;      // JOIN: last scan listed the group
    int64_t elapsed_ms = 0;       // since the dialog started running
    int64_t since_scan_ms = 1 << 30;
    bool infra_ok = false;        // infrastructure server configured
    bool infra_pending = false;   // connection to it in progress
};

struct Step {
    bool finished = false;
    int result = kResultSuccess;
    Command command = Command::None;
};

Step step(const Input& in);

}  // namespace netconf
