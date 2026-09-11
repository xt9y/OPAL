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
#include <cwchar>
#include <cwctype>
#include <optional>
#include <string>

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

bool contains_case_insensitive(const wchar_t* value, const wchar_t* needle)
{
    if (!value || !needle || !*needle) return false;
    std::wstring haystack(value);
    std::wstring pattern(needle);
    std::transform(haystack.begin(), haystack.end(), haystack.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towupper(c));
    });
    std::transform(pattern.begin(), pattern.end(), pattern.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towupper(c));
    });
    return haystack.find(pattern) != std::wstring::npos;
}

bool opal_display_text(const wchar_t* value)
{
    return contains_case_insensitive(value, L"OPALDISPLAY") ||
           contains_case_insensitive(value, L"OPAL VIRTUAL DISPLAY");
}

bool display_device_is_opal(const DISPLAY_DEVICEW& adapter)
{
    if (opal_display_text(adapter.DeviceID) || opal_display_text(adapter.DeviceString)) return true;

    DISPLAY_DEVICEW monitor{};
    monitor.cb = sizeof(monitor);
    return EnumDisplayDevicesW(adapter.DeviceName, 0, &monitor, EDD_GET_DEVICE_INTERFACE_NAME) &&
           (opal_display_text(monitor.DeviceID) || opal_display_text(monitor.DeviceString));
}

bool opal_display_rect(RECT& rect)
{
    for (DWORD index = 0;; ++index) {
        DISPLAY_DEVICEW adapter{};
        adapter.cb = sizeof(adapter);
        if (!EnumDisplayDevicesW(nullptr, index, &adapter, 0)) break;
        if ((adapter.StateFlags & DISPLAY_DEVICE_ACTIVE) == 0 || !display_device_is_opal(adapter)) continue;

        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        if (!EnumDisplaySettingsExW(adapter.DeviceName, ENUM_CURRENT_SETTINGS, &mode, 0)) continue;
        if (mode.dmPelsWidth == 0 || mode.dmPelsHeight == 0) continue;

        rect.left = mode.dmPosition.x;
        rect.top = mode.dmPosition.y;
        rect.right = rect.left + static_cast<LONG>(mode.dmPelsWidth);
        rect.bottom = rect.top + static_cast<LONG>(mode.dmPelsHeight);
        return rect.right > rect.left && rect.bottom > rect.top;
    }
    return false;
}

LONG normalized_virtual_coordinate(LONG pixel, LONG origin, int extent)
{
    if (extent <= 1) return 0;
    const auto max_pixel = static_cast<long long>(extent - 1);
    const auto local = std::clamp<long long>(static_cast<long long>(pixel) - origin, 0, max_pixel);
    return static_cast<LONG>((local * 65535LL + max_pixel / 2) / max_pixel);
}

void pointer_coordinates(const InputRecord& record, LONG& x, LONG& y)
{
    x = std::clamp<LONG>(record.a, 0, 65535);
    y = std::clamp<LONG>(record.b, 0, 65535);

    RECT target{};
    if (!opal_display_rect(target)) return;

    const int target_width = static_cast<int>(target.right - target.left);
    const int target_height = static_cast<int>(target.bottom - target.top);
    if (target_width <= 0 || target_height <= 0) return;

    const auto target_x = target.left + static_cast<LONG>(
        (static_cast<long long>(x) * (target_width - 1) + 32767LL) / 65535LL);
    const auto target_y = target.top + static_cast<LONG>(
        (static_cast<long long>(y) * (target_height - 1) + 32767LL) / 65535LL);

    const LONG virtual_left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const LONG virtual_top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int virtual_width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int virtual_height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (virtual_width <= 0 || virtual_height <= 0) return;

    x = normalized_virtual_coordinate(target_x, virtual_left, virtual_width);
    y = normalized_virtual_coordinate(target_y, virtual_top, virtual_height);
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
            pointer_coordinates(record, input.mi.dx, input.mi.dy);
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
