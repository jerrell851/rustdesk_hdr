#include "install_mode.h"
#include "debug.h"
#include "hdr_control.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <vector>

namespace install_mode {

static const wchar_t SVC_NAME[] = L"RustDeskHdrMonitor";
static const wchar_t SVC_DISP[] = L"RustDesk HDR Monitor";

static void print_err(const char* ctx) {
    DWORD e = GetLastError();
    wchar_t* buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                   FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e, 0, (LPWSTR)&buf, 0, nullptr);
    debug::log("INSTALL: %s: [%lu] %S", ctx, e, buf ? buf : L"unknown");
    printf("%s: [%lu] %S\n", ctx, e, buf ? buf : L"unknown");
    LocalFree(buf);
}

// ── Build service command line from current process flags ──────
static void build_svc_cmd(wchar_t* out, size_t out_size, const wchar_t* exe_path) {
    wcscpy_s(out, out_size, exe_path);
    if (debug::is_enabled())
        wcscat_s(out, out_size, L" --debug");
    if (hdr_control::get_force_keyboard_mode())
        wcscat_s(out, out_size, L" --force-keyboard");
}

// ── Parse flags from a service binary path ─────────────────────
// Path format: "C:\path\to\exe" [--debug] [--force-keyboard]
// Uses token matching so --no-debug won't false-match --debug.
static void parse_svc_path(const wchar_t* path, bool& has_debug, bool& has_fk) {
    has_debug = false;
    has_fk    = false;
    // Skip past the quoted EXE path to reach argument tokens
    const wchar_t* p = path;
    if (*p == L'"') { p = wcschr(p + 1, L'"'); if (p) p++; }
    if (!p) return;
    while (*p) {
        while (*p == L' ') p++;
        if (!*p) break;
        const wchar_t* end = wcschr(p, L' ');
        if (!end) end = p + wcslen(p);
        size_t len = end - p;
        if (len == 7 && wcsncmp(p, L"--debug", 7) == 0)
            has_debug = true;
        else if (len == 17 && wcsncmp(p, L"--force-keyboard", 17) == 0)
            has_fk = true;
        p = end;
    }
}

// ── Build a clean svc path (EXE + explicitly listed flags) ─────
static void make_svc_path(wchar_t* out, size_t out_size,
                          const wchar_t* exe_path, bool debug, bool fk) {
    wcscpy_s(out, out_size, exe_path);
    if (debug) wcscat_s(out, out_size, L" --debug");
    if (fk)    wcscat_s(out, out_size, L" --force-keyboard");
}

// ── Stop service, wait up to 30s ───────────────────────────────
static bool stop_and_wait(SC_HANDLE svc) {
    SERVICE_STATUS st;
    if (!ControlService(svc, SERVICE_CONTROL_STOP, &st)) {
        DWORD e = GetLastError();
        if (e != ERROR_SERVICE_NOT_ACTIVE) return false;
        return true; // already stopped
    }
    for (int i = 0; i < 30; i++) {
        if (!QueryServiceStatus(svc, &st)) return false;
        if (st.dwCurrentState == SERVICE_STOPPED) return true;
        Sleep(1000);
    }
    return false; // timeout
}

// ── Open SCM + service with given access ───────────────────────
static SC_HANDLE open_svc(DWORD access) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return nullptr;
    SC_HANDLE svc = OpenServiceW(scm, SVC_NAME, access);
    CloseServiceHandle(scm);
    return svc;
}

bool is_service_installed() {
    SC_HANDLE svc = open_svc(SERVICE_QUERY_STATUS);
    if (!svc) return false;
    CloseServiceHandle(svc);
    return true;
}

bool is_service_running() {
    SC_HANDLE svc = open_svc(SERVICE_QUERY_STATUS);
    if (!svc) return false;
    SERVICE_STATUS st;
    bool running = QueryServiceStatus(svc, &st) && st.dwCurrentState == SERVICE_RUNNING;
    CloseServiceHandle(svc);
    return running;
}

