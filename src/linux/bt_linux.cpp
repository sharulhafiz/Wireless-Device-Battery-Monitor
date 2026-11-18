// Linux-only Bluetooth battery source for the wireless battery monitor.
//
// Windows reads Bluetooth device levels through the WinRT/SetupAPI paths in src/devices.cpp. On Linux
// the same information is available from two places, both read here without extra dependencies:
//
//   1. BlueZ over D-Bus: every paired+connected device that carries a Battery Service exports
//      org.bluez.Battery1.Percentage on its /org/bluez/hciX/dev_XX_XX.. object. This is what
//      `bluetoothctl info` prints as "Battery Percentage".
//   2. sysfs: the kernel hid-input / hid-logitech-hidpp drivers publish /sys/class/power_supply/
//      hid-<mac>-battery (generic BT HID) and hidpp_battery_N (Logitech HID++ over BT). These exist
//      even for devices whose firmware does not advertise the BLE Battery Service.
//
// Both sources are merged by MAC address so a device reported by both appears once. Devices reachable
// over a 2.4 GHz receiver are still handled by the vendor drivers in src/drivers/; duplicates are
// dropped in src/linux/devices_linux.cpp by comparing normalised product names.
#ifndef __linux__

#include "bt_linux.h"

std::vector<DeviceStatus> PollBluetoothLinux() { return {}; }
bool DumpBluetoothLinux(std::wstring&) { return false; }

#else

#include "bt_linux.h"

#include <gio/gio.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../platform_compat.h"

namespace fs = std::filesystem;

namespace {

constexpr const char* kBluezService = "org.bluez";
constexpr const char* kObjectManager = "org.freedesktop.DBus.ObjectManager";
constexpr const char* kDeviceIface = "org.bluez.Device1";
constexpr const char* kBatteryIface = "org.bluez.Battery1";

struct BtInfo {
    std::string addr;       // "D1:04:38:30:03:C2", empty when sysfs did not tell us
    std::string name;       // BlueZ alias or sysfs model_name
    std::string icon;       // BlueZ icon hint: input-mouse, input-keyboard, audio-headset, ...
    int percent = -1;
    bool hasPercent = false;
    int status = 0;         // 0 unknown, 1 discharging, 2 charging, 3 full
    std::string source;     // "bluez", "sysfs", "bluez+sysfs" (debug output)
};

std::string Trim(std::string s) {
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') a++;
    while (b > a && (unsigned char)s[b - 1] <= ' ') b--;
    return s.substr(a, b - a);
}

std::string UpperMac(const std::string& in) {
    std::string out;
    for (char c : in) out += (char)std::toupper((unsigned char)c);
    return out;
}

// hid-d1:04:38:30:03:c2-battery -> D1:04:38:30:03:C2
std::string MacFromSupplyName(const std::string& dir) {
    std::string s = dir;
    if (s.rfind("hid-", 0) == 0) s = s.substr(4);
    const std::string suffix = "-battery";
    if (s.size() > suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0)
        s = s.substr(0, s.size() - suffix.size());
    // Expect 6 hex groups separated by ':' (or '-').
    int groups = 1;
    int digits = 0;
    for (char c : s) {
        if (c == ':' || c == '-') { groups++; digits = 0; continue; }
        if (!std::isxdigit((unsigned char)c)) return {};
        if (++digits > 2) return {};
    }
    if (groups != 6) return {};
    for (char& c : s)
        if (c == '-') c = ':';
    return UpperMac(s);
}

int ReadInt(const fs::path& p) {
    std::ifstream f(p);
    int v = -1;
    if (f) f >> v;
    return v;
}

std::string ReadLine(const fs::path& p) {
    std::ifstream f(p);
    std::string s;
    if (f) std::getline(f, s);
    return Trim(s);
}

void Merge(BtInfo& into, const BtInfo& from) {
    if (into.addr.empty()) into.addr = from.addr;
    if (from.name.size() > into.name.size()) into.name = from.name;
    if (into.icon.empty()) into.icon = from.icon;
    if (from.hasPercent) {
        into.percent = from.percent;
        into.hasPercent = true;
    }
    if (from.status) into.status = from.status;
    if (!from.source.empty() && from.source != into.source) into.source = "bluez+sysfs";
}

using Found = std::map<std::string, BtInfo>;  // key: MAC when known, else supply/system name

// ---- BlueZ over D-Bus -------------------------------------------------------------------------

void ReadBluez(Found& found, std::string& note) {
    GError* err = nullptr;
    GDBusConnection* conn = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &err);
    if (!conn) {
        note = err ? err->message : "no system bus";
        if (err) g_error_free(err);
        return;
    }
    GVariant* reply = g_dbus_connection_call_sync(
        conn, kBluezService, "/", kObjectManager, "GetManagedObjects", nullptr,
        G_VARIANT_TYPE("(a{oa{sa{sv}}})"), G_DBUS_CALL_FLAGS_NONE, 3000, nullptr, &err);
    if (!reply) {
        note = err ? err->message : "GetManagedObjects failed";
        if (err) g_error_free(err);
        g_object_unref(conn);
        return;
    }

