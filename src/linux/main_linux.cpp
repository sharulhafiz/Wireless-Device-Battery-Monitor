// Linux entry point: a StatusNotifierItem (system tray) app built with GTK4/GIO.
//
// Kept separate from src/main.cpp (Win32 tray + taskbar widget) so the Windows build is not
// touched. The polling logic and all vendor drivers are shared; only the presentation layer and
// settings storage differ.
//
// One tray icon PER DEVICE: every visible device gets its own StatusNotifierItem (own object path,
// own dbusmenu), so a mouse, a keyboard and a headset can be read individually instead of being
// collapsed into a single number. When nothing is visible a single fallback icon keeps the menu
// (refresh / settings / quit) reachable.
#include "bt_linux.h"
#include "../platform_compat.h"
#include "../devices.h"
#include "../hid.h"

#include <gtk/gtk.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <cairo/cairo.h>
#include <unistd.h>

namespace {

const char* kAppId = "com.github.krisgfx9.WirelessBattery";
const char* kSniPath = "/org/kde/StatusNotifierItem";
const char* kMenuPath = "/Menu";
constexpr int kPollSeconds = 60;
constexpr int kLowBattery = 15;
constexpr int kIconSize = 64;

struct MenuNode {
    int id = 0;
    int parent = 0;
    std::string label;
    std::string action;
    bool separator = false;
    bool toggle = false;
    bool checked = false;
    bool enabled = true;
};

// One tray icon: a single device, or the fallback icon shown when nothing is visible.
//
// Every icon lives on its OWN D-Bus connection. Hosts (xapp-sn-watcher on Cinnamon in particular)
// only drop an icon when the *service* that registered it goes away; unregistering just the object
// path leaves a dead icon behind forever. So an icon that has to disappear closes its connection.
struct TrayItem {
    std::string key;          // DeviceStatus::key (UTF-8); empty for the fallback icon
    int slot = 0;             // 0 = fallback, 1..N = per-device
    std::string id;           // SNI "Id" property, unique per host
    GDBusConnection* conn = nullptr;  // private connection: closing it removes the icon
    std::string title;
    std::string tooltipText;
    std::string status = "Active";
    GVariant* pixmap = nullptr;
    std::vector<MenuNode> nodes;  // com.canonical.dbusmenu tree (id 0 is the root)
    int revision = 0;
    guint sniReg = 0;
    guint menuReg = 0;
    bool wanted = false;      // set during a reconcile pass
};

struct State {
    GtkApplication* app = nullptr;
    GDBusConnection* conn = nullptr;
    std::vector<DeviceStatus> devices;
    std::vector<std::unique_ptr<TrayItem>> items;
    std::map<std::string, std::string> hideActions;  // action name -> device key
    int nextSlot = 1;
    std::mutex mu;
    std::condition_variable cv;
    std::vector<DeviceStatus> pending;
    std::atomic<bool> stop{ false };
    bool wake = false;
    std::set<std::wstring> hidden;
    bool hideLastKnown = false;
    bool autostart = false;
};

State g;

void RecomputePresentation();
GVariant* SafeString(const std::string& s);

// ---- settings (GKeyFile under $XDG_CONFIG_HOME) ------------------------------------------------

std::string ConfigPath() {
    return std::string(g_get_user_config_dir()) + "/wireless-battery/config.ini";
}

void LoadSettings() {
    g_autoptr(GKeyFile) kf = g_key_file_new();
    g_key_file_load_from_file(kf, ConfigPath().c_str(), G_KEY_FILE_NONE, nullptr);
    g_autofree gchar** hidden = g_key_file_get_string_list(kf, "general", "hidden", nullptr, nullptr);
    if (hidden) for (int i = 0; hidden[i]; i++) g.hidden.insert(Utf8ToWide(hidden[i]));
    g.hideLastKnown = g_key_file_get_boolean(kf, "general", "hide-last-known", nullptr);
    g.autostart = g_key_file_get_boolean(kf, "general", "autostart", nullptr);
}

void SaveSettings() {
    g_autoptr(GKeyFile) kf = g_key_file_new();
    g_key_file_load_from_file(kf, ConfigPath().c_str(), G_KEY_FILE_NONE, nullptr);
    std::vector<std::string> items;
    for (const auto& k : g.hidden) items.push_back(WideToUtf8(k));
    std::vector<const char*> ptrs;
    for (const auto& s : items) ptrs.push_back(s.c_str());
    if (ptrs.empty()) g_key_file_remove_key(kf, "general", "hidden", nullptr);
    else g_key_file_set_string_list(kf, "general", "hidden", ptrs.data(), ptrs.size());
    g_key_file_set_boolean(kf, "general", "hide-last-known", g.hideLastKnown);
    g_key_file_set_boolean(kf, "general", "autostart", g.autostart);
    std::string path = ConfigPath();
    g_mkdir_with_parents(g_path_get_dirname(path.c_str()), 0700);
    g_key_file_save_to_file(kf, path.c_str(), nullptr);
}

std::string DesktopPath() {
    return std::string(g_get_user_config_dir()) + "/autostart/wireless-battery.desktop";
}

void ApplyAutostart(bool on) {
    std::string path = DesktopPath();
    if (!on) { unlink(path.c_str()); return; }
    g_mkdir_with_parents(g_path_get_dirname(path.c_str()), 0700);
    g_autofree gchar* exe = g_file_read_link("/proc/self/exe", nullptr);
    if (!exe) return;
    std::string content =
        "[Desktop Entry]\nType=Application\nName=Wireless Battery\n"
        "Comment=Show battery levels of wireless devices in the tray\n"
        "Exec=\"" + std::string(exe) + "\"\nNoDisplay=true\nX-GNOME-Autostart-enabled=true\n";
    g_file_set_contents(path.c_str(), content.c_str(), (gssize)content.size(), nullptr);
}

// ---- icon rendering (Cairo -> SNI pixmap) ------------------------------------------------------

GVariant* RenderPixmap(int pct, bool charging, bool dim, bool low) {
    const int S = kIconSize;
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, S, S);
    cairo_t* cr = cairo_create(surface);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);

    std::string text = pct < 0 ? "?" : std::string(pct >= 100 ? "99" : std::to_string(pct));
    double r = low && !charging ? 0.95 : (charging ? 0.25 : 0.92);
    double g = low && !charging ? 0.25 : (charging ? 0.70 : 0.92);
    double b = low && !charging ? 0.28 : (charging ? 1.00 : 0.92);
    double alpha = dim ? 0.45 : 1.0;

    double fontSize = text.size() >= 3 ? 28 : (text.size() == 2 ? 38 : 46);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, fontSize);
    cairo_text_extents_t ext{};
    cairo_text_extents(cr, text.c_str(), &ext);
    cairo_set_source_rgba(cr, r, g, b, alpha);
    cairo_move_to(cr, (S - ext.width) / 2 - ext.x_bearing, S * 0.48 - (ext.height / 2 + ext.y_bearing));
    cairo_show_text(cr, text.c_str());

    // Level bar.
    int barH = 7, barY = S - barH - 2;
    int fill = pct < 0 ? 0 : std::max(1, (pct * (S - 8)) / 100);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.22);
    cairo_rectangle(cr, 4, barY, S - 8, barH);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, charging ? 0.2 : (pct <= kLowBattery ? 0.95 : 0.2),
                          charging ? 0.7 : (pct <= kLowBattery ? 0.25 : 0.8),
                          charging ? 1.0 : (pct <= kLowBattery ? 0.3 : 0.45), alpha);
    cairo_rectangle(cr, 4, barY, fill, barH);
    cairo_fill(cr);
    cairo_destroy(cr);

    cairo_surface_flush(surface);
    unsigned char* data = cairo_image_surface_get_data(surface);
    int stride = cairo_image_surface_get_stride(surface);
    GByteArray* bytes = g_byte_array_new();
    for (int y = 0; y < S; y++) {
        const guint32* row = (const guint32*)(data + y * stride);
        for (int x = 0; x < S; x++) {
            guint32 p = row[x];  // native-endian ARGB32
            guint8 a = (p >> 24) & 0xFF, rr = (p >> 16) & 0xFF, gg = (p >> 8) & 0xFF, bb = p & 0xFF;
            guint8 px[4] = { a, rr, gg, bb };  // SNI wants A,R,G,B bytes
            g_byte_array_append(bytes, px, 4);
        }
    }
    cairo_surface_destroy(surface);

    GVariant* flat = g_variant_new_from_data(
        G_VARIANT_TYPE("ay"), bytes->data, bytes->len, TRUE,
        +[](gpointer p) { g_byte_array_unref(static_cast<GByteArray*>(p)); }, bytes);
    GVariant* item = g_variant_new("(ii@ay)", S, S, flat);
    GVariant* arr = g_variant_new_array(G_VARIANT_TYPE("(iiay)"), &item, 1);
    return g_variant_ref_sink(arr);
}

