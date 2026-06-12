#include "install_mode.h"
#include "debug.h"
#include <windows.h>
#include <cstdio>

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

int install() {
    wchar_t path[MAX_PATH];
    if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0) {
        debug::log("INSTALL: cannot get exe path"); printf("Cannot get exe path\n"); return 1;
    }
    debug::log("INSTALL: exe=%S", path);

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!scm) { print_err("OpenSCManager"); return 1; }

    SC_HANDLE svc = CreateServiceW(scm, SVC_NAME, SVC_DISP, SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
        path, nullptr, nullptr, nullptr, nullptr, nullptr);
    if (!svc) {
        if (GetLastError() == ERROR_SERVICE_EXISTS) {
            debug::log("INSTALL: service exists, updating");
            printf("Service exists, updating path...\n");
            svc = OpenServiceW(scm, SVC_NAME, SERVICE_ALL_ACCESS);
            if (!svc) { print_err("OpenService"); CloseServiceHandle(scm); return 1; }
            ChangeServiceConfigW(svc, SERVICE_NO_CHANGE, SERVICE_NO_CHANGE, SERVICE_NO_CHANGE,
                                 path, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
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
