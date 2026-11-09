#include "giant/ui/canvas.h"

#include <algorithm>
#include <cmath>

namespace giant::ui {

namespace {

constexpr char32_t k_bad = 0xFFFD;

bool finite(float v) { return std::isfinite(v); }

// What a character is laid out and drawn as: a tab as a space, a carriage return as nothing (0).
char32_t shown(char32_t c) { return c == U'\t' ? U' ' : c == U'\r' ? 0 : c; }

// One line of laid out text: its bytes and how wide it is (at the size drawn).
struct Line {
    usize begin = 0, end = 0;
    float width = 0.0f;
};

// Lines as Canvas::text draws them: broken at '\n' and, with a wrap, at the
// last run of spaces that keeps a line within it (a word wider than the
// wrap broken where it no longer fits). Spaces ending a line take no width
// (they are not drawn either: they have no pixels).
template <typename F>
void lay_out(const Font& font, std::string_view text, float k, float wrap, F&& on_line) {
    const auto step_of = [&](char32_t prev, char32_t c) {
        return (font.glyph(c).advance + (prev != 0 ? font.kerning(prev, c) : 0.0f)) * k;
    };
    usize begin = 0;
    float width = 0.0f;
    char32_t prev = 0;
    // The run of spaces the line ends in, if any: where it starts and the width before it.
    bool trailing = false;
    usize trail_from = 0;
    float trail_width = 0.0f;
    // The last run of spaces after something (where the line may break): where it starts,
    // the width before it, and where the next line would start (after the run).
    bool breakable = false;
    usize break_at = 0, after_break = 0;
    float width_at_break = 0.0f;
    const auto emit = [&](usize end, float w) {
        if (trailing) { // (the spaces ending it dropped)
            end = std::min(end, trail_from);
            w = trail_width;
        }
        on_line(Line{begin, std::max(end, begin), w});
    };
    const auto restart = [&](usize from) {
        begin = from;
        width = 0.0f;
        prev = 0;
        trailing = breakable = false;
    };
    usize at = 0;
    while (at < text.size()) {
        const usize here = at;
        const char32_t c = shown(next_code_point(text, at));
        if (c == 0) {
            if (here == begin) begin = at; // (nothing yet: the line starts after it)
            continue;
        }
        if (c == U'\n') {
            emit(here, width);
            restart(at);
            continue;
        }
        if (c == U' ') {
            if (!trailing) {
                trailing = true;
                trail_from = here;
                trail_width = width;
                if (here > begin) { // (spaces leading a line are no place to break)
                    breakable = true;
                    break_at = here;
                    width_at_break = width;
                }
            }
            if (breakable && break_at == trail_from) after_break = at;
            width += step_of(prev, c);
            prev = c;
            continue;
        }
        const float step = step_of(prev, c);
        // (A line of only leading spaces is not broken: the letter joins it.)
        if (wrap > 0.0f && width + step > wrap && here > begin && !(trailing && trail_from == begin)) {
            if (breakable) {
                trailing = false;
                on_line(Line{begin, break_at, width_at_break});
                // What followed the spaces carries over, and this letter is looked at again.
                const usize from = after_break;
                restart(from);
                for (usize i = from; i < here;) {
                    const char32_t d = shown(next_code_point(text, i));
                    if (d == 0) continue;
                    width += step_of(prev, d);
                    prev = d;
                }
                at = here;
                continue;
            }
            emit(here, width);
            restart(here);
            width = step_of(0, c); // (at least one letter a line)
            prev = c;
            continue;
        }
        trailing = false;
        width += step;
        prev = c;
    }
    emit(text.size(), width);
}

float scale_of(const Font& font, const TextStyle& style) {
    return finite(style.size) && style.size > 0.0f ? std::min(style.size, 4096.0f) / font.pixel_size() : 0.0f;
}

} // namespace

char32_t next_code_point(std::string_view text, usize& at) {
    const auto byte = [&](usize i) { return static_cast<u8>(text[i]); };
    const u8 b0 = byte(at);
    if (b0 < 0x80) {
        ++at;
        return b0;
    }
    usize n = 0;
    char32_t c = 0;
    if ((b0 & 0xE0) == 0xC0) {
        n = 1;
        c = b0 & 0x1F;
    } else if ((b0 & 0xF0) == 0xE0) {
        n = 2;
        c = b0 & 0x0F;
    } else if ((b0 & 0xF8) == 0xF0) {
        n = 3;
        c = b0 & 0x07;
    } else {
        ++at;
        return k_bad;
    }
    for (usize i = 1; i <= n; ++i) {
        if (at + i >= text.size()) { // (cut short)
            at += i;
            return k_bad;
        }
        const u8 b = byte(at + i);
        if ((b & 0xC0) != 0x80) {
            at += i;
            return k_bad;
        }
        c = (c << 6) | (b & 0x3F);
    }
    at += n + 1;
    // Overlong forms, surrogates and past U+10FFFF are not UTF-8.
    static constexpr char32_t k_least[4] = {0, 0x80, 0x800, 0x10000};
    if (c < k_least[n] || (c >= 0xD800 && c <= 0xDFFF) || c > 0x10FFFF) return k_bad;
    return c;
}

Vec2 measure(const Font& font, std::string_view utf8, const TextStyle& style) {
    const float k = scale_of(font, style);
    if (k <= 0.0f) return {};
    const float wrap = finite(style.wrap) ? std::max(style.wrap, 0.0f) : 0.0f;
    const float spacing = finite(style.line_spacing) ? std::clamp(style.line_spacing, 0.0f, 16.0f) : 1.0f;
    float widest = 0.0f;
    usize lines = 0;
    lay_out(font, utf8, k, wrap, [&](const Line& l) {
        widest = std::max(widest, l.width);
        ++lines;
    });
    const float line = font.line_height() * k * spacing;
    // The last line as tall as the font (not its spacing).
    return {widest, static_cast<float>(lines - 1) * line + (font.ascent() + font.descent()) * k};
}

void Canvas::begin(float width, float height, const Font& font) {
    m_font = &font;
    m_width = finite(width) ? std::max(width, 0.0f) : 0.0f;
    m_height = finite(height) ? std::max(height, 0.0f) : 0.0f;
    m_vertices.clear();
    m_indices.clear();
    m_commands.clear();
    m_clips.clear();
    m_clips.push_back({0.0f, 0.0f, m_width, m_height});
    m_last = {};
}

void Canvas::push_clip(const Rect& r) {
    const Rect& c = m_clips.empty() ? Rect{} : m_clips.back();
    if (!(finite(r.x) && finite(r.y) && finite(r.w) && finite(r.h))) {
        m_clips.push_back({c.x, c.y, 0.0f, 0.0f});
        return;
    }
    const float x0 = std::max(c.x, r.x), y0 = std::max(c.y, r.y);
    const float x1 = std::min(c.x + c.w, r.x + std::max(r.w, 0.0f)), y1 = std::min(c.y + c.h, r.y + std::max(r.h, 0.0f));
    m_clips.push_back({x0, y0, std::max(x1 - x0, 0.0f), std::max(y1 - y0, 0.0f)});
}

void Canvas::pop_clip() {
    if (m_clips.size() > 1) m_clips.pop_back();
}

bool Canvas::reserve(const Shade& shade, usize vertices) {
    if (shade.texture == 0 || m_clips.empty() || m_vertices.size() + vertices > k_max_vertices) return false;
    const Rect& c = m_clips.back();
    if (c.w <= 0.0f || c.h <= 0.0f) return false;
    const bool same = !m_commands.empty() && m_last == shade && m_commands.back().clip_min_x == c.x &&
                      m_commands.back().clip_min_y == c.y && m_commands.back().clip_max_x == c.x + c.w &&
                      m_commands.back().clip_max_y == c.y + c.h;
    if (!same) {
        rhi::UiDrawCommand cmd;
        cmd.first_index = static_cast<u32>(m_indices.size());
        cmd.clip_min_x = c.x;
        cmd.clip_min_y = c.y;
        cmd.clip_max_x = c.x + c.w;
        cmd.clip_max_y = c.y + c.h;
        cmd.texture = shade.texture;
        cmd.shade = shade.shade;
        cmd.outline = shade.outline;
        cmd.outline_color = shade.outline_color;
        m_commands.push_back(cmd);
        m_last = shade;
    }
    return true;
}

void Canvas::quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, u32 color) {
    const u32 base = static_cast<u32>(m_vertices.size());
    m_vertices.push_back({x0, y0, u0, v0, color});
    m_vertices.push_back({x1, y0, u1, v0, color});
    m_vertices.push_back({x1, y1, u1, v1, color});
    m_vertices.push_back({x0, y1, u0, v1, color});
    for (const u32 i : {0u, 1u, 2u, 0u, 2u, 3u}) m_indices.push_back(base + i);
    m_commands.back().index_count += 6;
}

