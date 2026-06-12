#include "debug.h"
#include <cstdio>
#include <cstdarg>

namespace debug {

static FILE* g_f = nullptr;
static CRITICAL_SECTION g_cs;
static bool g_init = false;
static bool g_cs_ok = false;

// Eagerly init CS so debug::log() is always safe to call
struct CsInit { CsInit() { InitializeCriticalSection(&g_cs); g_cs_ok = true; } };
static CsInit g_cs_init;

bool init_debug_log() {
    if (g_init) return g_f != nullptr;
    g_init = true;

    // Write to EXE directory, fixed filename
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    wchar_t* s = wcsrchr(path, L'\\');
    if (s) *s = L'\0';
    wcscat_s(path, L"\\RustDeskHdrService_debug.log");

    errno_t e1 = _wfopen_s(&g_f, path, L"a");
    // Output to debugger so we can see what's happening
    wchar_t diag[512];
    swprintf_s(diag, L"DEBUG INIT: path=%s err=%d handle=%p", path, (int)e1, g_f);
    OutputDebugStringW(diag);

    if (!g_f) {
        GetTempPathW(MAX_PATH, path);
        wcscat_s(path, L"RustDeskHdrService_debug.log");
        _wfopen_s(&g_f, path, L"a");
    }

    if (g_f) {
        fprintf(g_f, "=== Debug started ===\n"); fflush(g_f);
    }
    return g_f != nullptr;
}

void log(const char* fmt, ...) {
    if (!g_f) return;
    SYSTEMTIME t; GetLocalTime(&t);
    char hdr[64];
    snprintf(hdr, sizeof(hdr), "[%02d:%02d:%02d.%03d] ",
             t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    char msg[2048];
    va_list a; va_start(a, fmt);
    int n = vsnprintf(msg, sizeof(msg), fmt, a);
    va_end(a);
    if (n < 0) return;
    if (g_cs_ok) EnterCriticalSection(&g_cs);
    fwrite(hdr, 1, strlen(hdr), g_f);
    fwrite(msg, 1, n, g_f);
    fwrite("\n", 1, 1, g_f); fflush(g_f);
    if (g_cs_ok) LeaveCriticalSection(&g_cs);
    OutputDebugStringA(hdr); OutputDebugStringA(msg); OutputDebugStringA("\n");
}

bool is_enabled() { return g_f != nullptr; }

} // namespace debug