// ---- presentation state -----------------------------------------------------------------------

std::wstring StatusText(const DeviceStatus& s) {
    if (!s.connected) return L"off / not connected";
    std::wstring t;
    if (!s.levelText.empty() && s.percent >= 0) t = s.levelText;
    else if (s.percent >= 0) t = std::to_wstring(s.percent) + L"%";
    else t = L"unknown";
    if (!s.detail.empty() && s.percent >= 0) t += L" (" + s.detail + L")";
    if (s.charging) t += L", charging";
    if (s.asleep) t += L", asleep";
    return t;
}

bool IsVisible(const DeviceStatus& s) {
    if (g.hidden.count(s.key)) return false;
    if (g.hideLastKnown && (s.asleep || (s.percent < 0))) return false;
    return true;
}

int KindOrder(DeviceKind k) {
    switch (k) {
        case DeviceKind::Headset: return 0;
        case DeviceKind::Mouse: return 1;
        case DeviceKind::Keyboard: return 2;
        case DeviceKind::Gamepad: return 3;
        case DeviceKind::Phone: return 4;
        default: return 5;
    }
}

// Devices that deserve their own icon, in a stable order (kind, then name).
std::vector<const DeviceStatus*> VisibleDevices() {
    std::vector<const DeviceStatus*> list;
    for (const auto& s : g.devices) if (IsVisible(s)) list.push_back(&s);
    std::sort(list.begin(), list.end(), [](const DeviceStatus* a, const DeviceStatus* b) {
        int ka = KindOrder(a->kind), kb = KindOrder(b->kind);
        return ka != kb ? ka < kb : a->name < b->name;
    });
    return list;
}

