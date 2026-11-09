// Keyboard and mouse input as plain data, so nothing outside platform sees
// SDL. The editor feeds these to its UI; the game will map them to actions.
#pragma once

#include "giant/core/types.h"

#include <string>

namespace giant::platform {

// Keys by their meaning on the user's layout (pressing the key labelled "Z"
// gives Key::z on any layout). Only keys the engine uses so far.
enum class Key : u16 {
    unknown,
    tab, left, right, up, down, page_up, page_down, home, end, insert, del, backspace,
    space, enter, escape,
    apostrophe, comma, minus, period, slash, semicolon, equals, left_bracket, backslash,
    right_bracket, grave,
    n0, n1, n2, n3, n4, n5, n6, n7, n8, n9,
    a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, t, u, v, w, x, y, z,
    f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12,
    left_ctrl, left_shift, left_alt, left_super, right_ctrl, right_shift, right_alt, right_super,
    count
};

enum class MouseButton : u8 { left, right, middle, x1, x2 };

namespace modifier {
constexpr u8 ctrl = 1;
constexpr u8 shift = 2;
constexpr u8 alt = 4;
constexpr u8 super = 8;
} // namespace modifier

struct InputEvent {
    enum class Type : u8 { mouse_move, mouse_button, mouse_wheel, key, text, focus, mouse_left };
    Type type = Type::mouse_move;
    // mouse_move and mouse_button: position in screen points from the
    // top-left of the window. mouse_wheel: scroll amount (+y is away from
    // the user, +x is right).
    float x = 0.0f;
    float y = 0.0f;
    // mouse_move: how far it moved (points), which keeps coming in relative
    // mode (Window::set_relative_mouse), where x, y stop at the window's edges.
    float dx = 0.0f;
    float dy = 0.0f;
    MouseButton button = MouseButton::left; // mouse_button
    Key key = Key::unknown;                 // key
    u8 modifiers = 0;                       // key: modifier:: bits held
    bool down = false;                      // mouse_button, key, focus (gained)
    bool repeat = false;                    // key: down again from being held (the system repeating it)
    std::string text;                       // text: UTF-8 typed text
};

} // namespace giant::platform
