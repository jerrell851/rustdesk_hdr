#include "cm_log_watcher.h"
#include "debug.h"
#include <cstdio>
#include <vector>
#include <algorithm>

CmLogWatcher::~CmLogWatcher() { stop(); }

bool CmLogWatcher::start(const wchar_t* dir, const wchar_t* ck,
                         const wchar_t* dk, Callback cb) {
    dir_ = dir; conn_kw_ = ck; disc_kw_ = dk; cb_ = std::move(cb);
    DWORD attr = GetFileAttributesW(dir);
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY))
        return false;
    stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    thread_ = CreateThread(nullptr, 0, thread_proc, this, 0, nullptr);
    return thread_ != nullptr;
}

void CmLogWatcher::stop() {
    if (stop_) SetEvent(stop_);
    if (thread_) { WaitForSingleObject(thread_, 5000); CloseHandle(thread_); thread_ = nullptr; }
    if (stop_) { CloseHandle(stop_); stop_ = nullptr; }
}

DWORD WINAPI CmLogWatcher::thread_proc(LPVOID p) {
    static_cast<CmLogWatcher*>(p)->run(); return 0;
}

// ── Find the CURRENT log file ───────────────────────────────
// RustDesk writes to CURRENT.log; rotated logs are renamed to timestamped files.
// We only care about CURRENT.log — the most recent events are always there.
static bool find_current_log(const std::wstring& dir, std::wstring& out) {
    // Look for files containing "CURRENT" (case-insensitive)
    std::wstring pattern = dir + L"\\*CURRENT*";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    out = dir + L"\\" + fd.cFileName;
    FindClose(h);
    return true;
}

// ── Read tail of a file and find the last connect/disconnect event ──
// Returns: 1 = connected, 0 = disconnected, -1 = unknown
static int tail_scan_state(const std::wstring& path,
                           const std::string& conn_kw,
                           const std::string& disc_kw) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return -1;

    LARGE_INTEGER sz; GetFileSizeEx(h, &sz);
    int64_t size = sz.QuadPart;
    // Read last 8KB (enough to find the most recent event)
    int64_t read_start = (size > 8192) ? size - 8192 : 0;
    DWORD to_read = (DWORD)(size - read_start);

    LARGE_INTEGER li; li.QuadPart = read_start;
    SetFilePointerEx(h, li, nullptr, FILE_BEGIN);

    std::vector<char> buf(to_read + 1);
    DWORD rd = 0;
    ReadFile(h, buf.data(), to_read, &rd, nullptr);
    CloseHandle(h);
    if (rd == 0) return -1;
    buf[rd] = '\0';

    // Scan from end to find last connect/disconnect keyword
    // Keywords appear in log lines like:
    //   "... Got new connection"
    //   "... cm ipc connection closed"
    int state = -1;
    const char* p = buf.data();
    const char* end = p + rd;
    while (p < end) {
        const char* nl = (const char*)memchr(p, '\n', end - p);
        if (!nl) nl = end;
        std::string line(p, nl - p);
        if (!line.empty() && line.back() == '\r') line.pop_back();

        if (line.find(conn_kw) != std::string::npos) state = 1;
        else if (line.find(disc_kw) != std::string::npos) state = 0;

        p = nl + 1;
    }
    return state;
}

// ── Helper: wide to narrow (keywords are ASCII-only) ───────
static std::string w2n(const std::wstring& ws) {
    std::string s; s.reserve(ws.size());
    for (wchar_t c : ws) s += (char)c;
    return s;
}

