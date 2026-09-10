#include <opal/windows_input_inject.hpp>

#include <opal/input_record.hpp>
#include <opal/input_wire.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <optional>

namespace opal {
namespace {

struct ScanCode {
    WORD code = 0;
    bool extended = false;
};

std::optional<ScanCode> scan_code_for(int wire)
{
    using namespace wire_key;
    if (wire >= Esc && wire <= KpDot) return ScanCode{static_cast<WORD>(wire), false};
    switch (wire) {
        case F11: return ScanCode{0x57, false};
        case F12: return ScanCode{0x58, false};
        case KpEnter: return ScanCode{0x1c, true};
        case RightCtrl: return ScanCode{0x1d, true};
        case KpSlash: return ScanCode{0x35, true};
        case SysRq: return ScanCode{0x37, true};
        case RightAlt: return ScanCode{0x38, true};
        case Home: return ScanCode{0x47, true};
        case Up: return ScanCode{0x48, true};
        case PageUp: return ScanCode{0x49, true};
        case Left: return ScanCode{0x4b, true};
        case Right: return ScanCode{0x4d, true};
        case End: return ScanCode{0x4f, true};
        case Down: return ScanCode{0x50, true};
        case PageDown: return ScanCode{0x51, true};
        case Insert: return ScanCode{0x52, true};
        case Delete: return ScanCode{0x53, true};
        case Pause: return ScanCode{0x45, false};
        case LeftMeta: return ScanCode{0x5b, true};
        case RightMeta: return ScanCode{0x5c, true};
        case Compose: return ScanCode{0x5d, true};
        default: return std::nullopt;
    }
}

bool send_one(INPUT input)
{
    return SendInput(1, &input, sizeof(INPUT)) == 1;
}

bool apply_record(const InputRecord& record)
{
    INPUT input{};
    switch (record.type) {
        case InputRecordType::Key: {
            const auto scan = scan_code_for(record.a);
            if (!scan) return true;
            input.type = INPUT_KEYBOARD;
            input.ki.wScan = scan->code;
            input.ki.dwFlags = KEYEVENTF_SCANCODE;
            if (scan->extended) input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
            if (!record.b) input.ki.dwFlags |= KEYEVENTF_KEYUP;
            return send_one(input);
        }
        case InputRecordType::Pointer:
            input.type = INPUT_MOUSE;
            input.mi.dx = std::clamp<LONG>(record.a, 0, 65535);
            input.mi.dy = std::clamp<LONG>(record.b, 0, 65535);
            input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
            return send_one(input);
        case InputRecordType::Button:
            input.type = INPUT_MOUSE;
            if (record.a == 1) input.mi.dwFlags = record.b ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
            else if (record.a == 2) input.mi.dwFlags = record.b ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
            else if (record.a == 3) input.mi.dwFlags = record.b ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
            else return true;
            return send_one(input);
        case InputRecordType::Wheel: {
            input.type = INPUT_MOUSE;
            const auto delta = std::clamp<long long>(static_cast<long long>(record.a) * WHEEL_DELTA,
                                                     INT32_MIN, INT32_MAX);
            input.mi.mouseData = static_cast<DWORD>(static_cast<std::int32_t>(delta));
            input.mi.dwFlags = MOUSEEVENTF_WHEEL;
            return send_one(input);
        }
        case InputRecordType::Relative:
            if (record.a == 0 && record.b == 0) return true;
            input.type = INPUT_MOUSE;
            input.mi.dx = record.a;
            input.mi.dy = record.b;
            input.mi.dwFlags = MOUSEEVENTF_MOVE;
            return send_one(input);
    }
    return false;
}

}

bool windows_input_send(std::string_view command)
{
    InputRecord record;
    return parse_input_command(command, record) && apply_record(record);
}

}
