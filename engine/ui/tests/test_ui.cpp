// The game UI's fonts and drawing: the default font's atlas, glyphs and
// kerning; UTF-8; text measured, wrapped and aligned; a canvas's commands
// shared and split as they should be.
#include "giant/ui/canvas.h"

#include "giant_test.h"

#include <chrono>
#include <cmath>
#include <string>

using namespace giant;
using namespace giant::ui;

namespace {

const Font& default_font() {
    static const std::unique_ptr<Font> font = [] {
        std::string error;
        auto f = Font::load_default(error);
        GIANT_CHECK(f != nullptr && error.empty());
        if (f != nullptr) f->texture = 7; // (any id: nothing is drawn here)
        return f;
    }();
    return *font;
}

} // namespace

GIANT_TEST("ui font: the default font's glyphs as distance fields in one atlas, kerned") {
    const auto from = std::chrono::steady_clock::now();
    std::string error;
    const std::unique_ptr<Font> font = Font::load_default(error);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - from).count();
    GIANT_CHECK(font != nullptr && error.empty());
    if (font == nullptr) return;
    std::printf("    default font: %zu glyphs, atlas %ux%u, %.0f ms\n", font->glyph_count(), font->atlas_width(),
                font->atlas_height(), ms);
    GIANT_CHECK(font->glyph_count() > 300);
    GIANT_CHECK(font->atlas_width() == 1024 && font->atlas_height() <= 1024);
    GIANT_CHECK(font->atlas().size() == static_cast<usize>(font->atlas_width()) * font->atlas_height() * 4);
    GIANT_CHECK(font->has(U'A') && font->has(U'é') && font->has(U'Ł') && font->has(U'…') && font->has(U' '));
    GIANT_CHECK(!font->has(U'中'));
    const Glyph& a = font->glyph(U'A');
    GIANT_CHECK(!a.empty && a.advance > 15.0f && a.advance < 40.0f);
    GIANT_CHECK(a.y0 < -20.0f && a.y1 > 0.0f && a.y1 < 10.0f); // up from the baseline, padded by the spread
    GIANT_CHECK(a.u1 > a.u0 && a.v1 > a.v0);
    const Glyph& space = font->glyph(U' ');
    GIANT_CHECK(space.empty && space.advance > 5.0f);
    // A character it lacks draws as '?'.
    const Glyph& q = font->glyph(U'?');
    GIANT_CHECK(font->glyph(U'中').u0 == q.u0 && font->glyph(0x01).advance == q.advance);
    // The distance: 0.5 on the outline, white everywhere; the solid block solid.
    const auto alpha = [&](float u, float v) {
        const u32 x = static_cast<u32>(u * static_cast<float>(font->atlas_width()));
        const u32 y = static_cast<u32>(v * static_cast<float>(font->atlas_height()));
        return font->atlas()[(static_cast<usize>(y) * font->atlas_width() + x) * 4 + 3];
    };
    GIANT_CHECK(alpha(font->solid_u(), font->solid_v()) == 255);
    GIANT_CHECK(font->atlas()[0] == 255 && font->atlas()[3] == 0);
    GIANT_CHECK(alpha(a.u0, a.v0) < 40); // a corner of its box: well outside
    // Kerned: "AV" closer than A and V apart; metrics sensible.
    GIANT_CHECK(font->kerning(U'A', U'V') < 0.0f);
    GIANT_CHECK(font->kerning(U'A', U'A') == 0.0f);
    GIANT_CHECK(font->ascent() > 0.0f && font->descent() > 0.0f &&
                std::fabs(font->ascent() + font->descent() - font->pixel_size()) < 1.0f);
    GIANT_CHECK(font->line_height() >= font->pixel_size());
    // Not a font: refused, saying why.
    const u8 junk[64] = {1, 2, 3};
    GIANT_CHECK(Font::load(junk, error) == nullptr && !error.empty());
    GIANT_CHECK(Font::load(default_font_ttf(), error, FontDesc{4.0f}) == nullptr && !error.empty());
}

