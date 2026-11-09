// A font for the game UI: its glyphs as signed distance fields in one atlas
// (built once, when it is loaded: never mid-frame), so text is sharp at any
// size from one texture. Loaded from TTF or OTF (TrueType outlines) bytes.
//
// The atlas is RGBA8: white, its alpha the distance (0.5 on the outline,
// more inside); drawn with rhi::UiShade::sdf. It holds the glyphs of
// Basic Latin, Latin-1, Latin Extended-A and a few symbols; any other
// character draws as '?' (or nothing, if the font has no '?'). A block of
// it is solid (fully inside), for rectangles drawn in the same command.
#pragma once

#include "giant/core/types.h"
#include "giant/rhi/rhi.h"

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace giant::ui {

struct Glyph {
    // Where its quad is from the pen on the baseline, at the atlas's pixel
    // size (y down), and where in the atlas (0 to 1).
    float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
    float u0 = 0.0f, v0 = 0.0f, u1 = 0.0f, v1 = 0.0f;
    float advance = 0.0f; // at the atlas's pixel size
    bool empty = true;    // nothing to draw (a space)
};

struct FontDesc {
    float pixel_size = 40.0f; // the size the glyphs are made at (text is drawn at any size)
    int spread = 8;           // pixels the distance reaches out from the outline at that size (outlines reach at most this)
    u32 atlas_width = 1024;
};

class Font {
public:
    ~Font();
    Font(Font&&) noexcept;
    Font& operator=(Font&&) noexcept;
    Font(const Font&) = delete;
    Font& operator=(const Font&) = delete;

    // Null (and `error` set) if the bytes are not a font this can read or
    // its glyphs do not fit an atlas 4096 high. Only for fonts the game
    // ships: stb_truetype trusts its input (a malformed file may crash it).
    static std::unique_ptr<Font> load(std::span<const u8> ttf, std::string& error, const FontDesc& desc = {});
    // The default font (Roboto Medium), built into the engine.
    static std::unique_ptr<Font> load_default(std::string& error, const FontDesc& desc = {});

    // Its glyph (the fallback's if it has none).
    const Glyph& glyph(char32_t c) const;
    bool has(char32_t c) const;
    // How much closer (negative) or further the pen moves between the two, at
    // the pixel size: the font's kern table between the glyphs held, and
    // its GPOS kerning between Basic Latin's.
    float kerning(char32_t a, char32_t b) const;

    float pixel_size() const { return m_desc.pixel_size; }
    // How far the distance reaches out, as a share of the distance's 0 to 1 per pixel at pixel_size.
    float spread() const { return static_cast<float>(m_desc.spread); }
    // At the pixel size: above the baseline, below it (positive), and the line's height.
    float ascent() const { return m_ascent; }
    float descent() const { return m_descent; }
    float line_height() const { return m_line_height; }
    usize glyph_count() const { return m_count; }

    u32 atlas_width() const { return m_width; }
    u32 atlas_height() const { return m_height; }
    const std::vector<u8>& atlas() const { return m_atlas; } // RGBA8, width * height * 4
    // The middle of the solid block (0 to 1).
    float solid_u() const { return m_solid_u; }
    float solid_v() const { return m_solid_v; }

    // The atlas on the GPU, set by whoever uploads it (0: not uploaded; nothing is drawn).
    rhi::TextureId texture = 0;

private:
    Font();
    FontDesc m_desc;
    std::vector<Glyph> m_low;     // by code point, below 0x180 (the fallback's where not held)
    std::vector<bool> m_held;     // by code point, below 0x180
    std::unordered_map<char32_t, Glyph> m_high;
    Glyph m_fallback;
    usize m_count = 0;
    float m_ascent = 0.0f, m_descent = 0.0f, m_line_height = 0.0f, m_scale = 0.0f;
    u32 m_width = 0, m_height = 0;
    std::vector<u8> m_atlas;
    float m_solid_u = 0.0f, m_solid_v = 0.0f;
    std::unordered_map<u64, float> m_kerning; // the font's kerning table (kern), between the glyphs it holds
};

// The default font's TTF bytes.
std::span<const u8> default_font_ttf();

} // namespace giant::ui
