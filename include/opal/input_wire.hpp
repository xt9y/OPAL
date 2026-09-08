#pragma once

#include <cstdint>

namespace opal {

using WireKeyCode = std::uint16_t;

namespace wire_key {
inline constexpr WireKeyCode None = 0;
inline constexpr WireKeyCode Esc = 1;
inline constexpr WireKeyCode Num1 = 2;
inline constexpr WireKeyCode Num2 = 3;
inline constexpr WireKeyCode Num3 = 4;
inline constexpr WireKeyCode Num4 = 5;
inline constexpr WireKeyCode Num5 = 6;
inline constexpr WireKeyCode Num6 = 7;
inline constexpr WireKeyCode Num7 = 8;
inline constexpr WireKeyCode Num8 = 9;
inline constexpr WireKeyCode Num9 = 10;
inline constexpr WireKeyCode Num0 = 11;
inline constexpr WireKeyCode Minus = 12;
inline constexpr WireKeyCode Equal = 13;
inline constexpr WireKeyCode Backspace = 14;
inline constexpr WireKeyCode Tab = 15;
inline constexpr WireKeyCode Q = 16;
inline constexpr WireKeyCode W = 17;
inline constexpr WireKeyCode E = 18;
inline constexpr WireKeyCode R = 19;
inline constexpr WireKeyCode T = 20;
inline constexpr WireKeyCode Y = 21;
inline constexpr WireKeyCode U = 22;
inline constexpr WireKeyCode I = 23;
inline constexpr WireKeyCode O = 24;
inline constexpr WireKeyCode P = 25;
inline constexpr WireKeyCode LeftBrace = 26;
inline constexpr WireKeyCode RightBrace = 27;
inline constexpr WireKeyCode Enter = 28;
inline constexpr WireKeyCode LeftCtrl = 29;
inline constexpr WireKeyCode A = 30;
inline constexpr WireKeyCode S = 31;
inline constexpr WireKeyCode D = 32;
inline constexpr WireKeyCode F = 33;
inline constexpr WireKeyCode G = 34;
inline constexpr WireKeyCode H = 35;
inline constexpr WireKeyCode J = 36;
inline constexpr WireKeyCode K = 37;
inline constexpr WireKeyCode L = 38;
inline constexpr WireKeyCode Semicolon = 39;
inline constexpr WireKeyCode Apostrophe = 40;
inline constexpr WireKeyCode Grave = 41;
inline constexpr WireKeyCode LeftShift = 42;
inline constexpr WireKeyCode Backslash = 43;
inline constexpr WireKeyCode Z = 44;
inline constexpr WireKeyCode X = 45;
inline constexpr WireKeyCode C = 46;
inline constexpr WireKeyCode V = 47;
inline constexpr WireKeyCode B = 48;
inline constexpr WireKeyCode N = 49;
inline constexpr WireKeyCode M = 50;
inline constexpr WireKeyCode Comma = 51;
inline constexpr WireKeyCode Dot = 52;
inline constexpr WireKeyCode Slash = 53;
inline constexpr WireKeyCode RightShift = 54;
inline constexpr WireKeyCode KpAsterisk = 55;
inline constexpr WireKeyCode LeftAlt = 56;
inline constexpr WireKeyCode Space = 57;
inline constexpr WireKeyCode CapsLock = 58;
inline constexpr WireKeyCode F1 = 59;
inline constexpr WireKeyCode F2 = 60;
inline constexpr WireKeyCode F3 = 61;
inline constexpr WireKeyCode F4 = 62;
inline constexpr WireKeyCode F5 = 63;
inline constexpr WireKeyCode F6 = 64;
inline constexpr WireKeyCode F7 = 65;
inline constexpr WireKeyCode F8 = 66;
inline constexpr WireKeyCode F9 = 67;
inline constexpr WireKeyCode F10 = 68;
inline constexpr WireKeyCode NumLock = 69;
inline constexpr WireKeyCode ScrollLock = 70;
inline constexpr WireKeyCode Kp7 = 71;
inline constexpr WireKeyCode Kp8 = 72;
inline constexpr WireKeyCode Kp9 = 73;
inline constexpr WireKeyCode KpMinus = 74;
inline constexpr WireKeyCode Kp4 = 75;
inline constexpr WireKeyCode Kp5 = 76;
inline constexpr WireKeyCode Kp6 = 77;
inline constexpr WireKeyCode KpPlus = 78;
inline constexpr WireKeyCode Kp1 = 79;
inline constexpr WireKeyCode Kp2 = 80;
inline constexpr WireKeyCode Kp3 = 81;
inline constexpr WireKeyCode Kp0 = 82;
inline constexpr WireKeyCode KpDot = 83;
inline constexpr WireKeyCode F11 = 87;
inline constexpr WireKeyCode F12 = 88;
inline constexpr WireKeyCode KpEnter = 96;
inline constexpr WireKeyCode RightCtrl = 97;
inline constexpr WireKeyCode KpSlash = 98;
inline constexpr WireKeyCode SysRq = 99;
inline constexpr WireKeyCode RightAlt = 100;
inline constexpr WireKeyCode Home = 102;
inline constexpr WireKeyCode Up = 103;
inline constexpr WireKeyCode PageUp = 104;
inline constexpr WireKeyCode Left = 105;
inline constexpr WireKeyCode Right = 106;
inline constexpr WireKeyCode End = 107;
inline constexpr WireKeyCode Down = 108;
inline constexpr WireKeyCode PageDown = 109;
inline constexpr WireKeyCode Insert = 110;
inline constexpr WireKeyCode Delete = 111;
inline constexpr WireKeyCode Pause = 119;
inline constexpr WireKeyCode LeftMeta = 125;
inline constexpr WireKeyCode RightMeta = 126;
inline constexpr WireKeyCode Compose = 127;
}

WireKeyCode wire_keycode_from_sdl_scancode(int scancode);

}
