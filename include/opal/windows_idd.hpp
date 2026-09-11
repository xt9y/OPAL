#pragma once

#include <cstdint>

namespace opal {

struct WindowsIddStatus {
    bool device_present = false;
    bool adapter_ready = false;
    bool monitor_active = false;
    int width = 0;
    int height = 0;
    int refresh_hz = 0;
};

WindowsIddStatus windows_idd_status();
bool windows_physical_display_present();
bool windows_physical_display_usable();

}
