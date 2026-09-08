#include <opal/input_record.hpp>
#include <opal/input_wire.hpp>

#import <ApplicationServices/ApplicationServices.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <optional>
#include <set>
#include <span>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

bool accessibility_trusted(bool prompt)
{
    if (!prompt) return AXIsProcessTrusted();
    const void* keys[] = {kAXTrustedCheckOptionPrompt};
    const void* values[] = {kCFBooleanTrue};
    CFDictionaryRef options = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1,
                                                   &kCFTypeDictionaryKeyCallBacks,
                                                   &kCFTypeDictionaryValueCallBacks);
    if (!options) return AXIsProcessTrusted();
    const bool trusted = AXIsProcessTrustedWithOptions(options);
    CFRelease(options);
    return trusted;
}

std::optional<CGKeyCode> mac_keycode(int code)
{
    using namespace opal::wire_key;
    switch (code) {
        case A: return 0; case S: return 1; case D: return 2; case F: return 3;
        case H: return 4; case G: return 5; case Z: return 6; case X: return 7;
        case C: return 8; case V: return 9; case B: return 11; case Q: return 12;
        case W: return 13; case E: return 14; case R: return 15; case Y: return 16;
        case T: return 17; case Num1: return 18; case Num2: return 19; case Num3: return 20;
        case Num4: return 21; case Num6: return 22; case Num5: return 23; case Equal: return 24;
        case Num9: return 25; case Num7: return 26; case Minus: return 27; case Num8: return 28;
        case Num0: return 29; case RightBrace: return 30; case O: return 31; case U: return 32;
        case LeftBrace: return 33; case I: return 34; case P: return 35; case Enter: return 36;
        case L: return 37; case J: return 38; case Apostrophe: return 39; case K: return 40;
        case Semicolon: return 41; case Backslash: return 42; case Comma: return 43; case Slash: return 44;
        case N: return 45; case M: return 46; case Dot: return 47; case Tab: return 48;
        case Space: return 49; case Grave: return 50; case Backspace: return 51; case Esc: return 53;
        case RightMeta: return 54; case LeftMeta: return 55; case LeftShift: return 56; case CapsLock: return 57;
        case LeftAlt: return 58; case LeftCtrl: return 59; case RightShift: return 60; case RightAlt: return 61;
        case RightCtrl: return 62; case KpDot: return 65; case KpAsterisk: return 67; case KpPlus: return 69;
        case NumLock: return 71; case KpSlash: return 75; case KpEnter: return 76; case KpMinus: return 78;
        case Kp0: return 82; case Kp1: return 83; case Kp2: return 84; case Kp3: return 85;
        case Kp4: return 86; case Kp5: return 87; case Kp6: return 88; case Kp7: return 89;
        case Kp8: return 91; case Kp9: return 92;
        case F5: return 96; case F6: return 97; case F7: return 98; case F3: return 99;
        case F8: return 100; case F9: return 101; case F11: return 103; case F10: return 109;
        case F12: return 111; case Insert: return 114; case Home: return 115; case PageUp: return 116;
        case Delete: return 117; case F4: return 118; case End: return 119; case F2: return 120;
        case PageDown: return 121; case F1: return 122; case Left: return 123; case Right: return 124;
        case Down: return 125; case Up: return 126;
        default: return std::nullopt;
    }
}

CGPoint cursor_location()
{
    CGEventRef event = CGEventCreate(nullptr);
    if (!event) return CGPointZero;
    const CGPoint point = CGEventGetLocation(event);
    CFRelease(event);
    return point;
}

bool post_key(int code, bool down)
{
    const auto key = mac_keycode(code);
    if (!key) return true;
    CGEventRef event = CGEventCreateKeyboardEvent(nullptr, *key, down);
    if (!event) return false;
    CGEventPost(kCGHIDEventTap, event);
    CFRelease(event);
    return true;
}

bool post_pointer_absolute(int x, int y)
{
    constexpr double max_value = 65535.0;
    const CGRect bounds = CGDisplayBounds(CGMainDisplayID());
    const double px = CGRectGetMinX(bounds) + std::clamp(x / max_value, 0.0, 1.0) * std::max(0.0, CGRectGetWidth(bounds) - 1.0);
    const double py = CGRectGetMinY(bounds) + std::clamp(y / max_value, 0.0, 1.0) * std::max(0.0, CGRectGetHeight(bounds) - 1.0);
    CGEventRef event = CGEventCreateMouseEvent(nullptr, kCGEventMouseMoved, CGPointMake(px, py), kCGMouseButtonLeft);
    if (!event) return false;
    CGEventPost(kCGHIDEventTap, event);
    CFRelease(event);
    return true;
}