void Canvas::rect(const Rect& r, u32 color) {
    if (m_font == nullptr || !(finite(r.x) && finite(r.y) && finite(r.w) && finite(r.h)) || r.w <= 0.0f || r.h <= 0.0f)
        return;
    if (!reserve({m_font->texture, rhi::UiShade::sdf, 0.0f, 0}, 4)) return;
    const float u = m_font->solid_u(), v = m_font->solid_v();
    quad(r.x, r.y, r.x + r.w, r.y + r.h, u, v, u, v, color);
}

void Canvas::frame(const Rect& r, float thickness, u32 color) {
    if (!(finite(thickness) && thickness > 0.0f)) return;
    const float t = std::min({thickness, 0.5f * r.w, 0.5f * r.h});
    rect({r.x, r.y, r.w, t}, color);
    rect({r.x, r.y + r.h - t, r.w, t}, color);
    rect({r.x, r.y + t, t, r.h - 2.0f * t}, color);
    rect({r.x + r.w - t, r.y + t, t, r.h - 2.0f * t}, color);
}

void Canvas::line(Vec2 a, Vec2 b, float thickness, u32 color) {
    if (m_font == nullptr || !(finite(a.x) && finite(a.y) && finite(b.x) && finite(b.y) && finite(thickness)) ||
        thickness <= 0.0f)
        return;
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len <= 0.0f) return;
    if (!reserve({m_font->texture, rhi::UiShade::sdf, 0.0f, 0}, 4)) return;
    const float nx = -dy / len * 0.5f * thickness, ny = dx / len * 0.5f * thickness;
    const float u = m_font->solid_u(), v = m_font->solid_v();
    const u32 base = static_cast<u32>(m_vertices.size());
    m_vertices.push_back({a.x + nx, a.y + ny, u, v, color});
    m_vertices.push_back({b.x + nx, b.y + ny, u, v, color});
    m_vertices.push_back({b.x - nx, b.y - ny, u, v, color});
    m_vertices.push_back({a.x - nx, a.y - ny, u, v, color});
    for (const u32 i : {0u, 1u, 2u, 0u, 2u, 3u}) m_indices.push_back(base + i);
    m_commands.back().index_count += 6;
}

