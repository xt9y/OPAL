#include <opal/connection_code.hpp>

#include <openssl/evp.h>
#include <array>
#include <cctype>
#include <cstdint>
#include <string>

namespace opal { namespace {
constexpr char kAlphabet[]="0123456789ABCDEFGHJKMNPQRSTVWXYZ";

bool digest(std::string_view input,std::array<unsigned char,32>&out){unsigned int length=0;return EVP_Digest(input.data(),input.size(),out.data(),&length,EVP_sha256(),nullptr)==1&&length==out.size();}
int alphabet_value(char c){c=static_cast<char>(std::toupper(static_cast<unsigned char>(c)));for(int i=0;i<32;++i)if(kAlphabet[i]==c)return i;return -1;}
char canonical_input_char(char c){c=static_cast<char>(std::toupper(static_cast<unsigned char>(c)));if(c=='O')return '0';if(c=='I'||c=='L')return '1';return c;}
bool hex_string(std::string_view value,std::size_t exact){if(value.size()!=exact)return false;for(char c:value)if(!std::isxdigit(static_cast<unsigned char>(c)))return false;return true;}
std::string checksum_for(std::string_view data){std::array<unsigned char,32> hash{};const std::string input="OPAL-ID-v1\n"+std::string(data);if(!digest(input,hash))return {};const unsigned value=(static_cast<unsigned>(hash[0])<<2)|(hash[1]>>6);std::string result;result.push_back(kAlphabet[(value>>5)&31]);result.push_back(kAlphabet[value&31]);return result;}
bool valid_id(std::string_view id){if(id.size()!=kConnectionIdChars)return false;for(char c:id)if(alphabet_value(c)<0)return false;std::string upper;upper.reserve(id.size());for(char c:id)upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));return checksum_for(std::string_view(upper).substr(0,10))==std::string_view(upper).substr(10,2);}
}

std::string connection_id_from_public_key(std::string_view public_key_hex){
    if(!hex_string(public_key_hex,64))return {};
    std::string normalized;normalized.reserve(public_key_hex.size());for(char c:public_key_hex)normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    std::array<unsigned char,32> hash{};
    // Keep the original identity-domain string so existing OPAL connection codes remain valid.
    if(!digest("OPAL-RENDEZVOUS-ID-v1\n"+normalized,hash))return {};
    std::uint64_t value=0;for(int i=0;i<8;++i)value=(value<<8)|hash[static_cast<std::size_t>(i)];
    std::string data;data.reserve(10);for(int i=0;i<10;++i){const int shift=59-i*5;data.push_back(kAlphabet[(value>>shift)&31]);}
    return data+checksum_for(data);
}

std::string format_connection_code(std::string_view connection_id){if(!valid_id(connection_id))return {};std::string upper;upper.reserve(14);for(std::size_t i=0;i<connection_id.size();++i){if(i&&i%4==0)upper.push_back('-');upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(connection_id[i]))));}return upper;}

bool parse_connection_code(std::string_view code,std::string &connection_id){connection_id.clear();std::string_view body=code;if(body.size()>=5&&body.substr(0,5)=="opal:")body.remove_prefix(5);std::string compact;for(char c:body){if(c=='-')continue;const char canonical=canonical_input_char(c);if(alphabet_value(canonical)<0)return false;compact.push_back(canonical);}if(!valid_id(compact))return false;connection_id=std::move(compact);return true;}

}
