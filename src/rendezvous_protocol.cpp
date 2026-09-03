#include <opal/rendezvous_protocol.hpp>

#include <openssl/evp.h>
#include <array>
#include <cctype>
#include <charconv>
#include <sstream>
#include <string>
#include <vector>

namespace opal { namespace {
constexpr char kAlphabet[]="0123456789ABCDEFGHJKMNPQRSTVWXYZ";

bool digest(std::string_view input,std::array<unsigned char,32>&out){
    unsigned int length=0;
    return EVP_Digest(input.data(),input.size(),out.data(),&length,EVP_sha256(),nullptr)==1&&length==out.size();
}

int alphabet_value(char c){
    c=static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    for(int i=0;i<32;++i)if(kAlphabet[i]==c)return i;
    return -1;
}

bool hex_string(std::string_view value,std::size_t exact){
    if(value.size()!=exact)return false;
    for(char c:value)if(!std::isxdigit(static_cast<unsigned char>(c)))return false;
    return true;
}

bool token_string(std::string_view value,std::size_t max){
    if(value.empty()||value.size()>max)return false;
    for(char c:value)if(std::isspace(static_cast<unsigned char>(c))||static_cast<unsigned char>(c)<0x21||static_cast<unsigned char>(c)>0x7e)return false;
    return true;
}

std::string checksum_for(std::string_view data){
    std::array<unsigned char,32> hash{};
    const std::string input="OPAL-ID-v1\n"+std::string(data);
    if(!digest(input,hash))return {};
    const unsigned value=(static_cast<unsigned>(hash[0])<<2)|(hash[1]>>6);
    std::string result;result.push_back(kAlphabet[(value>>5)&31]);result.push_back(kAlphabet[value&31]);return result;
}

bool valid_id(std::string_view id){
    if(id.size()!=kRendezvousIdChars)return false;
    for(char c:id)if(alphabet_value(c)<0)return false;
    std::string upper;upper.reserve(id.size());for(char c:id)upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    return checksum_for(std::string_view(upper).substr(0,10))==std::string_view(upper).substr(10,2);
}

bool parse_u16(std::string_view text,std::uint16_t &out){
    unsigned value=0;auto first=text.data(),last=first+text.size();auto [ptr,ec]=std::from_chars(first,last,value);if(ec!=std::errc{}||ptr!=last||value==0||value>65535)return false;out=static_cast<std::uint16_t>(value);return true;
}
bool parse_u32(std::string_view text,std::uint32_t &out){
    unsigned long long value=0;auto first=text.data(),last=first+text.size();auto [ptr,ec]=std::from_chars(first,last,value);if(ec!=std::errc{}||ptr!=last||value==0||value>300)return false;out=static_cast<std::uint32_t>(value);return true;
}

std::vector<std::string> split(std::string_view wire){
    std::vector<std::string> fields;std::istringstream in{std::string(wire)};std::string field;while(in>>field)fields.push_back(std::move(field));return fields;
}

bool same_id_key(std::string_view id,std::string_view key){return rendezvous_id_from_public_key(key)==id;}

}

std::string rendezvous_id_from_public_key(std::string_view public_key_hex){
    if(!hex_string(public_key_hex,64))return {};
    std::string normalized;normalized.reserve(public_key_hex.size());for(char c:public_key_hex)normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    std::array<unsigned char,32> hash{};if(!digest("OPAL-RENDEZVOUS-ID-v1\n"+normalized,hash))return {};
    std::uint64_t value=0;for(int i=0;i<8;++i)value=(value<<8)|hash[static_cast<std::size_t>(i)];
    std::string data;data.reserve(10);for(int i=0;i<10;++i){const int shift=59-i*5;data.push_back(kAlphabet[(value>>shift)&31]);}
    return data+checksum_for(data);
}

std::string format_connection_code(std::string_view rendezvous_id){
    if(!valid_id(rendezvous_id))return {};
    std::string upper;upper.reserve(5+14);upper="opal:";
    for(std::size_t i=0;i<rendezvous_id.size();++i){if(i&&i%4==0)upper.push_back('-');upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(rendezvous_id[i]))));}
    return upper;
}

bool parse_connection_code(std::string_view code,std::string &rendezvous_id){
    rendezvous_id.clear();if(code.size()<5||code.substr(0,5)!="opal:")return false;
    std::string compact;for(char c:code.substr(5)){if(c=='-')continue;if(alphabet_value(c)<0)return false;compact.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));}
    if(!valid_id(compact))return false;rendezvous_id=std::move(compact);return true;
}