bool post_pointer_relative(int dx, int dy)
{
    if (dx == 0 && dy == 0) return true;
    const CGPoint current = cursor_location();
    CGEventRef event = CGEventCreateMouseEvent(nullptr, kCGEventMouseMoved,
                                               CGPointMake(current.x + dx, current.y + dy),
                                               kCGMouseButtonLeft);
    if (!event) return false;
    CGEventSetIntegerValueField(event, kCGMouseEventDeltaX, dx);
    CGEventSetIntegerValueField(event, kCGMouseEventDeltaY, dy);
    CGEventPost(kCGHIDEventTap, event);
    CFRelease(event);
    return true;
}

bool post_button(int button, bool down)
{
    CGMouseButton cg_button;
    CGEventType type;
    if (button == 1) { cg_button = kCGMouseButtonLeft; type = down ? kCGEventLeftMouseDown : kCGEventLeftMouseUp; }
    else if (button == 2) { cg_button = kCGMouseButtonCenter; type = down ? kCGEventOtherMouseDown : kCGEventOtherMouseUp; }
    else if (button == 3) { cg_button = kCGMouseButtonRight; type = down ? kCGEventRightMouseDown : kCGEventRightMouseUp; }
    else return true;
    CGEventRef event = CGEventCreateMouseEvent(nullptr, type, cursor_location(), cg_button);
    if (!event) return false;
    if (button == 2) CGEventSetIntegerValueField(event, kCGMouseEventButtonNumber, 2);
    CGEventPost(kCGHIDEventTap, event);
    CFRelease(event);
    return true;
}

bool post_wheel(int amount)
{
    CGEventRef event = CGEventCreateScrollWheelEvent(nullptr, kCGScrollEventUnitLine, 1, amount);
    if (!event) return false;
    CGEventPost(kCGHIDEventTap, event);
    CFRelease(event);
    return true;
}

bool apply_record(const opal::InputRecord &record, std::set<int> &held_keys, std::set<int> &held_buttons)
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

bool consume_pending(std::vector<std::uint8_t> &pending, std::set<int> &held_keys,
                     std::set<int> &held_buttons, bool eof)
{
    for (;;) {
        if (pending.empty()) return true;
        const bool binary = pending.size() >= 4 && pending[0] == 'O' && pending[1] == 'P' && pending[2] == 'I' && pending[3] == 'N';
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
            if (!eof) { if (pending.size() > 512) return false; return true; }
            if (pending.empty()) return true;
        }
        const std::size_t length = newline == pending.end() ? pending.size() : static_cast<std::size_t>(newline - pending.begin());
        const std::string_view line(reinterpret_cast<const char *>(pending.data()), length);
        opal::InputRecord record;
        if (!line.empty() && opal::parse_input_command(line, record) && !apply_record(record, held_keys, held_buttons)) return false;
        const std::size_t consumed = length + (newline == pending.end() ? 0 : 1);
        pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(consumed));
        if (newline == pending.end()) return true;
    }
}

void release_held(const std::set<int> &keys, const std::set<int> &buttons)
{
    for (const int code : keys) (void)post_key(code, false);
    for (const int button : buttons) (void)post_button(button, false);
}

}

int main(int argc, char** argv)
{
    if (argc == 2 && std::strcmp(argv[1], "--check-access") == 0)
        return accessibility_trusted(false) ? 0 : 2;
    if (argc == 2 && std::strcmp(argv[1], "--request-access") == 0) {
        const bool trusted = accessibility_trusted(true);
        if (!trusted)
            std::cerr << "OPAL input helper requires Accessibility permission. Enable opal-input in System Settings > Privacy & Security > Accessibility, then run setup again.\n";
        return trusted ? 0 : 2;
    }
    if (argc != 1) return 2;
    if (!accessibility_trusted(false)) {
        std::cerr << "OPAL input permission denied. Enable opal-input in System Settings > Privacy & Security > Accessibility.\n";
        return 2;
    }

    std::set<int> held_keys, held_buttons;
    std::vector<std::uint8_t> pending;
    pending.reserve(1024);
    std::array<std::uint8_t, 1024> chunk{};
    bool ok = true;
    for (;;) {
        ssize_t n = read(STDIN_FILENO, chunk.data(), chunk.size());
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { ok = false; break; }
        if (n == 0) { ok = consume_pending(pending, held_keys, held_buttons, true); break; }
        pending.insert(pending.end(), chunk.begin(), chunk.begin() + n);
        if (!consume_pending(pending, held_keys, held_buttons, false)) { ok = false; break; }
    }

    release_held(held_keys, held_buttons);
    return ok ? 0 : 1;
}