const DeviceStatus* DeviceByKey(const std::string& key) {
    if (key.empty()) return nullptr;
    for (const auto& s : g.devices)
        if (WideToUtf8(s.key) == key) return &s;
    return nullptr;
}

// ---- com.canonical.dbusmenu -------------------------------------------------------------------
// StatusNotifierItem hosts (KDE Plasma, AppIndicator on GNOME, xapp-sn-watcher on Cinnamon) read
// the context menu through this interface at the object path given by the SNI "Menu" property.

const MenuNode* FindNode(const TrayItem& it, int id) {
    if (id == 0) return nullptr;  // root
    if (id < 0 || (size_t)id > it.nodes.size()) return nullptr;
    return &it.nodes[id - 1];
}

void AddMenuProperties(GVariantBuilder& props, const MenuNode* n) {
    if (!n) {
        g_variant_builder_add(&props, "{sv}", "children-display", g_variant_new_string("submenu"));
        return;
    }
    if (n->separator) {
        g_variant_builder_add(&props, "{sv}", "type", g_variant_new_string("separator"));
        return;
    }
    g_variant_builder_add(&props, "{sv}", "label", SafeString(n->label));
    g_variant_builder_add(&props, "{sv}", "enabled", g_variant_new_boolean(n->enabled));
    g_variant_builder_add(&props, "{sv}", "visible", g_variant_new_boolean(TRUE));
    g_variant_builder_add(&props, "{sv}", "type", g_variant_new_string(n->toggle ? "checkbox" : "standard"));
    if (n->toggle)
        g_variant_builder_add(&props, "{sv}", "toggle-state", g_variant_new_int32(n->checked ? 1 : 0));
}

GVariant* BuildLayout(const TrayItem& it, int id, int depth) {
    GVariantBuilder props;
    g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
    AddMenuProperties(props, FindNode(it, id));
    GVariantBuilder children;
    g_variant_builder_init(&children, G_VARIANT_TYPE("av"));
    if (depth != 0) {
        for (const auto& n : it.nodes)
            if (n.parent == id)
                g_variant_builder_add(&children, "v", BuildLayout(it, n.id, depth > 0 ? depth - 1 : -1));
    }
    GVariant* layout = g_variant_new("(ia{sv}av)", id, &props, &children);
    g_variant_builder_clear(&props);
    g_variant_builder_clear(&children);
    return g_variant_ref_sink(layout);
}

void AddMenuNode(TrayItem& it, int parent, const std::string& label, const std::string& action,
                 bool separator = false, bool toggle = false, bool checked = false) {
    MenuNode n;
    n.id = (int)it.nodes.size() + 1;
    n.parent = parent;
    n.label = label;
    n.action = action;
    n.separator = separator;
    n.toggle = toggle;
    n.checked = checked;
    n.enabled = !action.empty();  // entries without an action are read-only information
    it.nodes.push_back(std::move(n));
}

void EmitSignal(TrayItem& it, const char* name) {
    if (!it.conn) return;
    g_dbus_connection_emit_signal(it.conn, nullptr, kSniPath, "org.kde.StatusNotifierItem",
                                  name, nullptr, nullptr);
}

// Device names come from firmware (HID product strings, BlueZ aliases) and are not guaranteed to be
// valid UTF-8. D-Bus strings must be, so repair anything that is not.
GVariant* SafeString(const std::string& s) {
    if (g_utf8_validate(s.c_str(), (gssize)s.size(), nullptr)) return g_variant_new_string(s.c_str());
    g_autofree gchar* fixed = g_utf8_make_valid(s.c_str(), (gssize)s.size());
    g_warning("Invalid UTF-8 in device string, sanitised: \"%s\"", fixed);
    return g_variant_new_string(fixed);
}