void Canvas::image(const Rect& r, rhi::TextureId texture, const Rect& uv, u32 tint) {
    if (!(finite(r.x) && finite(r.y) && finite(r.w) && finite(r.h) && finite(uv.x) && finite(uv.y) && finite(uv.w) &&
          finite(uv.h)) ||
        r.w <= 0.0f || r.h <= 0.0f)
        return;
    if (!reserve({texture, rhi::UiShade::plain, 0.0f, 0}, 4)) return;
    quad(r.x, r.y, r.x + r.w, r.y + r.h, uv.x, uv.y, uv.x + uv.w, uv.y + uv.h, tint);
}

Vec2 Canvas::text(Vec2 at, std::string_view utf8, const TextStyle& style) {
    const Font* font = style.font != nullptr ? style.font : m_font;
    if (font == nullptr || !finite(at.x) || !finite(at.y)) return {};
    const Vec2 size = measure(*font, utf8, style);
    if (size.x <= 0.0f && size.y <= 0.0f) return size;
    // (A shadow only with room for the text too: the text is never the one dropped.)
    if (finite(style.shadow) && style.shadow > 0.0f && m_vertices.size() + 8 * utf8.size() <= k_max_vertices) {
        glyphs(at, utf8, style, *font, {std::min(style.shadow, 64.0f), std::min(style.shadow, 64.0f)}, style.shadow_color);
    }
    glyphs(at, utf8, style, *font, {}, style.color);
    return size;
}

void Canvas::glyphs(Vec2 at, std::string_view utf8, const TextStyle& style, const Font& font, Vec2 offset, u32 color) {
    const float k = scale_of(font, style);
    if (k <= 0.0f) return;
    // The outline in distance units: pixels on screen to pixels at the atlas's size, over the distance's reach.
    float outline = 0.0f;
    if (finite(style.outline) && style.outline > 0.0f && offset.x == 0.0f) {
        // (At most a pixel short of the spread: else the glyph's box shows.)
        const float pixel = 1.0f / k / (2.0f * font.spread());
        outline = std::clamp(style.outline / k / (2.0f * font.spread()), 0.0f, std::max(0.5f - pixel, 0.0f));
    }
    const Shade shade{font.texture, rhi::UiShade::sdf, outline, outline > 0.0f ? style.outline_color : 0u};
    const float wrap = finite(style.wrap) ? std::max(style.wrap, 0.0f) : 0.0f;
    const float spacing = finite(style.line_spacing) ? std::clamp(style.line_spacing, 0.0f, 16.0f) : 1.0f;
    const float line_height = font.line_height() * k * spacing;
    usize index = 0;
    lay_out(font, utf8, k, wrap, [&](const Line& l) {
        float x = at.x + offset.x;
        const float box = wrap > 0.0f ? wrap : 0.0f;
        // (With no wrap, `at.x` is the line's middle or right.)
        if (style.align == Align::center) x += 0.5f * (box - l.width);
        if (style.align == Align::right) x += box - l.width;
        const float baseline = std::round(at.y + offset.y + font.ascent() * k + static_cast<float>(index) * line_height);
        ++index;
        char32_t prev = 0;
        for (usize i = l.begin; i < l.end;) {
            const char32_t c = shown(next_code_point(utf8, i));
            if (c == 0) continue;
            if (prev != 0) x += font.kerning(prev, c) * k;
            const Glyph& g = font.glyph(c);
            if (!g.empty && reserve(shade, 4)) {
                quad(x + g.x0 * k, baseline + g.y0 * k, x + g.x1 * k, baseline + g.y1 * k, g.u0, g.v0, g.u1, g.v1, color);
            }
            x += g.advance * k;
            prev = c;
        }
    });
}

} // namespace giant::ui
