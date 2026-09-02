#pragma once
#include <algorithm>
#include <string>
#include <sys/types.h>
namespace opal {
struct TunnelAccessHandle {
    pid_t control_pid=-1;
    pid_t video_pid=-1;
    int control_port=0;
    int video_port=0;
};
bool tunnel_access_start(TunnelAccessHandle &handle,const std::string &control_token,const std::string &video_token,int timeout_ms=30000);
bool tunnel_access_healthy(TunnelAccessHandle &handle);
void tunnel_access_stop(TunnelAccessHandle &handle);
}
