// sceUtilityNetconf ad hoc decisions, after PPSSPP PSPNetconfDialog::Update
// (GPL-2.0-or-later).
#include "net/netconf_adhoc.h"

namespace netconf {

static constexpr int kStateDisconnected = 0;
static constexpr int kStateConnected = 1;

Step step(const Input& in) {
    Step s;
    const bool adhoc = in.action == CONNECT_ADHOC || in.action == CREATE_ADHOC || in.action == JOIN_ADHOC;
    if (!adhoc) {
        s.finished = true;
        s.result = kResultAbort;
        return s;
    }
    if (in.adhocctl_state == kStateConnected) {
        s.finished = true;
        return s;
    }
    if (in.adhocctl_state == kStateDisconnected && in.elapsed_ms > kConnectTimeoutMs) {
        s.finished = true;
        s.result = kResultAbort;
        return s;
    }
    if (!in.have_group || in.requested || in.adhocctl_state != kStateDisconnected) return s;
    switch (in.action) {
    case CREATE_ADHOC:
        s.command = Command::Create;
        break;
    case JOIN_ADHOC:
        if (in.group_seen) {
            s.command = Command::Join;
        } else if (in.since_scan_ms >= kRescanMs) {
            s.command = Command::Scan;
        }
        break;
    default:
        s.command = Command::Connect;
        break;
    }
    return s;
}

}  // namespace netconf
