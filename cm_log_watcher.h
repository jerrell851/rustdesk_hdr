#pragma once
#include <windows.h>
#include <cstdint>
#include <functional>
#include <string>
#include <map>

// Watches RustDesk cm log directory for connection state changes.
// Only reads CURRENT.log tail (~8KB) for initial state, then
// monitors file size changes for live connect/disconnect events.
class CmLogWatcher {
public:
    using Callback = std::function<void(bool connected)>;

    CmLogWatcher() = default;
    ~CmLogWatcher();

    bool start(const wchar_t* cm_log_dir, const wchar_t* conn_kw,
               const wchar_t* disc_kw, Callback cb);
    void stop();
    int scan_connection_count(const wchar_t* dir, const wchar_t* conn_kw,
                              const wchar_t* disc_kw);

private:
    static DWORD WINAPI thread_proc(LPVOID p);
    void run();

    std::wstring dir_, conn_kw_, disc_kw_;
    Callback cb_;
    HANDLE stop_ = nullptr, thread_ = nullptr;
    bool active_ = false;
};
