#pragma once
#include <filesystem>
#include <string>
#include <vector>
namespace opal {
std::string random_hex(size_t bytes);
std::string pairing_code();
std::string normalize_pairing_code(std::string code);
std::string hmac_sha256_hex(const std::string &key,const std::string &data);
bool secure_equal(const std::string&a,const std::string&b);
bool ensure_identity(const std::filesystem::path &priv,const std::filesystem::path &pub);
std::string public_key_hex(const std::filesystem::path &pub);
std::string sign_hex(const std::filesystem::path &priv,const std::string &message);
bool verify_hex(const std::string &pub_hex,const std::string &message,const std::string &sig_hex);
std::vector<unsigned char> unhex(const std::string &s);
std::string hex(const unsigned char *p,size_t n);
}
