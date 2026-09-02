#pragma once
#include <string>
namespace opal { int client_connect(const std::string &target,const std::string &password=""); int hosts_add(const std::string &name,const std::string &address,const std::string &mac=""); int hosts_list(); }