std::string serialize_rendezvous_message(const RendezvousMessage &m){
    std::ostringstream out;
    switch(m.type){
    case RendezvousType::LeaseHello:
        if(!valid_id(m.id)||!hex_string(m.public_key,64)||!same_id_key(m.id,m.public_key))return {};
        out<<"LEASE_HELLO "<<m.id<<' '<<m.public_key;break;
    case RendezvousType::LeaseChallenge:
        if(!valid_id(m.id)||!hex_string(m.nonce,32))return {};
        out<<"LEASE_CHALLENGE "<<m.id<<' '<<m.nonce;break;
    case RendezvousType::LeaseProof:
        if(!valid_id(m.id)||!hex_string(m.public_key,64)||!same_id_key(m.id,m.public_key)||!hex_string(m.nonce,32)||!hex_string(m.signature,128))return {};
        out<<"LEASE_PROOF "<<m.id<<' '<<m.public_key<<' '<<m.nonce<<' '<<m.signature;break;
    case RendezvousType::LeaseOk:
        if(!valid_id(m.id)||m.ttl_seconds==0||m.ttl_seconds>300)return {};
        out<<"LEASE_OK "<<m.id<<' '<<m.ttl_seconds;break;
    case RendezvousType::Introduce:
        if(!valid_id(m.id)||!hex_string(m.public_key,64)||!hex_string(m.nonce,32))return {};
        out<<"INTRO "<<m.id<<' '<<m.public_key<<' '<<m.nonce;break;
    case RendezvousType::Offer:
        if(!valid_id(m.id)||!hex_string(m.session_id,32)||!hex_string(m.public_key,64)||!token_string(m.host,255)||m.port==0||!hex_string(m.nonce,32))return {};
        out<<"OFFER "<<m.id<<' '<<m.session_id<<' '<<m.public_key<<' '<<m.host<<' '<<m.port<<' '<<m.nonce;break;
    case RendezvousType::Ready:
        if(!valid_id(m.id)||!hex_string(m.session_id,32)||!hex_string(m.public_key,64)||!same_id_key(m.id,m.public_key)||!token_string(m.host,255)||m.port==0||!hex_string(m.nonce,32))return {};
        out<<"READY "<<m.id<<' '<<m.session_id<<' '<<m.public_key<<' '<<m.host<<' '<<m.port<<' '<<m.nonce;break;
    case RendezvousType::RelayRequest:
        if(!hex_string(m.session_id,32)||!hex_string(m.public_key,64)||!hex_string(m.nonce,32)||!hex_string(m.signature,128))return {};
        out<<"RELAY_REQUEST "<<m.session_id<<' '<<m.public_key<<' '<<m.nonce<<' '<<m.signature;break;
    case RendezvousType::RelayReady:
        if(!hex_string(m.session_id,32)||!token_string(m.host,255)||m.port==0||!hex_string(m.allocation_id,32)||m.ttl_seconds==0||m.ttl_seconds>300)return {};
        out<<"RELAY_READY "<<m.session_id<<' '<<m.host<<' '<<m.port<<' '<<m.allocation_id<<' '<<m.ttl_seconds;break;
    case RendezvousType::Error:
        if(!token_string(m.error_code,64))return {};
        out<<"ERROR "<<m.error_code;break;
    }
    const auto result=out.str();return result.size()<=kRendezvousMaxMessageBytes?result:std::string{};
}

bool parse_rendezvous_message(std::string_view wire,RendezvousMessage &m){
    m={};if(wire.empty()||wire.size()>kRendezvousMaxMessageBytes)return false;for(char c:wire)if(c=='\0'||c=='\r'||c=='\n')return false;
    const auto f=split(wire);if(f.empty())return false;
    if(f[0]=="LEASE_HELLO"&&f.size()==3){m.type=RendezvousType::LeaseHello;m.id=f[1];m.public_key=f[2];}
    else if(f[0]=="LEASE_CHALLENGE"&&f.size()==3){m.type=RendezvousType::LeaseChallenge;m.id=f[1];m.nonce=f[2];}
    else if(f[0]=="LEASE_PROOF"&&f.size()==5){m.type=RendezvousType::LeaseProof;m.id=f[1];m.public_key=f[2];m.nonce=f[3];m.signature=f[4];}
    else if(f[0]=="LEASE_OK"&&f.size()==3){m.type=RendezvousType::LeaseOk;m.id=f[1];if(!parse_u32(f[2],m.ttl_seconds))return false;}
    else if(f[0]=="INTRO"&&f.size()==4){m.type=RendezvousType::Introduce;m.id=f[1];m.public_key=f[2];m.nonce=f[3];}
    else if(f[0]=="OFFER"&&f.size()==7){m.type=RendezvousType::Offer;m.id=f[1];m.session_id=f[2];m.public_key=f[3];m.host=f[4];if(!parse_u16(f[5],m.port))return false;m.nonce=f[6];}
    else if(f[0]=="READY"&&f.size()==7){m.type=RendezvousType::Ready;m.id=f[1];m.session_id=f[2];m.public_key=f[3];m.host=f[4];if(!parse_u16(f[5],m.port))return false;m.nonce=f[6];}
    else if(f[0]=="RELAY_REQUEST"&&f.size()==5){m.type=RendezvousType::RelayRequest;m.session_id=f[1];m.public_key=f[2];m.nonce=f[3];m.signature=f[4];}
    else if(f[0]=="RELAY_READY"&&f.size()==6){m.type=RendezvousType::RelayReady;m.session_id=f[1];m.host=f[2];if(!parse_u16(f[3],m.port))return false;m.allocation_id=f[4];if(!parse_u32(f[5],m.ttl_seconds))return false;}
    else if(f[0]=="ERROR"&&f.size()==2){m.type=RendezvousType::Error;m.error_code=f[1];}
    else return false;
    return !serialize_rendezvous_message(m).empty();
}

}
