#include <opal/input_record.hpp>

#include <array>
#include <cassert>
#include <cstdint>

int main(){
    using namespace opal;
    InputRecord record{};
    assert(parse_input_command("KEY 30 1",record));
    assert(record.type==InputRecordType::Key&&record.a==30&&record.b==1);
    assert(parse_input_command("POINTER 65535 0",record));
    assert(record.type==InputRecordType::Pointer&&record.a==65535&&record.b==0);
    assert(parse_input_command("BUTTON 3 0",record));
    assert(record.type==InputRecordType::Button&&record.a==3&&record.b==0);
    assert(parse_input_command("WHEEL -2",record));
    assert(record.type==InputRecordType::Wheel&&record.a==-2&&record.b==0);
    assert(parse_input_command("MOUSE -17 23",record));
    assert(record.type==InputRecordType::Relative&&record.a==-17&&record.b==23);

    assert(!parse_input_command("KEY 30 2",record));
    assert(!parse_input_command("POINTER 65536 0",record));
    assert(!parse_input_command("BUTTON 4 1",record));
    assert(!parse_input_command("WHEEL 1 junk",record));
    assert(!parse_input_command("MOUSE 1",record));

    const InputRecord original{InputRecordType::Relative,-123456,654321};
    const auto wire=encode_input_record(original);
    assert(wire.size()==kInputRecordBytes);
    assert(wire[0]=='O'&&wire[1]=='P'&&wire[2]=='I'&&wire[3]=='N');
    InputRecord decoded{};
    assert(decode_input_record(wire,decoded));
    assert(decoded.type==original.type&&decoded.a==original.a&&decoded.b==original.b);

    auto bad=wire;bad[0]^=1;assert(!decode_input_record(bad,decoded));
    bad=wire;bad[4]=2;assert(!decode_input_record(bad,decoded));
    bad=wire;bad[5]=99;assert(!decode_input_record(bad,decoded));
    bad=wire;bad[6]=1;assert(!decode_input_record(bad,decoded));

    std::array<std::uint8_t,kInputRecordBytes> encoded{};
    assert(encode_input_command("KEY 42 0",encoded));
    assert(decode_input_record(encoded,decoded));
    assert(decoded.type==InputRecordType::Key&&decoded.a==42&&decoded.b==0);
    return 0;
}
