#pragma once
#include <windows.h>

// HDR control via DisplayConfig API, with SendInput keyboard fallback.
// Primary:  DisplayConfigSetDeviceInfo (Win10 2004+)
// Fallback: SendInput Win+Alt+B (pre-2004 Win10)

namespace hdr_control {

bool query_hdr_state();
bool set_hdr_enabled(bool enable);
void toggle_via_keyboard();
void set_force_keyboard_mode(bool force);
bool get_force_keyboard_mode();

} // namespace hdr_control
