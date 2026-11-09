// Drawing the game UI: rectangles, lines, images and text into one draw
// list for rhi::CommandList::draw_ui, in pixels (y down), a frame at a
// time. Text and rectangles share the font atlas (rhi::UiShade::sdf), so a
// HUD is a few draw calls; consecutive draws with the same texture, shade
// and clip share one command.
#pragma once

#include "giant/ui/font.h"

#include <string_view>
#include <vector>

namespace giant::ui {

struct Vec2 {
    float x = 0.0f, y = 0.0f;
    bool operator==(const Vec2&) const = default;
};

struct Rect {
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
    bool operator==(const Rect&) const = default;
};

// RGBA8, red in the lowest byte (as rhi::UiVertex has it), gamma space.
constexpr u32 rgba(u8 r, u8 g, u8 b, u8 a = 255) {
    return static_cast<u32>(r) | (static_cast<u32>(g) << 8) | (static_cast<u32>(b) << 16) | (static_cast<u32>(a) << 24);
}
constexpr u32 k_white = rgba(255, 255, 255);
constexpr u32 k_black = rgba(0, 0, 0);

enum class Align : u8 { left, center, right };

struct TextStyle {
    const Font* font = nullptr; // null: the canvas's
    float size = 24.0f;         // pixels from the tallest letters' top to the lowest's bottom (a font's size)
    u32 color = k_white;
    float outline = 0.0f;       // pixels round each letter (at most what the font's spread allows)
    u32 outline_color = k_black;
    float shadow = 0.0f;        // pixels down and right a shadow is drawn (0: none)
    u32 shadow_color = rgba(0, 0, 0, 160);
    Align align = Align::left;  // of each line within `wrap` (or about the point, with no wrap)
    float wrap = 0.0f;          // wrapped at spaces to this width (0: only at line breaks)
    float line_spacing = 1.0f;
};

// The size text takes, as Canvas::text lays it out.
Vec2 measure(const Font& font, std::string_view utf8, const TextStyle& style);

// The next code point in UTF-8 from `at` (moved past it); U+FFFD for bytes that are not UTF-8.
char32_t next_code_point(std::string_view text, usize& at);

class Canvas {
public:
    // A new frame `width` by `height` pixels; text with no font of its own
    // and rectangles use `font` (its texture must be set: else nothing is drawn).
    void begin(float width, float height, const Font& font);

    void rect(const Rect& r, u32 color);
    void frame(const Rect& r, float thickness, u32 color); // its edges only
    void line(Vec2 a, Vec2 b, float thickness, u32 color);
    // An rgba8_unorm texture, `uv` the part of it drawn.
    void image(const Rect& r, rhi::TextureId texture, const Rect& uv = {0.0f, 0.0f, 1.0f, 1.0f}, u32 tint = k_white);
    // Its first line's top at `at.y`; `at.x` its left (left), middle
    // (center) or right (right), or the box's with a wrap. Returns its size.
    Vec2 text(Vec2 at, std::string_view utf8, const TextStyle& style);

    // Only inside `r` (and the clip already pushed) until popped.
    void push_clip(const Rect& r);
    void pop_clip();

    float width() const { return m_width; }
    float height() const { return m_height; }
    // What to draw (valid until the next begin or draw).
    rhi::UiDrawList list() const { return {m_vertices, m_indices, m_commands}; }
    usize vertices() const { return m_vertices.size(); }
    usize commands() const { return m_commands.size(); }

    // At most this many vertices a frame (the rest not drawn).
    static constexpr usize k_max_vertices = 1 << 18;

private:
    struct Shade {
        rhi::TextureId texture = 0;
        rhi::UiShade shade = rhi::UiShade::plain;
        float outline = 0.0f;
        u32 outline_color = 0;
        bool operator==(const Shade&) const = default;
    };
    // Room for `vertices` more under `shade` and the clip now (false: none, or no texture).
    bool reserve(const Shade& shade, usize vertices);
    void quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, u32 color);
    void glyphs(Vec2 at, std::string_view utf8, const TextStyle& style, const Font& font, Vec2 offset, u32 color);

    const Font* m_font = nullptr;
    float m_width = 0.0f, m_height = 0.0f;
    std::vector<rhi::UiVertex> m_vertices;
    std::vector<u32> m_indices;
    std::vector<rhi::UiDrawCommand> m_commands;
    std::vector<Rect> m_clips;
    Shade m_last;
};

} // namespace giant::ui
