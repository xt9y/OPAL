#pragma once
#include <filesystem>
#include <map>
#include <string>

namespace opal {
struct Paths {
    std::filesystem::path root, config, hosts, host, identity_key, identity_pub, authorized, cert, cert_key, logs;
    static Paths load();
};
bool ensure_layout(const Paths &p);

class Ini {
public:
    bool load(const std::filesystem::path &path);
    bool save(const std::filesystem::path &path) const;
    std::string get(const std::string &section, const std::string &key, const std::string &fallback="") const;
    int get_int(const std::string &section, const std::string &key, int fallback) const;
    bool get_bool(const std::string &section, const std::string &key, bool fallback) const;
    void set(const std::string &section, const std::string &key, const std::string &value);
    const std::map<std::string,std::map<std::string,std::string>>& sections() const { return data_; }
private:
    std::map<std::string,std::map<std::string,std::string>> data_;
};
std::string trim(std::string s);
std::string shell_quote(const std::string &s);
bool command_exists(const std::string &name);
}
