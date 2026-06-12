#include "session_bridge.h"
#include "debug.h"
#include <wtsapi32.h>
#include <userenv.h>
#include <cstdio>

#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "userenv.lib")
#pragma comment(lib, "advapi32.lib")

namespace session_bridge {

bool launch_in_user_session(const wchar_t* exe_path, const wchar_t* action_arg,
                            DWORD timeout_ms) {
    uint32_t sid = WTSGetActiveConsoleSessionId();
    debug::log("SB: session=%u", sid);
    if (sid == 0xFFFFFFFF) { debug::log("SB: no active session"); return false; }

    HANDLE hToken = nullptr;
    if (!WTSQueryUserToken(sid, &hToken)) {
        debug::log("SB: WTSQueryUserToken err=%lu", GetLastError()); return false; }

    HANDLE hDup = nullptr;
    if (!DuplicateTokenEx(hToken, MAXIMUM_ALLOWED, nullptr, SecurityImpersonation, TokenPrimary, &hDup)) {
        debug::log("SB: DupToken err=%lu", GetLastError()); CloseHandle(hToken); return false; }
    CloseHandle(hToken);

    void* env = nullptr;
    if (!CreateEnvironmentBlock(&env, hDup, FALSE)) {
        debug::log("SB: CreateEnv err=%lu", GetLastError()); CloseHandle(hDup); return false; }

    wchar_t cmd[1024];
    swprintf_s(cmd, L"\"%s\" --action %s", exe_path, action_arg);
    debug::log("SB: cmd=%S", cmd);

    STARTUPINFOW si = { sizeof(si) };
    si.lpDesktop = const_cast<wchar_t*>(L"winsta0\\default");
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessAsUserW(hDup, nullptr, cmd, nullptr, nullptr, FALSE,
        CREATE_UNICODE_ENVIRONMENT | NORMAL_PRIORITY_CLASS, env, nullptr, &si, &pi)) {
        debug::log("SB: CreateProcess err=%lu", GetLastError());
        DestroyEnvironmentBlock(env); CloseHandle(hDup); return false; }
    DestroyEnvironmentBlock(env); CloseHandle(hDup);

    CloseHandle(pi.hThread);
    DWORD ec = 0;
    if (WaitForSingleObject(pi.hProcess, timeout_ms) == WAIT_OBJECT_0)
        GetExitCodeProcess(pi.hProcess, &ec);
    else TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hProcess);
    debug::log("SB: done ec=%lu", ec);
    return ec == 0;
}

} // namespace session_bridge
