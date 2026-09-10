#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace opal {

constexpr std::size_t kConnectionIdChars=12;

std::string connection_id_from_public_key(std::string_view public_key_hex);
std::string format_connection_code(std::string_view connection_id);
bool parse_connection_code(std::string_view code,std::string &connection_id);

}