GIANT_TEST("ui text: UTF-8 read strictly; measured, wrapped at spaces and aligned") {
    const auto all = [](std::string_view s) {
        std::u32string out;
        for (usize i = 0; i < s.size();) out.push_back(next_code_point(s, i));
        return out;
    };
    GIANT_CHECK(all("Aé€😀") == U"Aé€😀");
    GIANT_CHECK(all("\xC3") == U"�");             // cut short
    GIANT_CHECK(all("\xC0\xAF") == U"�");         // overlong
    GIANT_CHECK(all("\xED\xA0\x80") == U"�");     // a surrogate
    GIANT_CHECK(all("\xFF" "a") == U"�a");
    GIANT_CHECK(all("\xE2\x82" "a") == U"�a");    // a bad continuation: the next byte read again
    GIANT_CHECK(all("\xE2" "a") == U"\uFFFDa");        // cut short by a letter: the letter kept
    GIANT_CHECK(all("a\xF0\x9F") == U"a\uFFFD");      // cut short by the end

    const Font& font = default_font();
    TextStyle style;
    style.size = 20.0f;
    const Vec2 one = measure(font, "Hello", style);
    const float k = 20.0f / font.pixel_size();
    GIANT_CHECK(one.x > 30.0f && one.x < 70.0f);
    GIANT_CHECK(std::fabs(one.y - 20.0f) < 0.5f); // one line: ascent + descent
    GIANT_CHECK(measure(font, "", style).x == 0.0f);
    // Twice the size: twice as wide.
    style.size = 40.0f;
    GIANT_CHECK(std::fabs(measure(font, "Hello", style).x - 2.0f * one.x) < 0.01f);
    style.size = 20.0f;
    // Line breaks: two lines.
    const Vec2 two = measure(font, "Hello\nHello", style);
    GIANT_CHECK(two.x == one.x && std::fabs(two.y - (one.y + font.line_height() * k)) < 0.01f);
    // Wrapped at spaces to the width: three words in a box two words wide take two lines.
    const float word = measure(font, "word", style).x;
    const float space = font.glyph(U' ').advance * k;
    style.wrap = 2.0f * word + space + 1.0f;
    const Vec2 wrapped = measure(font, "word word word", style);
    GIANT_CHECK(std::fabs(wrapped.y - two.y) < 0.01f);
    GIANT_CHECK(wrapped.x <= style.wrap && wrapped.x > 2.0f * word);
    // A word wider than the wrap is broken (every line holds a letter at least).
    style.wrap = word * 0.5f;
    const Vec2 broken = measure(font, "word", style);
    GIANT_CHECK(broken.y > two.y - 0.01f && broken.x <= word * 0.5f + 0.01f);
    // Spaces: leading ones are no place to break, a run counts once, ending ones take no width.
    style.wrap = word * 0.5f;
    GIANT_CHECK(std::fabs(measure(font, " word", style).y - broken.y) < 0.01f);
    style.wrap = 0.0f;
    GIANT_CHECK(measure(font, "word   ", style).x == word);
    GIANT_CHECK(measure(font, "word   \nword", style).x == word);
    style.wrap = 2.0f * word + space + 1.0f;
    const Vec2 runs = measure(font, "word word" + std::string(30, ' ') + "word", style);
    GIANT_CHECK(std::fabs(runs.y - two.y) < 0.01f && runs.x <= style.wrap);
    // A carriage return is nothing, a tab a space.
    GIANT_CHECK(measure(font, "word\r\nword", style) == measure(font, "word\nword", style));
    GIANT_CHECK(measure(font, "a\tb", style) == measure(font, "a b", style));
    style.wrap = word * 0.5f;
    GIANT_CHECK(measure(font, "\r word", style) == measure(font, " word", style));
    style.wrap = 0.0f;
    style.wrap = 1.0f; // narrower than any letter: one a line
    GIANT_CHECK(std::fabs(measure(font, "abc", style).y - (one.y + 2.0f * font.line_height() * k)) < 0.01f);

    // Aligned: centred about the point, right to it; with a wrap, within it.
    Canvas c;
    c.begin(800.0f, 600.0f, font);
    TextStyle centred;
    centred.size = 20.0f;
    centred.align = Align::center;
    c.text({400.0f, 100.0f}, "Hello", centred);
    const auto span = [&](usize from) {
        float lo = 1e9f, hi = -1e9f;
        const rhi::UiDrawList l = c.list();
        for (usize i = from; i < l.vertices.size(); ++i) {
            lo = std::min(lo, l.vertices[i].x);
            hi = std::max(hi, l.vertices[i].x);
        }
        return std::make_pair(lo, hi);
    };
    auto [lo, hi] = span(0);
    GIANT_CHECK(std::fabs(0.5f * (lo + hi) - 400.0f) < 3.0f);
    const usize before = c.vertices();
    TextStyle right = centred;
    right.align = Align::right;
    c.text({400.0f, 200.0f}, "Hello", right);
    std::tie(lo, hi) = span(before);
    GIANT_CHECK(hi < 404.0f && hi > 390.0f); // (the last glyph's padding reaches a little past)
}