// ---- SNI D-Bus interfaces ---------------------------------------------------------------------

static const char* kSniXml =
    "<node><interface name='org.kde.StatusNotifierItem'>"
    "<property name='Id' type='s' access='read'/>"
    "<property name='Category' type='s' access='read'/>"
    "<property name='Status' type='s' access='read'/>"
    "<property name='Title' type='s' access='read'/>"
    "<property name='IconName' type='s' access='read'/>"
    "<property name='IconPixmap' type='a(iiay)' access='read'/>"
    "<property name='ToolTip' type='(sa(iiay)ss)' access='read'/>"
    "<property name='Menu' type='o' access='read'/>"
    "<property name='ItemIsMenu' type='b' access='read'/>"
    "<signal name='NewIcon'/><signal name='NewToolTip'/><signal name='NewStatus'/>"
    "</interface></node>";

static const char* kAppXml =
    "<node><interface name='com.github.krisgfx9.WirelessBattery'>"
    "<method name='Quit'/>"
    "</interface></node>";

static const char* kMenuXml =
    "<node><interface name='com.canonical.dbusmenu'>"
    "<method name='GetLayout'>"
    "<arg type='i' name='parentId' direction='in'/>"
    "<arg type='i' name='recursionDepth' direction='in'/>"
    "<arg type='as' name='propertyNames' direction='in'/>"
    "<arg type='(ia{sv}av)' name='layout' direction='out'/>"
    "</method>"
    "<method name='GetGroupProperties'>"
    "<arg type='ai' name='ids' direction='in'/>"
    "<arg type='as' name='propertyNames' direction='in'/>"
    "<arg type='a(ia{sv})' name='properties' direction='out'/>"
    "</method>"
    "<method name='GetProperty'>"
    "<arg type='i' name='id' direction='in'/>"
    "<arg type='s' name='name' direction='in'/>"
    "<arg type='v' name='value' direction='out'/>"
    "</method>"
    "<method name='Event'>"
    "<arg type='i' name='id' direction='in'/>"
    "<arg type='s' name='eventId' direction='in'/>"
    "<arg type='v' name='data' direction='in'/>"
    "</method>"
    "<method name='AboutToShow'><arg type='i' name='id' direction='in'/>"
    "<arg type='b' name='needUpdate' direction='out'/></method>"
    "<signal name='LayoutUpdated'><arg type='i' name='revision'/>"
    "<arg type='i' name='parent'/></signal>"
    "<property name='Revision' type='i' access='read'/>"
    "<property name='TextDirection' type='s' access='read'/>"
    "</interface></node>";

static GVariant* sni_get_property(GDBusConnection*, const gchar*, const gchar*, const gchar*,
                                  const gchar* name, GError**, gpointer user_data) {
    TrayItem* it = static_cast<TrayItem*>(user_data);
    if (!it) return nullptr;
    if (!strcmp(name, "Id")) return SafeString(it->id);
    if (!strcmp(name, "Category")) return g_variant_new_string("SystemServices");
    if (!strcmp(name, "Title")) return SafeString(it->title);
    // Empty IconName: the host falls back to the pixmap we draw (percentage text + level bar).
    if (!strcmp(name, "IconName")) return g_variant_new_string("");
    if (!strcmp(name, "IconPixmap"))
        return it->pixmap ? g_variant_ref(it->pixmap) : RenderPixmap(-1, false, true, false);
    if (!strcmp(name, "ToolTip")) {
        GVariant* px = it->pixmap ? g_variant_ref(it->pixmap) : RenderPixmap(-1, false, true, false);
        return g_variant_new("(s@a(iiay)@s@s)", "", px, SafeString(it->title),
                             SafeString(it->tooltipText));
    }
    if (!strcmp(name, "Menu")) return g_variant_new_object_path(kMenuPath);
    if (!strcmp(name, "ItemIsMenu")) return g_variant_new_boolean(TRUE);
    if (!strcmp(name, "Status")) return g_variant_new_string(it->status.c_str());
    return nullptr;
}

static void app_method_call(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar* method,
                            GVariant*, GDBusMethodInvocation* inv, gpointer) {
    if (!strcmp(method, "Quit")) {
        g_dbus_method_invocation_return_value(inv, nullptr);
        g_application_quit(G_APPLICATION(g.app));
    } else g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "Unknown method");
}

