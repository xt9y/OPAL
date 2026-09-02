#pragma once
#include <string>
namespace opal {
int tunnel_host_setup(std::string &connection_code);
int tunnel_host_start();
bool tunnel_access(const std::string &control_token,const std::string &video_token);
bool tunnel_connection_code(const std::string &code,std::string *control_token=nullptr,std::string *video_token=nullptr);
int tunnel_clean_local();
int tunnel_host();
}
