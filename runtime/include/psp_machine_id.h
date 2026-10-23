#pragma once
// A stable id of the host machine (not of the game folder), used to bind the
// ad hoc MAC file to the PC that generated it. Never log it raw: only its
// adhoc_machine_hash().
#include <string>

// Windows: the MachineGuid registry value, else the computer name.
// POSIX: /etc/machine-id, else /var/lib/dbus/machine-id, else the host name.
// Empty when all of them fail.
std::string psp_machine_id();