static void dbusmenu_method_call(GDBusConnection*, const gchar*, const gchar*, const gchar*,
                                 const gchar* method, GVariant* params,
                                 GDBusMethodInvocation* inv, gpointer user_data) {
    TrayItem* item = static_cast<TrayItem*>(user_data);
    if (!item) {
        g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "No menu registered");
        return;
    }
    if (!strcmp(method, "GetLayout")) {
        int parent = 0, depth = 0;
        GVariant* names = nullptr;
        g_variant_get(params, "(ii@as)", &parent, &depth, &names);
        GVariantBuilder ret;
        g_variant_builder_init(&ret, G_VARIANT_TYPE("((ia{sv}av))"));
        g_variant_builder_add_value(&ret, BuildLayout(*item, parent, depth));
        g_dbus_method_invocation_return_value(inv, g_variant_builder_end(&ret));
        g_variant_unref(names);
        return;
    }
    if (!strcmp(method, "GetGroupProperties")) {
        GVariant* ids = nullptr, *names = nullptr;
        g_variant_get(params, "(@ai@as)", &ids, &names);
        GVariantBuilder ret, out;
        g_variant_builder_init(&out, G_VARIANT_TYPE("a(ia{sv})"));
        GVariantIter iter;
        g_variant_iter_init(&iter, ids);
        gint id = 0;
        while (g_variant_iter_next(&iter, "i", &id)) {
            GVariantBuilder props;
            g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
            AddMenuProperties(props, FindNode(*item, id));
            g_variant_builder_add(&out, "(ia{sv})", id, &props);
            g_variant_builder_clear(&props);
        }
        g_variant_builder_init(&ret, G_VARIANT_TYPE("(a(ia{sv}))"));
        g_variant_builder_add_value(&ret, g_variant_builder_end(&out));
        g_dbus_method_invocation_return_value(inv, g_variant_builder_end(&ret));
        g_variant_unref(ids);
        g_variant_unref(names);
        return;
    }
    if (!strcmp(method, "GetProperty")) {
        int id = 0;
        const char* name = nullptr;
        g_variant_get(params, "(i&s)", &id, &name);
        GVariantBuilder props;
        g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
        AddMenuProperties(props, FindNode(*item, id));
        GVariant* dict = g_variant_builder_end(&props);
        GVariant* value = g_variant_lookup_value(dict, name, nullptr);
        if (value) {
            GVariantBuilder ret;
            g_variant_builder_init(&ret, G_VARIANT_TYPE("(v)"));
            g_variant_builder_add(&ret, "v", value);
            g_dbus_method_invocation_return_value(inv, g_variant_builder_end(&ret));
            g_variant_unref(value);
        } else {
            g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                                  "No such property: %s", name);
        }
        g_variant_unref(dict);
        return;
    }
    if (!strcmp(method, "Event")) {
        int id = 0;
        const char* event = nullptr;
        GVariant* data = nullptr;
        g_variant_get(params, "(i&s@v)", &id, &event, &data);
        const MenuNode* node = FindNode(*item, id);
        if (node && !node->action.empty() && !strcmp(event, "clicked")) {
            // Activate from an idle callback: an action can destroy this very item (and close the
            // connection this method call arrived on), so the reply must go out first.
            char* action = g_strdup(node->action.c_str());
            g_idle_add(+[](gpointer p) -> gboolean {
                char* name = static_cast<char*>(p);
                if (g.app) g_action_group_activate_action(G_ACTION_GROUP(g.app), name, nullptr);
                g_free(name);
                return G_SOURCE_REMOVE;
            }, action);
        }
        g_variant_unref(data);
        g_dbus_method_invocation_return_value(inv, nullptr);
        return;
    }
    if (!strcmp(method, "AboutToShow")) {
        g_dbus_method_invocation_return_value(inv, g_variant_new("(b)", TRUE));
        return;
    }
    g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "Unknown method");
}

static GVariant* dbusmenu_get_property(GDBusConnection*, const gchar*, const gchar*, const gchar*,
                                       const gchar* name, GError**, gpointer user_data) {
    TrayItem* it = static_cast<TrayItem*>(user_data);
    if (!strcmp(name, "Revision")) return g_variant_new_int32(it ? it->revision : 0);
    if (!strcmp(name, "TextDirection")) return g_variant_new_string("ltr");
    return nullptr;
}

static GDBusInterfaceVTable sniVtable = { nullptr, sni_get_property, nullptr };
static GDBusInterfaceVTable appVtable = { app_method_call, nullptr, nullptr };
static GDBusInterfaceVTable dbusmenuVtable = { dbusmenu_method_call, dbusmenu_get_property, nullptr };

void RegisterWithWatcher(GDBusConnection* conn, const char* path) {
    const char* watchers[] = { "org.kde.StatusNotifierWatcher", "org.freedesktop.StatusNotifierWatcher" };
    for (auto w : watchers) {
        GError* error = nullptr;
        GVariant* reply = g_dbus_connection_call_sync(conn, w, "/StatusNotifierWatcher", w,
                                                      "RegisterStatusNotifierItem",
                                                      g_variant_new("(s)", path), nullptr,
                                                      G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &error);
        if (reply) { g_variant_unref(reply); if (error) g_error_free(error); return; }
        if (error) g_error_free(error);
    }
}

