#pragma once

namespace install_mode {
int install();
int uninstall();

// Query service status
bool is_service_installed();
bool is_service_running();

// Reconfigure service flags and restart.
// debug_enable: -1=no change, 0=remove --debug, 1=add --debug
// fk_enable:    -1=no change, 0=remove --force-keyboard, 1=add --force-keyboard
// Returns 0 on success.
int reconfigure_service(int debug_enable, int fk_enable);
}