// ── Main loop ───────────────────────────────────────────────
void CmLogWatcher::run() {
    std::wstring current_path;
    std::string ck = w2n(conn_kw_);
    std::string dk = w2n(disc_kw_);

    debug::log("CmLogWatcher: watching %S", dir_.c_str());

    // 1. Check initial state from CURRENT.log tail
    if (find_current_log(dir_, current_path)) {
        int st = tail_scan_state(current_path, ck, dk);
        debug::log("CmLogWatcher: init tail scan state=%d", st);
        if (st == 1) {
            active_ = true;
            if (cb_) cb_(true);
        }
    }

    // 2. Track CURRENT.log size for incremental reads
    uint64_t last_size = 0;
    if (!current_path.empty()) {
        WIN32_FILE_ATTRIBUTE_DATA ad;
        if (GetFileAttributesExW(current_path.c_str(), GetFileExInfoStandard, &ad)) {
            last_size = ((uint64_t)ad.nFileSizeHigh << 32) | ad.nFileSizeLow;
        }
    }

    // 3. Set up change notification
    HANDLE hChange = FindFirstChangeNotificationW(
        dir_.c_str(), FALSE,
        FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE |
        FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_CREATION);
    if (hChange == INVALID_HANDLE_VALUE || hChange == nullptr) {
        debug::log("CmLogWatcher: FindFirstChangeNotification failed, poll mode");
        hChange = nullptr;
    }

    // 4. Event loop: notification-driven + 5s safety poll
    while (true) {
        DWORD timeout = 5000;
        HANDLE h[2] = { hChange ? hChange : stop_, stop_ };
        DWORD count = hChange ? 2 : 1;
        DWORD r = WaitForMultipleObjects(count, h, FALSE, timeout);

        if (r == WAIT_OBJECT_0 + 1 || (count == 1 && r == WAIT_OBJECT_0)) break; // stop
        // r == WAIT_OBJECT_0: notification fired
        // r == WAIT_TIMEOUT: safety poll

        debug::log("CmLogWatcher: wake (r=%lu)", r);

        // Check if CURRENT.log was rotated (new file appeared)
        std::wstring new_current;
        if (find_current_log(dir_, new_current) && new_current != current_path) {
            debug::log("CmLogWatcher: log rotated to %S", new_current.c_str());
            current_path = new_current;
            last_size = 0; // new file, start from beginning
        }

        // Read new content from CURRENT.log
        if (!current_path.empty()) {
            HANDLE fh = CreateFileW(current_path.c_str(), GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (fh != INVALID_HANDLE_VALUE) {
                LARGE_INTEGER sz; GetFileSizeEx(fh, &sz);
                uint64_t cur_size = (uint64_t)sz.QuadPart;

                if (cur_size < last_size) last_size = 0; // truncated
                if (cur_size > last_size) {
                    LARGE_INTEGER off; off.QuadPart = (LONGLONG)last_size;
                    SetFilePointerEx(fh, off, nullptr, FILE_BEGIN);
                    DWORD to_read = (DWORD)(cur_size - last_size);
                    if (to_read > 65536) to_read = 65536;
                    std::vector<char> buf(to_read + 1);
                    DWORD rd = 0;
                    ReadFile(fh, buf.data(), to_read, &rd, nullptr);
                    buf[rd] = '\0';
                    last_size += rd;

                    // Parse lines and look for connect/disconnect
                    const char* p = buf.data();
                    const char* end = p + rd;
                    while (p < end) {
                        const char* nl = (const char*)memchr(p, '\n', end - p);
                        if (!nl) nl = end;
                        std::string line(p, nl - p);
                        if (!line.empty() && line.back() == '\r') line.pop_back();

                        if (line.find(ck) != std::string::npos) {
                            debug::log("CmLogWatcher: LIVE CONNECT");
                            if (!active_) { active_ = true; if (cb_) cb_(true); }
                        }
                        if (line.find(dk) != std::string::npos) {
                            debug::log("CmLogWatcher: LIVE DISCONNECT");
                            if (active_) { active_ = false; if (cb_) cb_(false); }
                        }
                        p = nl + 1;
                    }
                }
                CloseHandle(fh);
            }
        }

        if (hChange && r == WAIT_OBJECT_0)
            FindNextChangeNotification(hChange);
    }

    if (hChange) FindCloseChangeNotification(hChange);
}

// ── One-shot status scan ────────────────────────────────────
int CmLogWatcher::scan_connection_count(const wchar_t* dir, const wchar_t* ck,
                                        const wchar_t* dk) {
    std::wstring wdir(dir);
    std::string conn_kw = w2n(ck);
    std::string disc_kw = w2n(dk);

    std::wstring path;
    if (!find_current_log(wdir, path)) return 0;

    int st = tail_scan_state(path, conn_kw, disc_kw);
    return (st == 1) ? 1 : 0;
}
