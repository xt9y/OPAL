#include <opal/input_record.hpp>
#include <opal/input_wire.hpp>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <set>
#include <span>
#include <string_view>
#include <vector>

namespace {

struct ScanCode {
    WORD code = 0;
    bool extended = false;
};

std::optional<ScanCode> scan_code_for(int wire)
{
    using namespace opal::wire_key;
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

bool post_key(int code, bool down)
{
    const auto scan = scan_code_for(code);
    if (!scan) return true;
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wScan = scan->code;
    input.ki.dwFlags = KEYEVENTF_SCANCODE;
    if (scan->extended) input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    if (!down) input.ki.dwFlags |= KEYEVENTF_KEYUP;
    return send_one(input);
}

bool post_pointer_absolute(int x, int y)
{
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dx = std::clamp<LONG>(x, 0, 65535);
    input.mi.dy = std::clamp<LONG>(y, 0, 65535);
    input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    return send_one(input);
}

bool post_pointer_relative(int dx, int dy)
{
    if (dx == 0 && dy == 0) return true;
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dx = dx;
    input.mi.dy = dy;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    return send_one(input);
}

DWORD button_flag(int button, bool down)
{
    if (button == 1) return down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    if (button == 2) return down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
    if (button == 3) return down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
    return 0;
}

bool post_button(int button, bool down)
{
    const DWORD flag = button_flag(button, down);
    if (!flag) return true;
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = flag;
    return send_one(input);
}

bool post_wheel(int amount)
{
    if (amount == 0) return true;
    const long long scaled = static_cast<long long>(amount) * WHEEL_DELTA;
    const auto clamped = std::clamp<long long>(scaled, INT32_MIN, INT32_MAX);
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.mouseData = static_cast<DWORD>(static_cast<std::int32_t>(clamped));
    input.mi.dwFlags = MOUSEEVENTF_WHEEL;
    return send_one(input);
}

bool apply_record(const opal::InputRecord& record, std::set<int>& held_keys, std::set<int>& held_buttons)
{
    switch (record.type) {
        case opal::InputRecordType::Key:
            if (record.b != 0 && record.b != 1) return true;
            if (!post_key(record.a, record.b != 0)) return false;
            if (record.b) held_keys.insert(record.a); else held_keys.erase(record.a);
            return true;
        case opal::InputRecordType::Pointer:
            return post_pointer_absolute(record.a, record.b);
        case opal::InputRecordType::Button:
            if (record.b != 0 && record.b != 1) return true;
            if (!post_button(record.a, record.b != 0)) return false;
            if (record.b) held_buttons.insert(record.a); else held_buttons.erase(record.a);
            return true;
        case opal::InputRecordType::Wheel:
            return post_wheel(record.a);
        case opal::InputRecordType::Relative:
            return post_pointer_relative(record.a, record.b);
    }
    return true;
}

bool consume_pending(std::vector<std::uint8_t>& pending, std::set<int>& held_keys,
                     std::set<int>& held_buttons, bool eof)
{
    for (;;) {
        if (pending.empty()) return true;
        const bool binary = pending.size() >= 4 && pending[0] == 'O' && pending[1] == 'P' &&
                            pending[2] == 'I' && pending[3] == 'N';
        if (binary) {
            if (pending.size() < opal::kInputRecordBytes) return !eof;
            opal::InputRecord record;
            if (!opal::decode_input_record(std::span<const std::uint8_t>(pending.data(), opal::kInputRecordBytes), record)) return false;
            if (!apply_record(record, held_keys, held_buttons)) return false;
            pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(opal::kInputRecordBytes));
            continue;
        }

        const auto newline = std::find(pending.begin(), pending.end(), static_cast<std::uint8_t>('\n'));
        if (newline == pending.end()) {
            if (!eof) {
                if (pending.size() > 512) return false;
                return true;
            }
            if (pending.empty()) return true;
        }
        const std::size_t length = newline == pending.end() ? pending.size() : static_cast<std::size_t>(newline - pending.begin());
        const std::string_view line(reinterpret_cast<const char*>(pending.data()), length);
        opal::InputRecord record;
        if (!line.empty() && opal::parse_input_command(line, record) && !apply_record(record, held_keys, held_buttons)) return false;
        const std::size_t consumed = length + (newline == pending.end() ? 0 : 1);
        pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(consumed));
        if (newline == pending.end()) return true;
    }
}

void release_held(const std::set<int>& keys, const std::set<int>& buttons)
{
    for (const int code : keys) (void)post_key(code, false);
    for (const int button : buttons) (void)post_button(button, false);
}

}

int main()
{
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    if (!input || input == INVALID_HANDLE_VALUE) return 1;

    std::set<int> held_keys, held_buttons;
    std::vector<std::uint8_t> pending;
    pending.reserve(1024);
    std::array<std::uint8_t, 1024> chunk{};
    bool ok = true;

    for (;;) {
        DWORD read = 0;
        if (!ReadFile(input, chunk.data(), static_cast<DWORD>(chunk.size()), &read, nullptr)) {
            if (GetLastError() == ERROR_BROKEN_PIPE) {
                ok = consume_pending(pending, held_keys, held_buttons, true);
                break;
            }
            ok = false;
            break;
        }
        if (read == 0) {
            ok = consume_pending(pending, held_keys, held_buttons, true);
            break;
        }
        pending.insert(pending.end(), chunk.begin(), chunk.begin() + read);
        if (!consume_pending(pending, held_keys, held_buttons, false)) {
            ok = false;
            break;
        }
    }

    release_held(held_keys, held_buttons);
    return ok ? 0 : 1;
}
