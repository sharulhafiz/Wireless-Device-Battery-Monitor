// Linux driver orchestration.
//
// Deliberately separate from src/devices.cpp so the Windows build is untouched. This first phase
// enables only the well-known vendors whose drivers do not depend on Windows-specific device
// paths. Add more Poll* calls here (and the matching .cpp to Makefile.linux) in later phases.
#include "../devices.h"

#include <algorithm>
#include <cstdio>
#include <cwctype>

#include "../drivers/drivers.h"
#include "../hid.h"
#include "bt_linux.h"

namespace {

std::wstring NormName(const std::wstring& s) {
    std::wstring out;
    bool pending = false;
    for (wchar_t c : s) {
        if (std::iswspace(c)) {
            pending = !out.empty();
            continue;
        }
        if (pending) {
            out += L' ';
            pending = false;
        }
        out += (wchar_t)std::towlower(c);
    }
    return out;
}

// A device reachable over Bluetooth shows up twice: once through hidraw (vendor driver) and once
// through BlueZ/sysfs. Match on the product name so the tray shows one entry per device.
bool SameDevice(const std::wstring& a, const std::wstring& b) {
    std::wstring x = NormName(a), y = NormName(b);
    if (x.empty() || y.empty()) return false;
    if (x == y) return true;
    if (x.size() >= 6 && y.size() >= 6 &&
        (x.find(y) != std::wstring::npos || y.find(x) != std::wstring::npos))
        return true;
    return false;
}

// Appends the Bluetooth devices that no HID driver reported.
void AddBluetooth(std::vector<DeviceStatus>& out) {
    for (const DeviceStatus& bt : PollBluetoothLinux()) {
        auto it = std::find_if(out.begin(), out.end(),
                               [&](const DeviceStatus& s) { return SameDevice(s.name, bt.name); });
        if (it == out.end()) {
            out.push_back(bt);
            continue;
        }
        // The vendor driver saw the device but could not read a level (typical for Bluetooth
        // firmware that needs the GATT Battery Service): the BlueZ/sysfs value wins.
        if (it->percent < 0 && bt.percent >= 0) *it = bt;
    }
}

}  // namespace

std::vector<DeviceStatus> PollAllDevices() {
    PollContext ctx;
    ctx.colls = EnumHid();

    // Phase 1: famous vendors, HID only.
    PollGlorious(ctx);       // also G-Wolves / Hitscan (defines the shared G-Wolves model table)
    PollKysona(ctx);         // also Pulsar, ATK/VXE and other popular Chinese mice
    PollLogitech(ctx);       // Unifying / Bolt / Lightspeed / Bluetooth HID++
    PollRazer(ctx);
    PollSteelSeries(ctx);
    PollCorsair(ctx);
    PollHyperX(ctx);
    PollPlayStation(ctx);    // DualShock 4 / DualSense over USB or Bluetooth HID
    PollCoolerMaster(ctx);
    PollFinalmouse(ctx);     // UltralightX / Starlight X dongles

    // Bluetooth (BlueZ + sysfs) covers the devices no HID driver speaks for.
    AddBluetooth(ctx.out);
    return ctx.out;
}

std::wstring DumpHidCollections() {
    std::wstring out = L"VID:PID    IF  UsagePg:Usage  In  Out Feat  Product\n";
    for (const auto& c : EnumHid()) {
        wchar_t line[256];
        swprintf(line, 256, L"%04X:%04X  %2d  %04X:%04X    %3u %3u %3u   ", c.vid, c.pid, c.iface,
                 c.page(), c.usage(), c.inLen(), c.outLen(), c.featLen());
        out += line + HidProduct(c) + L"\n";
    }
    return out;
}