    GVariant* objs = g_variant_get_child_value(reply, 0);
    GVariantIter it;
    g_variant_iter_init(&it, objs);
    const gchar* path = nullptr;
    GVariant* ifaces = nullptr;
    while (g_variant_iter_next(&it, "{&o@a{sa{sv}}}", &path, &ifaces)) {
        GVariant* dev = g_variant_lookup_value(ifaces, kDeviceIface, G_VARIANT_TYPE_VARDICT);
        if (dev) {
            gboolean connected = FALSE;
            g_variant_lookup(dev, "Connected", "b", &connected);
            if (connected) {
                BtInfo info;
                info.source = "bluez";
                const gchar* s = nullptr;
                if (g_variant_lookup(dev, "Address", "&s", &s)) info.addr = UpperMac(s);
                if (g_variant_lookup(dev, "Alias", "&s", &s)) info.name = Trim(s);
                else if (g_variant_lookup(dev, "Name", "&s", &s)) info.name = Trim(s);
                if (g_variant_lookup(dev, "Icon", "&s", &s)) info.icon = s;

                GVariant* bat = g_variant_lookup_value(ifaces, kBatteryIface, G_VARIANT_TYPE_VARDICT);
                if (bat) {
                    guchar pct = 0;
                    if (g_variant_lookup(bat, "Percentage", "y", &pct)) {
                        info.percent = (int)pct;
                        info.hasPercent = true;
                    }
                    g_variant_unref(bat);
                }

                std::string key = info.addr.empty() ? std::string(path) : info.addr;
                auto at = found.find(key);
                if (at == found.end()) found[key] = info;
                else Merge(at->second, info);
            }
            g_variant_unref(dev);
        }
        g_variant_unref(ifaces);
    }
    g_variant_unref(objs);
    g_variant_unref(reply);
    g_object_unref(conn);
}

// ---- sysfs power_supply -----------------------------------------------------------------------

void ReadSysfs(Found& found) {
    std::error_code ec;
    fs::path root("/sys/class/power_supply");
    if (!fs::exists(root, ec)) return;
    std::vector<fs::path> dirs;
    for (const auto& e : fs::directory_iterator(root, ec)) dirs.push_back(e.path());
    std::sort(dirs.begin(), dirs.end());
    for (const auto& d : dirs) {
        // Kernel HID batteries only; BAT0/AC belong to the laptop itself.
        const std::string scope = ReadLine(d / "scope");
        if (scope != "Device") continue;
        BtInfo info;
        info.source = "sysfs";
        info.addr = MacFromSupplyName(d.filename().string());
        info.name = Trim(ReadLine(d / "model_name"));
        if (info.name.empty()) info.name = Trim(ReadLine(d / "manufacturer"));
        const int cap = ReadInt(d / "capacity");
        if (cap >= 0 && cap <= 100) {
            info.percent = cap;
            info.hasPercent = true;
        }
        const std::string st = ReadLine(d / "status");
        if (st == "Charging") info.status = 2;
        else if (st == "Discharging") info.status = 1;
        else if (st == "Full") info.status = 3;
        if (!info.hasPercent && !info.status) continue;

        std::string key = info.addr.empty() ? std::string("sysfs:") + d.filename().string() : info.addr;
        auto at = found.find(key);
        if (at == found.end()) found[key] = info;
        else Merge(at->second, info);
    }
}

DeviceKind KindFromIcon(const std::string& icon) {
    if (icon == "input-mouse") return DeviceKind::Mouse;
    if (icon == "input-keyboard") return DeviceKind::Keyboard;
    if (icon == "input-gaming") return DeviceKind::Gamepad;
    if (icon == "audio-headset" || icon == "audio-headphones" || icon == "audio-card") return DeviceKind::Headset;
    return DeviceKind::Other;
}

std::vector<DeviceStatus> ToStatuses(const Found& found) {
    std::vector<DeviceStatus> out;
    for (const auto& [key, i] : found) {
        DeviceStatus s;
        s.key = L"bt:" + Utf8ToWide(i.addr.empty() ? key : i.addr);
        s.name = i.name.empty() ? L"Bluetooth device" : Utf8ToWide(i.name);
        s.via = L"Bluetooth";
        s.kind = KindFromIcon(i.icon);
        if (i.hasPercent) s.percent = i.percent;
        else if (i.status == 3) s.levelText = L"Full";
        s.charging = (i.status == 2);
        s.connected = true;
        if (s.percent < 0 && s.levelText.empty()) continue;
        out.push_back(s);
    }
    std::sort(out.begin(), out.end(), [](const DeviceStatus& a, const DeviceStatus& b) {
        return a.name < b.name;
    });
    return out;
}

}  // namespace

std::vector<DeviceStatus> PollBluetoothLinux() {
    Found found;
    std::string note;
    ReadBluez(found, note);
    ReadSysfs(found);
    return ToStatuses(found);
}

// --debug only: what BlueZ and sysfs currently expose.
bool DumpBluetoothLinux(std::wstring& out) {
    Found found;
    std::string note;
    ReadBluez(found, note);
    ReadSysfs(found);
    if (!note.empty()) out += L"bluez: " + Utf8ToWide(note) + L"\n";
    for (const auto& [key, i] : found) {
        wchar_t line[512];
        // glibc wide printf: %ls for wchar_t*, %s is char*.
        swprintf(line, 512, L"%-18ls %-28ls %-4d %-14ls %ls\n", Utf8ToWide(i.addr).c_str(),
                 Utf8ToWide(i.name).c_str(), i.hasPercent ? i.percent : -1,
                 Utf8ToWide(i.icon).c_str(), Utf8ToWide(i.source).c_str());
        out += line;
    }
    return !found.empty();
}

#endif  // __linux__
