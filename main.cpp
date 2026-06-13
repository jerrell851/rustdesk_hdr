// RustDesk HDR Monitor
#include <windows.h>
#include <cstdio>
#include <cstring>

#include "install_mode.h"
#include "service_mode.h"
#include "hdr_control.h"
#include "cm_log_watcher.h"
#include "debug.h"

// ── Service status check for --action ──────────────────────────
static void check_service_status() {
    if (install_mode::is_service_installed()) {
        if (!install_mode::is_service_running()) {
            printf("\n  NOTE: service installed but not running.\n"
                   "        HDR will NOT auto-switch on RustDesk connect.\n\n");
        }
    } else {
        printf("\n  NOTE: service not installed. Run --install first.\n\n");
    }
}

static int action_mode(const char* action) {
    if (strcmp(action, "status") == 0) {
        check_service_status();
        bool on = hdr_control::query_hdr_state();
        debug::log("HDR query: %s", on ? "ON" : "OFF");
        printf("HDR state: %s\n", on ? "ON" : "OFF");

        wchar_t appdata[MAX_PATH];
        GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
        wchar_t cm_dir[MAX_PATH];
        swprintf_s(cm_dir, L"%s\\RustDesk\\log\\cm", appdata);
        CmLogWatcher w;
        int cnt = w.scan_connection_count(cm_dir, L"Got new connection",
                                          L"cm ipc connection closed");
        debug::log("Connection scan: %d active", cnt);
        printf("RustDesk connections: %d\n", cnt);
        return 0;
    }
    if (strcmp(action, "disable") == 0) {
        check_service_status();
        printf("Mode: %s\n", hdr_control::get_force_keyboard_mode()
               ? "keyboard (Win+Alt+B)" : "DisplayConfig API");
        printf("Disabling HDR...\n");
        debug::log("HDR action: disable force_kb=%d", hdr_control::get_force_keyboard_mode());
        hdr_control::set_hdr_enabled(false);
        bool st = hdr_control::query_hdr_state();
        debug::log("HDR after disable: %s", st ? "ON" : "OFF");
        printf("HDR now: %s\n", st ? "ON" : "OFF");
        return st ? 1 : 0;
    }
    if (strcmp(action, "enable") == 0) {
        check_service_status();
        printf("Mode: %s\n", hdr_control::get_force_keyboard_mode()
               ? "keyboard (Win+Alt+B)" : "DisplayConfig API");
        printf("Enabling HDR...\n");
        debug::log("HDR action: enable force_kb=%d", hdr_control::get_force_keyboard_mode());
        hdr_control::set_hdr_enabled(true);
        bool st = hdr_control::query_hdr_state();
        debug::log("HDR after enable: %s", st ? "ON" : "OFF");
        printf("HDR now: %s\n", st ? "ON" : "OFF");
        return st ? 0 : 1;
    }
    printf("Unknown action: %s\n", action);
    return 1;
}

// ── Reconfigure helper: prompt --install if no service ─────────
static int reconf_or_prompt(int dbg, int fk) {
    if (!install_mode::is_service_installed()) {
        printf("Service not installed. Run --install first.\n");
        return 0;
    }
    return install_mode::reconfigure_service(dbg, fk);
}

int main(int argc, char* argv[]) {
    // ── Pass 1: global flags (runtime effect only) ─────────────
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--debug") == 0)            { debug::init_debug_log(); }
        if (strcmp(argv[i], "--force-keyboard") == 0)   { hdr_control::set_force_keyboard_mode(true); }
        if (strcmp(argv[i], "--no-force-keyboard") == 0){ hdr_control::set_force_keyboard_mode(false); }
    }

    // ── Pass 2: act first, collect reconfigure intents ─────────
    int action_result = -1;
    int reconf_dbg = -1, reconf_fk = -1;  // -1=no change, 0=off, 1=on

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--install") == 0)   { return install_mode::install(); }
        if (strcmp(argv[i], "--uninstall") == 0) { return install_mode::uninstall(); }
        if (strcmp(argv[i], "--action") == 0 && i + 1 < argc) {
            action_result = action_mode(argv[++i]);
        }
        else if (strcmp(argv[i], "--debug") == 0)            { reconf_dbg = 1; }
        else if (strcmp(argv[i], "--no-debug") == 0)         { reconf_dbg = 0; }
        else if (strcmp(argv[i], "--force-keyboard") == 0)   { reconf_fk  = 1; }
        else if (strcmp(argv[i], "--no-force-keyboard") == 0){ reconf_fk  = 0; }
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("RustDesk HDR Monitor v1.0\n\n"
                   "  --install / --uninstall     Service management\n"
                   "  --action status/disable/enable   HDR control\n"
                   "  --debug / --no-debug             Debug logging on/off\n"
                   "  --force-keyboard / --no-force-keyboard   HDR toggle method\n");
            return 0;
        }
    }

    // ── Step 3: apply service reconfiguration ──────────────────
    if (reconf_dbg >= 0) reconf_or_prompt(reconf_dbg, -1);
    if (reconf_fk  >= 0) reconf_or_prompt(-1, reconf_fk);

    if (action_result >= 0) return action_result;

    // No explicit action: run as service (SCM) or show help
    return service_mode::run(false);
}
