// Cable-switch detection: a wireless device that leaves the air while a USB HID device of the same
// class shows up is being charged through a cable (its firmware reports no battery over USB).
//
// The logic is in main_linux.cpp, so this test includes that translation unit with WB_NO_MAIN and
// exercises the state machine directly: a real unplug/replug is not needed to verify pairing.
#include "../src/linux/main_linux.cpp"

#include <cassert>
#include <cstdio>

namespace {

const char* kMouseCable = "0003:0000248A:00005B2E|XCTECH USB Gaming Mouse";
const char* kTouchpad = "0018:000006CB:0000CEA4|SYNA30D9:00 06CB:CEA4";
const char* kKeyboardCable = "0003:0000046D:0000C52B|USB Keyboard";

void Reset() {
    g.lastSeen.clear();
    g.remembered.clear();
    g.usbEvents.clear();
    g.devices.clear();
    g.view.clear();
    g.hidSigs.clear();
    unlink(CablePath().c_str());
}

gint64 Now() { return (gint64)time(nullptr); }  // lastSeen uses wall clock (it is persisted)

DeviceStatus Wireless(DeviceKind kind, const wchar_t* name, int percent) {
    DeviceStatus d;
    d.key = std::wstring(L"bt:") + name;
    d.name = name;
    d.via = L"Bluetooth";
    d.kind = kind;
    d.percent = percent;
    d.connected = true;
    return d;
}

void CaseBaseline() {
    Reset();
    const DeviceStatus mouse = Wireless(DeviceKind::Mouse, L"AULA-SC580X", 8);
    // Last seen on the air with no USB mouse plugged in.
    g.lastSeen["bt:AULA-SC580X"] = State::SeenEntry{ mouse, Now(), { kTouchpad } };
    g.hidSigs = { kTouchpad, kMouseCable };  // cable now plugged in

    DetectCableSwitch();
    assert(g.remembered.size() == 1);
    assert(g.remembered[0].key == "bt:AULA-SC580X");
    assert(g.remembered[0].percent == 8);
    assert(g.remembered[0].usbSig == kMouseCable);
    printf("ok  baseline: mouse paired with the USB mouse, level 8%% carried over\n");
}

void CaseEvent() {
    Reset();
    const DeviceStatus mouse = Wireless(DeviceKind::Mouse, L"AULA-SC580X", 12);
    // Baseline already contains the cable device (it was plugged in while the mouse was on the air),
    // so only the recorded plug event can explain the switch.
    g.lastSeen["bt:AULA-SC580X"] = State::SeenEntry{ mouse, Now(), { kTouchpad, kMouseCable } };
    g.hidSigs = { kTouchpad, kMouseCable };
    g.usbEvents.push_back({ kMouseCable, true, Now() - 5 });

    DetectCableSwitch();
    assert(g.remembered.size() == 1);
    assert(g.remembered[0].usbSig == kMouseCable);
    printf("ok  event: a recorded plug event pairs even when the baseline has the device\n");
}

void CaseKindGuard() {
    Reset();
    const DeviceStatus mouse = Wireless(DeviceKind::Mouse, L"AULA-SC580X", 15);
    g.lastSeen["bt:AULA-SC580X"] = State::SeenEntry{ mouse, Now(), { kTouchpad } };
    g.hidSigs = { kTouchpad, kKeyboardCable };  // a keyboard was plugged in, not the mouse

    DetectCableSwitch();
    assert(g.remembered.empty());
    printf("ok  guard: a plugged-in keyboard does not make the mouse look like it is charging\n");
}

void CaseLive() {
    Reset();
    const DeviceStatus mouse = Wireless(DeviceKind::Mouse, L"AULA-SC580X", 20);
    g.lastSeen["bt:AULA-SC580X"] = State::SeenEntry{ mouse, Now(), { kTouchpad } };
    g.devices = { mouse };  // still on the air
    g.hidSigs = { kTouchpad, kMouseCable };

    DetectCableSwitch();
    assert(g.remembered.empty());
    printf("ok  live: a device that is still on Bluetooth is never treated as cabled\n");
}

void CaseStale() {
    Reset();
    const DeviceStatus mouse = Wireless(DeviceKind::Mouse, L"AULA-SC580X", 33);
    g.lastSeen["bt:AULA-SC580X"] = State::SeenEntry{ mouse, Now() - 3600, { kTouchpad } };
    g.hidSigs = { kTouchpad, kMouseCable };

    DetectCableSwitch();
    assert(g.remembered.empty());
    printf("ok  stale: a device last seen an hour ago is not resurrected as charging\n");
}

void CasePresentation() {
    Reset();
    State::Remembered r;
    r.key = "bt:AULA-SC580X";
    r.name = L"AULA-SC580X";
    r.percent = 8;
    r.kind = DeviceKind::Mouse;
    r.usbSig = kMouseCable;
    g.remembered.push_back(r);

    RebuildCableView(std::set<std::string>{ kTouchpad, kMouseCable });
    assert(g.view.size() == 1);
    assert(g.view[0].charging);
    assert(g.view[0].percent == 8);
    assert(g.view[0].via == L"USB cable");
    assert(g.view[0].detail == L"via cable");
    printf("ok  view: cable entry is shown as charging at the last known level\n");

    // Pulling the cable removes the entry.
    RebuildCableView(std::set<std::string>{ kTouchpad });
    assert(g.remembered.empty());
    assert(g.view.empty());
    assert(g_file_test(CablePath().c_str(), G_FILE_TEST_EXISTS) == FALSE);
    printf("ok  view: pulling the cable drops the entry and its file\n");
}

void CasePersistence() {
    Reset();
    State::Remembered r;
    r.key = "bt:AULA-SC580X";
    r.name = L"AULA-SC580X";
    r.percent = 8;
    r.kind = DeviceKind::Mouse;
    r.usbSig = kMouseCable;
    g.remembered.push_back(r);
    SaveRemembered();
    g.remembered.clear();

    LoadRemembered();
    assert(g.remembered.size() == 1);
    assert(g.remembered[0].key == "bt:AULA-SC580X");
    assert(g.remembered[0].name == L"AULA-SC580X");
    assert(g.remembered[0].percent == 8);
    assert(g.remembered[0].usbSig == kMouseCable);
    printf("ok  persistence: entries survive a restart\n");
}

}  // namespace

int main() {
    CaseBaseline();
    CaseEvent();
    CaseKindGuard();
    CaseLive();
    CaseStale();
    CasePresentation();
    CasePersistence();
    printf("cable switch tests passed\n");
    return 0;
}
