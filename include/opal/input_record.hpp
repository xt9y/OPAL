#pragma once

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace opal {

inline constexpr std::uint32_t kInputRecordMagic=0x4f50494eU; // OPIN
inline constexpr std::uint8_t kInputRecordVersion=1;
inline constexpr std::size_t kInputRecordBytes=16;

enum class InputRecordType : std::uint8_t {
    Key=1,
    Pointer=2,
    Button=3,
    Wheel=4,
    Relative=5,
};

struct InputRecord {
    InputRecordType type=InputRecordType::Key;
    std::int32_t a=0;
    std::int32_t b=0;
};

namespace detail {
inline void put32(std::uint8_t*p,std::uint32_t value){
    p[0]=static_cast<std::uint8_t>(value>>24);
    p[1]=static_cast<std::uint8_t>(value>>16);
    p[2]=static_cast<std::uint8_t>(value>>8);
    p[3]=static_cast<std::uint8_t>(value);
}
inline std::uint32_t get32(const std::uint8_t*p){
    return (static_cast<std::uint32_t>(p[0])<<24)|
           (static_cast<std::uint32_t>(p[1])<<16)|
           (static_cast<std::uint32_t>(p[2])<<8)|p[3];
}
inline bool valid_type(InputRecordType type){
    const auto value=static_cast<unsigned>(type);
    return value>=static_cast<unsigned>(InputRecordType::Key)&&
           value<=static_cast<unsigned>(InputRecordType::Relative);
}
inline bool parse_i32(std::string_view token,std::int32_t&value){
    if(token.empty())return false;
    std::int32_t parsed=0;
    const auto result=std::from_chars(token.data(),token.data()+token.size(),parsed);
    if(result.ec!=std::errc{}||result.ptr!=token.data()+token.size())return false;
    value=parsed;
    return true;
}
inline bool next_token(std::string_view line,std::size_t&cursor,std::string_view&token){
    while(cursor<line.size()&&(line[cursor]==' '||line[cursor]=='\t'))++cursor;
    if(cursor>=line.size())return false;
    const auto begin=cursor;
    while(cursor<line.size()&&line[cursor]!=' '&&line[cursor]!='\t'&&line[cursor]!='\r'&&line[cursor]!='\n')++cursor;
    token=line.substr(begin,cursor-begin);
    return !token.empty();
}
}

inline std::array<std::uint8_t,kInputRecordBytes> encode_input_record(const InputRecord&record){
    std::array<std::uint8_t,kInputRecordBytes> out{};
    detail::put32(out.data(),kInputRecordMagic);
    out[4]=kInputRecordVersion;
    out[5]=static_cast<std::uint8_t>(record.type);
    detail::put32(out.data()+8,static_cast<std::uint32_t>(record.a));
    detail::put32(out.data()+12,static_cast<std::uint32_t>(record.b));
    return out;
}

inline bool decode_input_record(std::span<const std::uint8_t>bytes,InputRecord&record){
    if(bytes.size()!=kInputRecordBytes||detail::get32(bytes.data())!=kInputRecordMagic||
       bytes[4]!=kInputRecordVersion||bytes[6]!=0||bytes[7]!=0)return false;
    const auto type=static_cast<InputRecordType>(bytes[5]);
    if(!detail::valid_type(type))return false;
    record.type=type;
    record.a=static_cast<std::int32_t>(detail::get32(bytes.data()+8));
    record.b=static_cast<std::int32_t>(detail::get32(bytes.data()+12));
    return true;
}

inline bool parse_input_command(std::string_view line,InputRecord&record){
    std::size_t cursor=0;std::string_view command,a,b,extra;
    if(!detail::next_token(line,cursor,command))return false;
    auto finish=[&](){return !detail::next_token(line,cursor,extra);};
    if(command=="KEY"){
        if(!detail::next_token(line,cursor,a)||!detail::next_token(line,cursor,b)||!finish())return false;
        record.type=InputRecordType::Key;
        return detail::parse_i32(a,record.a)&&detail::parse_i32(b,record.b)&&(record.b==0||record.b==1)&&record.a>0;
    }
    if(command=="POINTER"){
        if(!detail::next_token(line,cursor,a)||!detail::next_token(line,cursor,b)||!finish())return false;
        record.type=InputRecordType::Pointer;
        return detail::parse_i32(a,record.a)&&detail::parse_i32(b,record.b)&&record.a>=0&&record.a<=65535&&record.b>=0&&record.b<=65535;
    }
    if(command=="BUTTON"){
        if(!detail::next_token(line,cursor,a)||!detail::next_token(line,cursor,b)||!finish())return false;
        record.type=InputRecordType::Button;
        return detail::parse_i32(a,record.a)&&detail::parse_i32(b,record.b)&&record.a>=1&&record.a<=3&&(record.b==0||record.b==1);
    }
    if(command=="WHEEL"){
        if(!detail::next_token(line,cursor,a)||!finish())return false;
        record.type=InputRecordType::Wheel;record.b=0;
        return detail::parse_i32(a,record.a);
    }
    if(command=="MOUSE"){
        if(!detail::next_token(line,cursor,a)||!detail::next_token(line,cursor,b)||!finish())return false;
        record.type=InputRecordType::Relative;
        return detail::parse_i32(a,record.a)&&detail::parse_i32(b,record.b);
    }
    return false;
}

inline bool encode_input_command(std::string_view line,std::array<std::uint8_t,kInputRecordBytes>&out){
    InputRecord record;
    if(!parse_input_command(line,record))return false;
    out=encode_input_record(record);
    return true;
}

} // namespace opal
