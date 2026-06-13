#include "service_mode.h"
#include "cm_log_watcher.h"
#include "hdr_control.h"
#include "session_bridge.h"
#include "debug.h"
#include <windows.h>
#include <cstdio>

namespace service_mode {

static constexpr DWORD CONFIRM_CONNECT_MS    = 500;
static constexpr DWORD CONFIRM_DISCONNECT_MS = 2000;
static constexpr DWORD HDR_COOLDOWN_MS       = 5000;

static SERVICE_STATUS_HANDLE g_sh = nullptr;
static HANDLE g_stop = nullptr;
static bool g_hdr_off_by_us = false;
static wchar_t g_path[MAX_PATH] = {}, g_log_dir[MAX_PATH] = {};
static FILE* g_log = nullptr;
static CmLogWatcher g_watcher;
static CRITICAL_SECTION g_sm_cs;
static CRITICAL_SECTION g_log_cs;
static HANDLE g_session_evt = nullptr;  // signaled on session change (watcher retry loop)

enum St { Idle, WaitConnect, Connected, WaitDisconnect };
static St g_st = Idle;
static DWORD g_last_toggle = 0;
static DWORD g_conn_seen_at = 0;
static DWORD g_disc_seen_at = 0;

// ── Logging ─────────────────────────────────────────────────
static void log_init() {
    InitializeCriticalSection(&g_sm_cs);
    InitializeCriticalSection(&g_log_cs);
    CreateDirectoryW(g_log_dir, nullptr);
    wchar_t fp[MAX_PATH]; swprintf_s(fp, L"%s\\service.log", g_log_dir);
    _wfopen_s(&g_log, fp, L"a");
}

static void log_w(const char* msg) {
    SYSTEMTIME t; GetLocalTime(&t);
    char b[512];
    int n = snprintf(b, sizeof(b), "[%02d:%02d:%02d] %s\n", t.wHour, t.wMinute, t.wSecond, msg);
    OutputDebugStringA(b);
    EnterCriticalSection(&g_log_cs);
    if (g_log) { fwrite(b, 1, n, g_log); fflush(g_log); }
    LeaveCriticalSection(&g_log_cs);
}

// ── HDR action ──────────────────────────────────────────────
static void do_hdr(bool disable) {
    debug::log("SM: do_hdr(%s) mode=%s", disable ? "OFF" : "ON", g_sh ? "service" : "interactive");
    bool ok;
    if (!g_sh) {
        ok = hdr_control::set_hdr_enabled(!disable);
    } else {
        const wchar_t* a = disable ? L"disable" : L"enable";
        ok = session_bridge::launch_in_user_session(g_path, a, 10000);
    }
    char m[256];
    snprintf(m, sizeof(m), "HDR %s -> %s", disable ? "OFF" : "ON", ok ? "OK" : "FAIL");
    debug::log("SM: %s", m); log_w(m);
    if (ok) g_hdr_off_by_us = disable;
}

// ── State machine (SM_CRITICAL_SECTION protected) ───────────
static void on_connect() {
    EnterCriticalSection(&g_sm_cs);
    DWORD now = GetTickCount();
    switch (g_st) {
    case Idle:
        g_st = WaitConnect; g_conn_seen_at = now;
        debug::log("SM: Idle -> WaitConnect"); log_w("St: Idle -> WaitConnect"); break;
    case WaitDisconnect:
        g_st = Connected;
        debug::log("SM: WaitDisconnect -> Connected (reconnect)"); log_w("St: WaitDisconnect -> Connected (reconnect)"); break;
    case Connected: break;
    case WaitConnect: g_conn_seen_at = now; break;
    }
    LeaveCriticalSection(&g_sm_cs);
}

static void on_disconnect() {
    EnterCriticalSection(&g_sm_cs);
    DWORD now = GetTickCount();
    switch (g_st) {
    case Connected:
        g_st = WaitDisconnect; g_disc_seen_at = now;
        debug::log("SM: Connected -> WaitDisconnect"); log_w("St: Connected -> WaitDisconnect"); break;
    case WaitConnect:
        g_st = Idle;
        debug::log("SM: WaitConnect -> Idle (false alarm)"); log_w("St: WaitConnect -> Idle (false alarm)"); break;
    case Idle: case WaitDisconnect: break;
    }
    LeaveCriticalSection(&g_sm_cs);
}

static void timer_tick() {
    EnterCriticalSection(&g_sm_cs);
    DWORD now = GetTickCount();
    switch (g_st) {
    case WaitConnect:
        if (now - g_conn_seen_at >= CONFIRM_CONNECT_MS) {
            g_st = Connected;
            debug::log("SM: WaitConnect -> Connected (confirmed)"); log_w("St: WaitConnect -> Connected (confirmed)");
            if (now - g_last_toggle >= HDR_COOLDOWN_MS) {
                g_last_toggle = now;
                LeaveCriticalSection(&g_sm_cs);
                do_hdr(true);
                return;
            } else {
                char b[64]; snprintf(b, sizeof(b), "HDR cooldown skip %lums", HDR_COOLDOWN_MS - (now - g_last_toggle));
                debug::log("%s", b); log_w(b);
            }
        }
        break;
    case WaitDisconnect:
        if (now - g_disc_seen_at >= CONFIRM_DISCONNECT_MS) {
            g_st = Idle;
            debug::log("SM: WaitDisconnect -> Idle (confirmed)"); log_w("St: WaitDisconnect -> Idle (confirmed)");
            bool restore = g_hdr_off_by_us && (now - g_last_toggle >= HDR_COOLDOWN_MS);
            if (restore) g_last_toggle = now;
            LeaveCriticalSection(&g_sm_cs);
            if (restore) do_hdr(false);
            return;
        }
        break;
    default: break;
    }
    LeaveCriticalSection(&g_sm_cs);
}

// ── Timer thread ────────────────────────────────────────────
static DWORD WINAPI timer_thread(LPVOID) {
    while (WaitForSingleObject(g_stop, 250) == WAIT_TIMEOUT) timer_tick();
    return 0;
}

// ── Find RustDesk cm log dir ────────────────────────────────
static bool find_cm_log_dir(wchar_t* out, size_t out_size) {
    wchar_t ad[MAX_PATH];
    if (GetEnvironmentVariableW(L"APPDATA", ad, MAX_PATH) > 0) {
        swprintf_s(out, out_size, L"%s\\RustDesk\\log\\cm", ad);
        if (GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES) return true;
    }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(L"C:\\Users\\*", &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        if (_wcsicmp(fd.cFileName, L"Public") == 0 || _wcsicmp(fd.cFileName, L"Default") == 0
            || _wcsicmp(fd.cFileName, L"All Users") == 0) continue;
        swprintf_s(out, out_size, L"C:\\Users\\%s\\AppData\\Roaming\\RustDesk\\log\\cm", fd.cFileName);
        if (GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES) {
            FindClose(h);
            return true;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    wchar_t up[MAX_PATH];
    if (GetEnvironmentVariableW(L"USERPROFILE", up, MAX_PATH) > 0) {
        swprintf_s(out, out_size, L"%s\\AppData\\Roaming\\RustDesk\\log\\cm", up);
        return true;
    }
    return false;
}

// ── Initialize watcher ──────────────────────────────────────
static bool start_watcher() {
    wchar_t cm_dir[MAX_PATH];
    if (!find_cm_log_dir(cm_dir, MAX_PATH)) {
        log_w("FATAL: Cannot find RustDesk cm log directory");
        return false;
    }
    debug::log("start_watcher: cm_dir=%S", cm_dir);
    bool ok = g_watcher.start(cm_dir, L"Got new connection", L"cm ipc connection closed",
        [](bool c) { if (c) on_connect(); else on_disconnect(); });
    if (ok) { char b[512]; snprintf(b, sizeof(b), "Watching: %S", cm_dir); log_w(b); }
    else    { char b[512]; snprintf(b, sizeof(b), "WARN: cm dir not found: %S", cm_dir); log_w(b); }
    return ok;
}

// ── Service handler ─────────────────────────────────────────
static DWORD WINAPI handler(DWORD ctrl, DWORD evt_type, LPVOID, LPVOID) {
    if (ctrl == SERVICE_CONTROL_SESSIONCHANGE) {
        // User session login/logout — wake watcher retry loop
        debug::log("SM: session change evt=%lu", evt_type);
        if (g_session_evt) SetEvent(g_session_evt);
        return NO_ERROR;
    }
    if (ctrl == SERVICE_CONTROL_STOP || ctrl == SERVICE_CONTROL_SHUTDOWN) {
        debug::log("SM: service stopping");
        SERVICE_STATUS st = { SERVICE_WIN32_OWN_PROCESS, SERVICE_STOP_PENDING, 0, 1, 3000, 0, 0 };
        SetServiceStatus(g_sh, &st);
        // Stop timer BEFORE blocking HDR restore
        SetEvent(g_stop);
        if (g_session_evt) { SetEvent(g_session_evt); }  // unblock retry loop
        bool restore;
        EnterCriticalSection(&g_sm_cs);
        restore = g_hdr_off_by_us;
        LeaveCriticalSection(&g_sm_cs);
        if (restore) { debug::log("SM: restoring HDR on stop"); log_w("Restoring HDR..."); do_hdr(false); }
        g_watcher.stop();
        st.dwCurrentState = SERVICE_STOPPED; SetServiceStatus(g_sh, &st);
    }
    return NO_ERROR;
}

static VOID WINAPI svc_main(DWORD, LPWSTR*) {
    g_sh = RegisterServiceCtrlHandlerExW(L"RustDeskHdrMonitor", handler, nullptr);
    if (!g_sh) return;
    SERVICE_STATUS st = { SERVICE_WIN32_OWN_PROCESS, SERVICE_RUNNING,
        SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_SESSIONCHANGE,
        1, 0, 0, 0 };
    SetServiceStatus(g_sh, &st);
    debug::log("SM: service_main starting"); log_w("Service started");

    bool init_hdr = hdr_control::query_hdr_state();
    char b[64]; snprintf(b, sizeof(b), "Init HDR: %s", init_hdr ? "ON" : "OFF");
    debug::log("SM: %s", b); log_w(b);

    GetModuleFileNameW(nullptr, g_path, MAX_PATH);
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_session_evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);  // auto-reset

    // Retry loop: if RustDesk hasn't been run yet, cm log dir won't exist.
    // Probe RustDesk IPC pipe — if the pipe is alive, RustDesk is running,
    // which means cm log directory has already been created.
    // We don't actually communicate, just check existence with CreateFile.
    // Once found, this loop is never entered again.
    while (!start_watcher()) {
        log_w("WARN: cm log dir not found, waiting for RustDesk...");
        HANDLE waits[2] = { g_stop, g_session_evt };
        // Wait for stop, session change, or 30s poll interval
        DWORD r = WaitForMultipleObjects(2, waits, FALSE, 30000);
        if (r == WAIT_OBJECT_0) {  // g_stop — service is shutting down
            CloseHandle(g_session_evt); g_session_evt = nullptr;
            CloseHandle(g_stop); g_stop = nullptr;
            st.dwCurrentState = SERVICE_STOPPED; SetServiceStatus(g_sh, &st);
            return;
        }
        // Probe RustDesk IPC pipe — if pipe exists, cm log dir is ready.
        // Only ERROR_FILE_NOT_FOUND definitively means "no pipe".
        // Any other result (success, busy, access-denied) → pipe is there.
        HANDLE hPipe = CreateFileW(L"\\\\.\\pipe\\RustDesk\\query",
            GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, 0, nullptr);
        if (hPipe != INVALID_HANDLE_VALUE) {
            CloseHandle(hPipe);
            log_w("RustDesk pipe detected, retrying watcher...");
        } else if (GetLastError() != ERROR_FILE_NOT_FOUND) {
            char tmp[80]; snprintf(tmp, sizeof(tmp),
                "RustDesk pipe detected (err=%lu), retrying watcher...", GetLastError());
            log_w(tmp);
        }
    }
    CloseHandle(g_session_evt); g_session_evt = nullptr;

    HANDLE hTimer = CreateThread(nullptr, 0, timer_thread, nullptr, 0, nullptr);
    WaitForSingleObject(g_stop, INFINITE);
    g_watcher.stop();
    WaitForSingleObject(hTimer, 5000); CloseHandle(hTimer);
    CloseHandle(g_stop);
    debug::log("SM: service_main ended"); log_w("Service stopped");
    st.dwCurrentState = SERVICE_STOPPED; SetServiceStatus(g_sh, &st);
}

int run(bool) {
    wchar_t pd[MAX_PATH]; GetTempPathW(MAX_PATH, pd);
    size_t len = wcslen(pd);
    if (len > 0 && pd[len-1] == L'\\') pd[len-1] = L'\0';
    swprintf_s(g_log_dir, L"%s\\RustDeskHdrMonitor", pd);
    log_init();
    debug::log("Starting service_mode::run"); log_w("Starting...");

    SERVICE_TABLE_ENTRYW t[] = {
        { const_cast<wchar_t*>(L"RustDeskHdrMonitor"), svc_main }, { nullptr, nullptr }
    };
    if (!StartServiceCtrlDispatcherW(t)) {
        if (GetLastError() == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
            MessageBoxW(nullptr,
                L"RustDesk HDR 监控服务 v1.0\n\n"
                L"服务管理：\n"
                L"  --install              安装并启动服务\n"
                L"  --uninstall            停止并删除服务\n"
                L"\n手动控制：\n"
                L"  --action status        查询 HDR 状态和连接数\n"
                L"  --action disable       关闭 HDR\n"
                L"  --action enable        开启 HDR\n"
                L"\n调试日志：\n"
                L"  --debug                开启调试日志\n"
                L"  --no-debug             关闭调试日志\n"
                L"\nHDR 切换方式：\n"
                L"  --force-keyboard       键盘模式 (Win+Alt+B)\n"
                L"  --no-force-keyboard    API 模式 (默认)\n"
                L"\n以上参数可组合使用，需管理员权限。",
                L"RustDesk HDR 监控服务", MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        return 1;
    }
    return 0;
}

} // namespace service_mode