GIANT_TEST("ui canvas: rectangles and text share a command; images, outlines and clips split them") {
    const Font& font = default_font();
    Canvas c;
    c.begin(1920.0f, 1080.0f, font);
    c.rect({10.0f, 10.0f, 100.0f, 20.0f}, rgba(255, 0, 0));
    TextStyle style;
    c.text({10.0f, 40.0f}, "Hi there", style);
    c.line({0.0f, 0.0f}, {100.0f, 100.0f}, 2.0f, k_white);
    GIANT_CHECK(c.commands() == 1);
    rhi::UiDrawList l = c.list();
    GIANT_CHECK(l.commands[0].shade == rhi::UiShade::sdf && l.commands[0].texture == font.texture);
    GIANT_CHECK(l.commands[0].index_count == l.indices.size() && l.indices.size() % 6 == 0);
    GIANT_CHECK(l.vertices.size() == 4 * (1 + 7 + 1)); // the rect, 7 letters (no space), the line
    // A rectangle's corners sample the solid block.
    GIANT_CHECK(l.vertices[0].u == font.solid_u() && l.vertices[2].x == 110.0f && l.vertices[2].y == 30.0f);
    // An image: a plain command; back to the atlas: another.
    c.image({0.0f, 0.0f, 64.0f, 64.0f}, 9);
    c.rect({0.0f, 0.0f, 1.0f, 1.0f}, k_white);
    GIANT_CHECK(c.commands() == 3);
    l = c.list();
    GIANT_CHECK(l.commands[1].shade == rhi::UiShade::plain && l.commands[1].texture == 9);
    // Outlined text: its own command (another outline), in distance units.
    style.outline = 2.0f;
    style.size = 40.0f;
    c.text({0.0f, 300.0f}, "Out", style);
    GIANT_CHECK(c.commands() == 4);
    l = c.list();
    GIANT_CHECK(std::fabs(l.commands[3].outline - 2.0f / (40.0f / font.pixel_size()) / (2.0f * font.spread())) < 1e-5f);
    // A shadow is drawn first, under it (one more command: no outline).
    style.outline = 0.0f;
    style.shadow = 2.0f;
    const usize was = c.commands();
    c.text({0.0f, 400.0f}, "Sh", style);
    GIANT_CHECK(c.commands() == was + 1); // the shadow and the text share the plain sdf shade
    // Clipped: a command of its own, its rectangle the clip's within the screen's.
    c.push_clip({100.0f, 100.0f, 5000.0f, 50.0f});
    c.rect({0.0f, 0.0f, 10.0f, 10.0f}, k_white);
    l = c.list();
    GIANT_CHECK(l.commands.back().clip_min_x == 100.0f && l.commands.back().clip_max_x == 1920.0f &&
                l.commands.back().clip_max_y == 150.0f);
    c.push_clip({0.0f, 0.0f, 10.0f, 10.0f}); // outside the last: nothing drawn
    const usize v = c.vertices();
    c.rect({0.0f, 0.0f, 10.0f, 10.0f}, k_white);
    GIANT_CHECK(c.vertices() == v);
    c.pop_clip();
    c.pop_clip();
    c.pop_clip(); // (the screen's stays)
    c.rect({0.0f, 0.0f, 10.0f, 10.0f}, k_white);
    GIANT_CHECK(c.vertices() == v + 4 && c.list().commands.back().clip_max_x == 1920.0f);
    // Not finite, or nothing to draw: nothing.
    c.rect({NAN, 0.0f, 10.0f, 10.0f}, k_white);
    c.rect({0.0f, 0.0f, -1.0f, 10.0f}, k_white);
    c.line({0.0f, 0.0f}, {0.0f, 0.0f}, 2.0f, k_white);
    style.size = INFINITY;
    c.text({0.0f, 0.0f}, "x", style);
    GIANT_CHECK(c.vertices() == v + 4);
    // A font not on the GPU yet: nothing drawn.
    std::string error;
    const std::unique_ptr<Font> other = Font::load_default(error);
    GIANT_CHECK(other != nullptr);
    if (other != nullptr) {
        Canvas d;
        d.begin(100.0f, 100.0f, *other);
        d.rect({0.0f, 0.0f, 10.0f, 10.0f}, k_white);
        d.text({0.0f, 0.0f}, "x", {});
        GIANT_CHECK(d.vertices() == 0 && d.commands() == 0);
    }
    // A new frame starts empty.
    c.begin(10.0f, 10.0f, font);
    GIANT_CHECK(c.vertices() == 0 && c.commands() == 0);
}