// ── Generic reconfigure: set/clear --debug and/or --force-keyboard ──
// -1 = no change, 0 = remove flag, 1 = add flag
int reconfigure_service(int debug_enable, int fk_enable) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!scm) { print_err("OpenSCManager"); return 1; }
    SC_HANDLE svc = OpenServiceW(scm, SVC_NAME, SERVICE_ALL_ACCESS);
    if (!svc) { print_err("OpenService"); CloseServiceHandle(scm); return 1; }

    // 1. Read current binary path
    DWORD bytes_needed = 0;
    QueryServiceConfigW(svc, nullptr, 0, &bytes_needed);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes_needed == 0) {
        print_err("QueryServiceConfig size"); CloseServiceHandle(svc); CloseServiceHandle(scm); return 1;
    }
    std::vector<BYTE> buf(bytes_needed);
    auto* cfg = (QUERY_SERVICE_CONFIGW*)buf.data();
    if (!QueryServiceConfigW(svc, cfg, bytes_needed, &bytes_needed)) {
        print_err("QueryServiceConfig"); CloseServiceHandle(svc); CloseServiceHandle(scm); return 1;
    }

    // 2. Parse existing flags
    bool cur_dbg = false, cur_fk = false;
    parse_svc_path(cfg->lpBinaryPathName, cur_dbg, cur_fk);

    // 3. Apply changes
    if (debug_enable >= 0) cur_dbg = (debug_enable != 0);
    if (fk_enable    >= 0) cur_fk  = (fk_enable    != 0);

    // 4. Extract bare EXE path and build new cmd line
    wchar_t exe_path[MAX_PATH];
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    wchar_t new_path[MAX_PATH + 64];
    make_svc_path(new_path, _countof(new_path), exe_path, cur_dbg, cur_fk);

    debug::log("INSTALL: reconfigure dbg=%d fk=%d -> %S", cur_dbg, cur_fk, new_path);
    printf("Reconfiguring service: debug=%s keyboard=%s\n",
           cur_dbg ? "ON" : "OFF", cur_fk ? "ON" : "OFF");

    // 5. Update service config
    if (!ChangeServiceConfigW(svc, SERVICE_NO_CHANGE, SERVICE_NO_CHANGE, SERVICE_NO_CHANGE,
                              new_path, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr)) {
        print_err("ChangeServiceConfig");
        CloseServiceHandle(svc); CloseServiceHandle(scm);
        return 1;
    }

    // 6. Stop + restart
    printf("Restarting service...\n");
    if (!stop_and_wait(svc)) {
        printf("WARN: service may not have stopped cleanly\n");
    }
    if (!StartServiceW(svc, 0, nullptr)) {
        DWORD e = GetLastError();
        if (e != ERROR_SERVICE_ALREADY_RUNNING) {
            print_err("StartService");
            CloseServiceHandle(svc); CloseServiceHandle(scm);
            return 1;
        }
    }
    printf("Service restarted.\n");
    CloseServiceHandle(svc); CloseServiceHandle(scm);
    return 0;
}

// ── Install ────────────────────────────────────────────────────
int install() {
    wchar_t path[MAX_PATH];
    if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0) {
        debug::log("INSTALL: cannot get exe path"); printf("Cannot get exe path\n"); return 1;
    }
    debug::log("INSTALL: exe=%S", path);

    wchar_t svc_path[MAX_PATH + 64];
    build_svc_cmd(svc_path, _countof(svc_path), path);

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!scm) { print_err("OpenSCManager"); return 1; }

    SC_HANDLE svc = CreateServiceW(scm, SVC_NAME, SVC_DISP, SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
        svc_path, nullptr, nullptr, nullptr, nullptr, nullptr);
    if (!svc) {
        if (GetLastError() == ERROR_SERVICE_EXISTS) {
            debug::log("INSTALL: service exists, updating");
            printf("Service exists, updating path...\n");
            svc = OpenServiceW(scm, SVC_NAME, SERVICE_ALL_ACCESS);
            if (!svc) { print_err("OpenService"); CloseServiceHandle(scm); return 1; }
            ChangeServiceConfigW(svc, SERVICE_NO_CHANGE, SERVICE_NO_CHANGE, SERVICE_NO_CHANGE,
                                 svc_path, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
        } else { print_err("CreateService"); CloseServiceHandle(scm); return 1; }
    }

    SERVICE_DESCRIPTIONW d = {};
    const wchar_t desc[] = L"Monitors RustDesk connections and manages HDR display settings.";
    d.lpDescription = const_cast<wchar_t*>(desc);
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &d);

    SERVICE_FAILURE_ACTIONSW fa = {};
    SC_ACTION acts[3] = { {SC_ACTION_RESTART, 10000}, {SC_ACTION_RESTART, 30000}, {SC_ACTION_RESTART, 60000} };
    fa.dwResetPeriod = 86400; fa.cActions = 3; fa.lpsaActions = acts;
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &fa);

    if (!StartServiceW(svc, 0, nullptr) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
        print_err("StartService");
    } else {
        debug::log("INSTALL: started OK");
        printf("Service '%S' installed + started.\n", SVC_NAME);
    }
    CloseServiceHandle(svc); CloseServiceHandle(scm);
    return 0;
}

int uninstall() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!scm) { print_err("OpenSCManager"); return 1; }
    SC_HANDLE svc = OpenServiceW(scm, SVC_NAME, SERVICE_ALL_ACCESS);
    if (!svc) { print_err("OpenService"); CloseServiceHandle(scm); return 1; }

    SERVICE_STATUS st;
    ControlService(svc, SERVICE_CONTROL_STOP, &st);
    debug::log("INSTALL: stopping..."); printf("Stopping...\n"); Sleep(2000);

    if (!DeleteService(svc)) { print_err("DeleteService"); }
    else { debug::log("INSTALL: removed OK"); printf("Service '%S' removed.\n", SVC_NAME); }

    CloseServiceHandle(svc); CloseServiceHandle(scm);
    return 0;
}

} // namespace install_mode