// Registers the SNI + dbusmenu objects for one icon on a private connection.
TrayItem* CreateItem(const std::string& key, int slot) {
    auto item = std::make_unique<TrayItem>();
    item->key = key;
    item->slot = slot;
    item->id = slot == 0 ? "wireless-battery" : "wireless-battery-" + std::to_string(slot);
    if (!g.conn) return nullptr;

    // Private bus connection: an icon is removed by closing it (see TrayItem).
    GError* error = nullptr;
    g_autofree gchar* address = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (!address) {
        if (error) { g_warning("Session bus address: %s", error->message); g_error_free(error); }
        return nullptr;
    }
    item->conn = g_dbus_connection_new_for_address_sync(
        address,
        (GDBusConnectionFlags)(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                               G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
        nullptr, nullptr, &error);
    if (!item->conn) {
        if (error) { g_warning("Item bus connection: %s", error->message); g_error_free(error); }
        return nullptr;
    }

    GDBusNodeInfo* node = g_dbus_node_info_new_for_xml(kSniXml, &error);
    if (node) {
        item->sniReg = g_dbus_connection_register_object(item->conn, kSniPath, node->interfaces[0],
                                                         &sniVtable, item.get(), nullptr, &error);
        if (error) {
            g_warning("Register SNI object: %s", error->message);
            g_error_free(error);
            error = nullptr;
        }
        g_dbus_node_info_unref(node);
    }
    node = g_dbus_node_info_new_for_xml(kMenuXml, &error);
    if (node) {
        item->menuReg = g_dbus_connection_register_object(item->conn, kMenuPath, node->interfaces[0],
                                                          &dbusmenuVtable, item.get(), nullptr, &error);
        if (error) {
            g_warning("Register menu object: %s", error->message);
            g_error_free(error);
            error = nullptr;
        }
        g_dbus_node_info_unref(node);
    }
    RegisterWithWatcher(item->conn, kSniPath);
    g.items.push_back(std::move(item));
    return g.items.back().get();
}

void DestroyItem(TrayItem* it) {
    if (it->pixmap) {
        g_variant_unref(it->pixmap);
        it->pixmap = nullptr;
    }
    if (it->conn) {
        // Closing the connection is what makes the host drop the icon.
        g_dbus_connection_close_sync(it->conn, nullptr, nullptr);
        g_object_unref(it->conn);
        it->conn = nullptr;
    }
    it->sniReg = it->menuReg = 0;
}

TrayItem* FindItem(const std::string& key) {
    for (auto& p : g.items)
        if (p->key == key) return p.get();
    return nullptr;
}

// ---- per-item content -------------------------------------------------------------------------

void BuildMenuFor(TrayItem& it, const DeviceStatus* dev) {
    it.nodes.clear();
    if (dev) {
        AddMenuNode(it, 0, WideToUtf8(dev->name), "");
        AddMenuNode(it, 0, WideToUtf8(StatusText(*dev)), "");
        AddMenuNode(it, 0, "", "", true);
    } else {
        AddMenuNode(it, 0, "No devices with battery info found", "");
        AddMenuNode(it, 0, "", "", true);
    }
    AddMenuNode(it, 0, "Refresh now", "refresh");
    AddMenuNode(it, 0, g.autostart ? "Start at login (on)" : "Start at login (off)",
                "toggle-autostart", false, true, g.autostart);
    AddMenuNode(it, 0, g.hideLastKnown ? "Hide last known / asleep devices (on)"
                                       : "Hide last known / asleep devices (off)",
                "toggle-last-known", false, true, g.hideLastKnown);
    if (!g.hidden.empty())
        AddMenuNode(it, 0, "Show hidden devices (" + std::to_string(g.hidden.size()) + ")", "unhide-all");
    AddMenuNode(it, 0, "", "", true);
    if (dev && !it.key.empty()) {
        std::string hideAction = "hide-" + std::to_string(it.slot);
        if (g.hideActions.count(hideAction)) AddMenuNode(it, 0, "Hide this device", hideAction);
    }
    AddMenuNode(it, 0, "Quit", "quit");
}

// Updates icon, tooltip and status of one item from the device snapshot.
void UpdateItemContent(TrayItem& it, const DeviceStatus* dev) {
    int pct = -1;
    bool charging = false, dim = false, low = false;
    if (dev) {
        pct = dev->percent;
        charging = dev->charging;
        low = pct >= 0 && pct <= kLowBattery && !charging;
        dim = dev->asleep || pct < 0;
        it.title = WideToUtf8(dev->name);
        it.tooltipText = WideToUtf8(StatusText(*dev));
        it.status = low ? "NeedsAttention" : (pct < 0 || dev->asleep ? "Passive" : "Active");
    } else {
        it.title = "Wireless Battery";
        it.tooltipText = "No devices with battery info found";
        it.status = "Passive";
        dim = true;
    }
    if (it.pixmap) g_variant_unref(it.pixmap);
    it.pixmap = RenderPixmap(pct, charging, dim, low);
}

// ---- actions -----------------------------------------------------------------------------------

void OnRefresh(GSimpleAction*, GVariant*, gpointer) {
    { std::lock_guard<std::mutex> lock(g.mu); g.wake = true; }
    g.cv.notify_all();
}

void OnToggleLastKnown(GSimpleAction* action, GVariant*, gpointer) {
    g.hideLastKnown = !g.hideLastKnown;
    g_simple_action_set_state(action, g_variant_new_boolean(g.hideLastKnown));
    SaveSettings();
    RecomputePresentation();
}

void OnToggleAutostart(GSimpleAction* action, GVariant*, gpointer) {
    g.autostart = !g.autostart;
    g_simple_action_set_state(action, g_variant_new_boolean(g.autostart));
    SaveSettings();
    ApplyAutostart(g.autostart);
    RecomputePresentation();
}

void OnUnhideAll(GSimpleAction*, GVariant*, gpointer) {
    g.hidden.clear();
    SaveSettings();
    RecomputePresentation();
}

void OnHideDevice(GSimpleAction* action, GVariant*, gpointer) {
    const char* name = g_action_get_name(G_ACTION(action));
    auto at = g.hideActions.find(name);
    if (at == g.hideActions.end()) return;
    g.hidden.insert(Utf8ToWide(at->second));
    SaveSettings();
    RecomputePresentation();
}

const GActionEntry kActions[] = {
    { "refresh", OnRefresh, nullptr, nullptr, nullptr, {} },
    { "toggle-last-known", OnToggleLastKnown, nullptr, "false", nullptr, {} },
    { "toggle-autostart", OnToggleAutostart, nullptr, "false", nullptr, {} },
    { "unhide-all", OnUnhideAll, nullptr, nullptr, nullptr, {} },
};

// One "hide-<slot>" action per device icon, rebuilt whenever the device set changes.
void RebuildHideActions() {
    for (const auto& entry : g.hideActions)
        g_action_map_remove_action(G_ACTION_MAP(g.app), entry.first.c_str());
    g.hideActions.clear();
    for (auto& p : g.items) {
        if (p->key.empty()) continue;
        std::string name = "hide-" + std::to_string(p->slot);
        GSimpleAction* a = g_simple_action_new(name.c_str(), nullptr);
        g_signal_connect(a, "activate", G_CALLBACK(OnHideDevice), nullptr);
        g_action_map_add_action(G_ACTION_MAP(g.app), G_ACTION(a));
        g_object_unref(a);
        g.hideActions[name] = p->key;
    }
}

// ---- reconcile + repaint ----------------------------------------------------------------------

void RecomputePresentation() {
    if (!g.conn || !g.app) return;

    const std::vector<const DeviceStatus*> visible = VisibleDevices();
    for (auto& p : g.items) p->wanted = false;

    // One icon per visible device; an existing icon for the same key is reused across polls.
    for (const DeviceStatus* s : visible) {
        const std::string key = WideToUtf8(s->key);
        TrayItem* it = FindItem(key);
        if (!it) it = CreateItem(key, g.nextSlot++);
        if (it) it->wanted = true;
    }

    // Nothing visible: keep a single fallback icon so refresh/settings/quit stay reachable.
    if (visible.empty()) {
        TrayItem* fb = FindItem("");
        if (!fb) fb = CreateItem("", 0);
        if (fb) fb->wanted = true;
    }

    // Icons whose device disappeared or was hidden go away.
    for (auto& p : g.items)
        if (!p->wanted) DestroyItem(p.get());
    g.items.erase(std::remove_if(g.items.begin(), g.items.end(),
                                 [](const std::unique_ptr<TrayItem>& p) { return !p->wanted; }),
                  g.items.end());

    RebuildHideActions();

    // Repaint every surviving icon and its menu (menus show the hidden-device count).
    for (auto& p : g.items) {
        TrayItem& it = *p;
        const DeviceStatus* dev = DeviceByKey(it.key);
        UpdateItemContent(it, dev);
        BuildMenuFor(it, dev);
        it.revision++;
        EmitSignal(it, "NewIcon");
        EmitSignal(it, "NewToolTip");
        EmitSignal(it, "NewStatus");
        if (it.conn)
            g_dbus_connection_emit_signal(it.conn, nullptr, kMenuPath, "com.canonical.dbusmenu",
                                          "LayoutUpdated", g_variant_new("(ii)", it.revision, 0), nullptr);
    }
}

void ApplyResults() {
    {
        std::lock_guard<std::mutex> lock(g.mu);
        g.devices.swap(g.pending);
    }
    RecomputePresentation();
}

gboolean PushResults(gpointer) {
    ApplyResults();
    return G_SOURCE_REMOVE;
}

void OnActivate(GApplication*) {
    // Tray-only app: nothing to present on activation.
}

// ---- worker ------------------------------------------------------------------------------------

void Worker() {
    while (!g.stop) {
        std::vector<DeviceStatus> result = PollAllDevices();
        {
            std::lock_guard<std::mutex> lock(g.mu);
            g.pending = std::move(result);
        }
        g_main_context_invoke(nullptr, PushResults, nullptr);
        std::unique_lock<std::mutex> lock(g.mu);
        g.cv.wait_for(lock, std::chrono::seconds(kPollSeconds), [&] { return g.stop || g.wake; });
        g.wake = false;
    }
}

// ---- CLI ---------------------------------------------------------------------------------------

int ListToConsole(bool dump) {
    if (dump) {
        std::wstring s = DumpHidCollections();
        fwrite(WideToUtf8(s).c_str(), 1, WideToUtf8(s).size(), stdout);
        return 0;
    }
    auto list = PollAllDevices();
    if (list.empty()) printf("No devices with battery info found.\n");
    for (const auto& s : list) {
        std::wstring line = s.name + L"  [" + s.via + L"]  " + StatusText(s);
        if (s.millivolts) line += L", " + std::to_wstring(s.millivolts) + L" mV";
        line += L"   (" + s.key + L")\n";
        std::string u = WideToUtf8(line);
        fwrite(u.c_str(), 1, u.size(), stdout);
    }
    return 0;
}

// --bluetooth: raw view of what BlueZ and sysfs report (helps adding new devices).
int DumpBluetooth() {
    std::wstring s;
    bool any = DumpBluetoothLinux(s);
    const std::string u = WideToUtf8(s);
    fwrite(u.c_str(), 1, u.size(), stdout);
    if (!any) printf("No Bluetooth battery source found (nothing paired/connected?).\n");
    return 0;
}

int QuitRunning() {
    GError* error = nullptr;
    GDBusConnection* conn = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (!conn) { fprintf(stderr, "Cannot connect to session bus: %s\n", error->message); g_error_free(error); return 1; }
    GVariant* reply = g_dbus_connection_call_sync(conn, kAppId, kSniPath,
                                                  "com.github.krisgfx9.WirelessBattery", "Quit",
                                                  nullptr, nullptr, G_DBUS_CALL_FLAGS_NONE, 3000, nullptr, &error);
    if (!reply) { fprintf(stderr, "Is Wireless Battery running? %s\n", error->message); g_error_free(error); }
    else g_variant_unref(reply);
    g_object_unref(conn);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--list")) return ListToConsole(false);
        if (!strcmp(argv[i], "--dump")) return ListToConsole(true);
        if (!strcmp(argv[i], "--bluetooth")) return DumpBluetooth();
        if (!strcmp(argv[i], "--quit")) return QuitRunning();
    }

    LoadSettings();
    g_autoptr(GError) error = nullptr;
    if (!gtk_init_check()) { fprintf(stderr, "Cannot initialise GTK (no display?).\n"); return 1; }

    g.app = GTK_APPLICATION(gtk_application_new(kAppId, G_APPLICATION_DEFAULT_FLAGS));
    g_action_map_add_action_entries(G_ACTION_MAP(g.app), kActions, G_N_ELEMENTS(kActions), nullptr);

    if (!g_application_register(G_APPLICATION(g.app), nullptr, &error)) {
        fprintf(stderr, "Cannot register application (already running?): %s\n", error->message);
        return 1;
    }
    // GApplication already owns kAppId; reuse its connection for the tray items.
    g.conn = g_application_get_dbus_connection(G_APPLICATION(g.app));

    // The app interface (used by --quit) always lives at the well-known path.
    GDBusNodeInfo* node = g_dbus_node_info_new_for_xml(kAppXml, &error);
    if (node) {
        g_dbus_connection_register_object(g.conn, kSniPath, node->interfaces[0], &appVtable,
                                          nullptr, nullptr, &error);
        if (error) { g_warning("Register app object: %s", error->message); g_error_free(error); }
        g_dbus_node_info_unref(node);
    }

    g_signal_connect(g.app, "activate", G_CALLBACK(OnActivate), nullptr);
    // No GTK window is ever created (tray only), so hold the application explicitly.
    g_application_hold(G_APPLICATION(g.app));

    // First paint (before the worker's first poll) so the icons exist immediately at login.
    RecomputePresentation();

    std::thread worker(Worker);
    int status = g_application_run(G_APPLICATION(g.app), argc, argv);
    g.stop = true;
    g.cv.notify_all();
    worker.join();
    return status;
}
