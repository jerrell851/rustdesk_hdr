#pragma once
#include <windows.h>
namespace debug {
bool init_debug_log();        // open log in EXE directory
void log(const char* fmt, ...); // timestamped, thread-safe
bool is_enabled();
}
