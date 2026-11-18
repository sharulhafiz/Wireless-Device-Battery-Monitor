#pragma once
// Linux-only Bluetooth battery source (BlueZ D-Bus + sysfs). See bt_linux.cpp.
#include <string>
#include <vector>

#include "../devices.h"

// Paired and connected Bluetooth devices that report a battery level. Empty on non-Linux builds.
std::vector<DeviceStatus> PollBluetoothLinux();

// Fills `out` with the raw BlueZ/sysfs view (helps when adding support for a new device).
// Returns false when nothing was found.
bool DumpBluetoothLinux(std::wstring& out);
