#include <opal/input.hpp>

#include <algorithm>
#include <cstdint>

namespace opal {

WireKeyCode wire_keycode_from_sdl_scancode(int scancode)
{
    switch (scancode) {
        case 4: return wire_key::A;
        case 5: return wire_key::B;
        case 6: return wire_key::C;
        case 7: return wire_key::D;
        case 8: return wire_key::E;
        case 9: return wire_key::F;
        case 10: return wire_key::G;
        case 11: return wire_key::H;
        case 12: return wire_key::I;
        case 13: return wire_key::J;
        case 14: return wire_key::K;
        case 15: return wire_key::L;
        case 16: return wire_key::M;
        case 17: return wire_key::N;
        case 18: return wire_key::O;
        case 19: return wire_key::P;
        case 20: return wire_key::Q;
        case 21: return wire_key::R;
        case 22: return wire_key::S;
        case 23: return wire_key::T;
        case 24: return wire_key::U;
        case 25: return wire_key::V;
        case 26: return wire_key::W;
        case 27: return wire_key::X;
        case 28: return wire_key::Y;
        case 29: return wire_key::Z;
        case 30: return wire_key::Num1;
        case 31: return wire_key::Num2;
        case 32: return wire_key::Num3;
        case 33: return wire_key::Num4;
        case 34: return wire_key::Num5;
        case 35: return wire_key::Num6;
        case 36: return wire_key::Num7;
        case 37: return wire_key::Num8;
        case 38: return wire_key::Num9;
        case 39: return wire_key::Num0;
        case 40: return wire_key::Enter;
        case 41: return wire_key::Esc;
        case 42: return wire_key::Backspace;
        case 43: return wire_key::Tab;
        case 44: return wire_key::Space;
        case 45: return wire_key::Minus;
        case 46: return wire_key::Equal;
        case 47: return wire_key::LeftBrace;
        case 48: return wire_key::RightBrace;
        case 49: return wire_key::Backslash;
        case 50: return wire_key::Backslash;
        case 51: return wire_key::Semicolon;
        case 52: return wire_key::Apostrophe;
        case 53: return wire_key::Grave;
        case 54: return wire_key::Comma;
        case 55: return wire_key::Dot;
        case 56: return wire_key::Slash;
        case 57: return wire_key::CapsLock;
        case 58: return wire_key::F1;
        case 59: return wire_key::F2;
        case 60: return wire_key::F3;
        case 61: return wire_key::F4;
        case 62: return wire_key::F5;
        case 63: return wire_key::F6;
        case 64: return wire_key::F7;
        case 65: return wire_key::F8;
        case 66: return wire_key::F9;
        case 67: return wire_key::F10;
        case 68: return wire_key::F11;
        case 69: return wire_key::F12;
        case 70: return wire_key::SysRq;
        case 71: return wire_key::ScrollLock;
        case 72: return wire_key::Pause;
        case 73: return wire_key::Insert;
        case 74: return wire_key::Home;
        case 75: return wire_key::PageUp;
        case 76: return wire_key::Delete;
        case 77: return wire_key::End;
        case 78: return wire_key::PageDown;
        case 79: return wire_key::Right;
        case 80: return wire_key::Left;
        case 81: return wire_key::Down;
        case 82: return wire_key::Up;
        case 83: return wire_key::NumLock;
        case 84: return wire_key::KpSlash;
        case 85: return wire_key::KpAsterisk;
        case 86: return wire_key::KpMinus;
        case 87: return wire_key::KpPlus;
        case 88: return wire_key::KpEnter;
        case 89: return wire_key::Kp1;
        case 90: return wire_key::Kp2;
        case 91: return wire_key::Kp3;
        case 92: return wire_key::Kp4;
        case 93: return wire_key::Kp5;
        case 94: return wire_key::Kp6;
        case 95: return wire_key::Kp7;
        case 96: return wire_key::Kp8;
        case 97: return wire_key::Kp9;
        case 98: return wire_key::Kp0;
        case 99: return wire_key::KpDot;
        case 101: return wire_key::Compose;
        case 224: return wire_key::LeftCtrl;
        case 225: return wire_key::LeftShift;
        case 226: return wire_key::LeftAlt;
        case 227: return wire_key::LeftMeta;
        case 228: return wire_key::RightCtrl;
        case 229: return wire_key::RightShift;
        case 230: return wire_key::RightAlt;
        case 231: return wire_key::RightMeta;
        default: return wire_key::None;
    }
}

static long rounded_delta(double value)
{
    return value >= 0.0 ? static_cast<long>(value + 0.5) : static_cast<long>(value - 0.5);
}

static std::string motion_command(double dx, double dy)
{
    const long x = rounded_delta(dx);
    const long y = rounded_delta(dy);
    if (x == 0 && y == 0) return {};
    return "MOUSE " + std::to_string(x) + " " + std::to_string(y);
}

std::string raw_motion_command(double dx, double dy)
{
    return motion_command(dx, dy);
}

double mouse_normalization_scale(int)
{
    return 1.0;
}

double clamp_mouse_sensitivity(double sensitivity)
{
    return std::clamp(sensitivity, 0.1, 4.0);
}

std::string normalized_motion_command(double dx, double dy, int, int, double sensitivity)
{
    const double user_scale = clamp_mouse_sensitivity(sensitivity);
    return motion_command(dx * user_scale, dy * user_scale);
}

std::string absolute_pointer_command(int x, int y, int width, int height)
{
    if (width <= 0 || height <= 0) return {};
    x = std::clamp(x, 0, width - 1);
    y = std::clamp(y, 0, height - 1);
    const auto scale = [](int value, int extent) {
        if (extent <= 1) return 0;
        const std::int64_t denominator = static_cast<std::int64_t>(extent - 1);
        return static_cast<int>((static_cast<std::int64_t>(value) * pointer_abs_max + denominator / 2) / denominator);
    };
    return "POINTER " + std::to_string(scale(x, width)) + " " + std::to_string(scale(y, height));
}

std::string video_pointer_command(int x, int y, int client_width, int client_height, int remote_width, int remote_height)
{
    if (client_width <= 0 || client_height <= 0) return {};
    if (remote_width <= 0 || remote_height <= 0) return absolute_pointer_command(x, y, client_width, client_height);

    int view_x = 0;
    int view_y = 0;
    int view_width = client_width;
    int view_height = client_height;
    const std::int64_t client_cross = static_cast<std::int64_t>(client_width) * remote_height;
    const std::int64_t remote_cross = static_cast<std::int64_t>(client_height) * remote_width;
    if (client_cross > remote_cross) {
        view_width = static_cast<int>((static_cast<std::int64_t>(client_height) * remote_width) / remote_height);
        view_width = std::clamp(view_width, 1, client_width);
        view_x = (client_width - view_width) / 2;
    } else if (client_cross < remote_cross) {
        view_height = static_cast<int>((static_cast<std::int64_t>(client_width) * remote_height) / remote_width);
        view_height = std::clamp(view_height, 1, client_height);
        view_y = (client_height - view_height) / 2;
    }
    return absolute_pointer_command(x - view_x, y - view_y, view_width, view_height);
}

bool HeldInputState::press_key(int code)
{
    if (code <= 0) return false;
    return keys_.insert(code).second;
}

bool HeldInputState::release_key(int code)
{
    if (code <= 0) return false;
    return keys_.erase(code) != 0;
}

bool HeldInputState::press_button(int button)
{
    if (button < 1 || button > 3) return false;
    return buttons_.insert(button).second;
}

bool HeldInputState::release_button(int button)
{
    if (button < 1 || button > 3) return false;
    return buttons_.erase(button) != 0;
}

bool HeldInputState::key_down(int code) const
{
    return keys_.find(code) != keys_.end();
}

bool HeldInputState::button_down(int button) const
{
    return buttons_.find(button) != buttons_.end();
}

std::vector<std::string> HeldInputState::release_commands()
{
    std::vector<std::string> out;
    for (int code : keys_) out.push_back("KEY " + std::to_string(code) + " 0");
    for (int button : buttons_) out.push_back("BUTTON " + std::to_string(button) + " 0");
    keys_.clear();
    buttons_.clear();
    return out;
}

ClientControlChord client_control_chord(const HeldInputState &held, int code)
{
    const bool ctrl = held.key_down(wire_key::LeftCtrl) || held.key_down(wire_key::RightCtrl);
    const bool alt = held.key_down(wire_key::LeftAlt) || held.key_down(wire_key::RightAlt);
    const bool shift = held.key_down(wire_key::LeftShift) || held.key_down(wire_key::RightShift);
    if (!(ctrl && alt && shift)) return ClientControlChord::None;
    if (code == wire_key::Q) return ClientControlChord::Quit;
    if (code == wire_key::W) return ClientControlChord::ReleaseCapture;
    return ClientControlChord::None;
}

}
