#include "hdr_control.h"
#include "debug.h"
#include <windows.h>
#include <wingdi.h>
#include <winreg.h>
#include <cstdio>
#include <vector>

#ifndef DISPLAYCONFIG_DEVICE_INFO_TYPE_GET_ADVANCED_COLOR_INFO
#define DISPLAYCONFIG_DEVICE_INFO_TYPE_GET_ADVANCED_COLOR_INFO  9
#define DISPLAYCONFIG_DEVICE_INFO_TYPE_SET_ADVANCED_COLOR_STATE 10
#endif

namespace hdr_control {

static bool g_force_keyboard = false;
void set_force_keyboard_mode(bool f) { g_force_keyboard = f; }
bool get_force_keyboard_mode()     { return g_force_keyboard; }

// ── Keyboard fallback (Win+Alt+B) ──────────────────────────
static void send_win_alt_b() {
    INPUT keys[3] = {};
    auto mk = [](INPUT& in, WORD vk, WORD sc, bool up, bool ext) {
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = vk; in.ki.wScan = sc;
        in.ki.dwFlags = KEYEVENTF_SCANCODE
            | (up ? KEYEVENTF_KEYUP : 0)
            | (ext ? KEYEVENTF_EXTENDEDKEY : 0);
    };
    mk(keys[0], VK_LWIN, 0xE05B, false, true);
    mk(keys[1], VK_MENU, 0x38,   false, false);
    mk(keys[2], 'B',     0x30,   false, false);
    SendInput(3, keys, sizeof(INPUT)); Sleep(80);
    mk(keys[0], 'B',     0x30,   true, false);
    mk(keys[1], VK_MENU, 0x38,   true, false);
    mk(keys[2], VK_LWIN, 0xE05B, true, true);
    SendInput(3, keys, sizeof(INPUT));
}

void toggle_via_keyboard() { send_win_alt_b(); }

// ── DisplayConfig helpers ──────────────────────────────────
static bool enum_paths(std::vector<DISPLAYCONFIG_PATH_INFO>& paths,
                       std::vector<DISPLAYCONFIG_MODE_INFO>& modes) {
    UINT32 pc = 0, mc = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pc, &mc) != ERROR_SUCCESS)
        return false;
    if (pc == 0) return false;
    paths.resize(pc); modes.resize(mc);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pc, paths.data(),
                           &mc, modes.data(), nullptr) != ERROR_SUCCESS)
        return false;
    paths.resize(pc); modes.resize(mc);
    return !paths.empty();
}

// ── Public API ──────────────────────────────────────────────
bool query_hdr_state() {
    if (g_force_keyboard) return false;

    // 1. Try DisplayConfig API (Win10 2004+)
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    if (enum_paths(paths, modes)) {
        for (auto& p : paths) {
            DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO info = {};
            info.header.type = (DISPLAYCONFIG_DEVICE_INFO_TYPE)
                DISPLAYCONFIG_DEVICE_INFO_TYPE_GET_ADVANCED_COLOR_INFO;
            info.header.size = sizeof(info);
            info.header.adapterId = p.targetInfo.adapterId;
            info.header.id = p.targetInfo.id;
            if (DisplayConfigGetDeviceInfo(&info.header) == ERROR_SUCCESS) {
                if (info.value & 0x2) return true;      // advancedColorEnabled
            }
        }
    }

    // 2. Fallback: check registry
    //    HKCU\Software\Microsoft\Windows\CurrentVersion\VideoSettings\EnableHDROutput
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\VideoSettings",
        0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD val = 0, size = sizeof(val);
        LONG ret = RegQueryValueExW(hKey, L"EnableHDROutput",
            nullptr, nullptr, (LPBYTE)&val, &size);
        RegCloseKey(hKey);
        if (ret == ERROR_SUCCESS) return val != 0;
    }

    return false;
}

static void kb_set_hdr(bool enable) {
    bool current = query_hdr_state();
    debug::log("HDR kb: current=%s want=%s", current ? "ON" : "OFF", enable ? "ON" : "OFF");
    if (current != enable) send_win_alt_b();
}

bool set_hdr_enabled(bool enable) {
    debug::log("HDR set_hdr_enabled(%s) force_keyboard=%d", enable ? "ON" : "OFF", g_force_keyboard);
    if (g_force_keyboard) { kb_set_hdr(enable); return true; }
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    if (!enum_paths(paths, modes)) {
        debug::log("HDR: enum_paths failed, keyboard fallback");
        kb_set_hdr(enable); return true;
    }
    debug::log("HDR: found %zu active display paths", paths.size());

    bool changed = false, apiOk = false;
    for (auto& p : paths) {
        DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO gi = {};
        gi.header.type = (DISPLAYCONFIG_DEVICE_INFO_TYPE)
            DISPLAYCONFIG_DEVICE_INFO_TYPE_GET_ADVANCED_COLOR_INFO;
        gi.header.size = sizeof(gi);
        gi.header.adapterId = p.targetInfo.adapterId;
        gi.header.id = p.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&gi.header) != ERROR_SUCCESS) {
            debug::log("HDR: DisplayConfigGetDeviceInfo failed for id %u", p.targetInfo.id);
            continue;
        }
        apiOk = true;
        bool supported = (gi.value & 0x1) != 0;
        bool enabled   = (gi.value & 0x2) != 0;
        debug::log("HDR: id=%u supported=%d enabled=%d value=%08x", p.targetInfo.id, supported, enabled, gi.value);
        if (!supported) continue;
        if (enabled == enable) { changed = true; continue; }

        DISPLAYCONFIG_SET_ADVANCED_COLOR_STATE si = {};
        si.header.type = (DISPLAYCONFIG_DEVICE_INFO_TYPE)
            DISPLAYCONFIG_DEVICE_INFO_TYPE_SET_ADVANCED_COLOR_STATE;
        si.header.size = sizeof(si);
        si.header.adapterId = p.targetInfo.adapterId;
        si.header.id = p.targetInfo.id;
        si.enableAdvancedColor = enable ? 1 : 0;
        if (DisplayConfigSetDeviceInfo(&si.header) == ERROR_SUCCESS) changed = true;
    }
    if (!apiOk) { kb_set_hdr(enable); return true; }
    debug::log("HDR: set_hdr_enabled done, changed=%d", changed);
    return changed;
}

} // namespace hdr_control
