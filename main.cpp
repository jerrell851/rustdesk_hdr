// RustDesk HDR Monitor
#include <windows.h>
#include <cstdio>
#include <cstring>

#include "install_mode.h"
#include "service_mode.h"
#include "hdr_control.h"
#include "cm_log_watcher.h"
#include "debug.h"

static const wchar_t HELP_TITLE[] = L"RustDesk HDR 监控服务"; // 监控服务
static const wchar_t HELP_TEXT[] =
    L"RustDesk HDR 监控服务 v1.0\n\n"       // 监控服务
    L"用法：\n"                                    // 用法：
    L"  RustDeskHdrService.exe --install          安装并启动服务\n"   // 安装并启动服务
    L"  RustDeskHdrService.exe --uninstall        停止并删除服务\n"   // 停止并删除服务
    L"  RustDeskHdrService.exe --action status    查询 HDR 状态和连接数\n" // 查询 HDR 状态和连接数
    L"  RustDeskHdrService.exe --action disable   关闭 HDR\n"        // 关闭 HDR
    L"  RustDeskHdrService.exe --action enable    开启 HDR\n"        // 开启 HDR
    L"  RustDeskHdrService.exe --force-keyboard   强制使用键盘快捷键 Win+Alt+B\n" // 强制使用键盘快捷键
    L"  RustDeskHdrService.exe --debug            开启调试日志\n"      // 开启调试日志
    L"\n"
    L"双击 EXE 直接运行为 Windows 服务模式。\n" // 双击 EXE 直接运行为 Windows 服务模式
    L"命令行参数优先于服务模式。";           // 命令行参数优先于服务模式

// Show help in GUI mode (MessageBox) or console mode (printf)
void show_help_gui() {
    MessageBoxW(nullptr, HELP_TEXT, HELP_TITLE, MB_OK | MB_ICONINFORMATION);
}

static int action_mode(const char* action) {
    if (strcmp(action, "status") == 0) {
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
        printf("Disabling HDR...\n");
        debug::log("HDR action: disable");
        hdr_control::set_hdr_enabled(false);
        bool st = hdr_control::query_hdr_state();
        debug::log("HDR after disable: %s", st ? "ON" : "OFF");
        printf("HDR now: %s\n", st ? "ON" : "OFF");
        return st ? 1 : 0;
    }
    if (strcmp(action, "enable") == 0) {
        printf("Enabling HDR...\n");
        debug::log("HDR action: enable");
        hdr_control::set_hdr_enabled(true);
        bool st = hdr_control::query_hdr_state();
        debug::log("HDR after enable: %s", st ? "ON" : "OFF");
        printf("HDR now: %s\n", st ? "ON" : "OFF");
        return st ? 0 : 1;
    }
    printf("Unknown action: %s\n", action);
    return 1;
}

int main(int argc, char* argv[]) {
    // Pass 1: global flags
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--debug") == 0) { debug::init_debug_log(); }
        if (strcmp(argv[i], "--force-keyboard") == 0) { hdr_control::set_force_keyboard_mode(true); }
    }

    // Pass 2: action flags
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--install") == 0)   { return install_mode::install(); }
        if (strcmp(argv[i], "--uninstall") == 0) { return install_mode::uninstall(); }
        if (strcmp(argv[i], "--action") == 0 && i + 1 < argc) { return action_mode(argv[++i]); }
        if (strcmp(argv[i], "--debug") == 0 || strcmp(argv[i], "--force-keyboard") == 0) { /* pass 1 */ }
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("RustDesk HDR Monitor v1.0\n\n"
                   "  (no args)             Run as Windows Service\n"
                   "  --install             Install + start service\n"
                   "  --uninstall           Stop + remove service\n"
                   "  --action status       Show HDR + connection state\n"
                   "  --action disable      Turn HDR off\n"
                   "  --action enable       Turn HDR on\n"
                   "  --force-keyboard      Use Win+Alt+B instead of API\n"
                   "  --debug               Write debug log to EXE directory\n");
            return 0;
        }
    }

    return service_mode::run(false);
}
