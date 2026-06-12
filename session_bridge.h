#pragma once
#include <windows.h>
#include <memory>

// Launch same EXE in user session with --action <action_arg>.
namespace session_bridge {

bool launch_in_user_session(const wchar_t* exe_path, const wchar_t* action_arg,
                            DWORD timeout_ms = 10000);

}
