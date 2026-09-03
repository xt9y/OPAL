#pragma once
#include <opal/media.hpp>
#include <string>
namespace opal {
int client_connect(const std::string &target,const std::string &password="");
int client_connect(const std::string &target,const std::string &password,const StreamOptions &stream);
int hosts_add(const std::string &name,const std::string &address,const std::string &mac="");
int hosts_list();
}
